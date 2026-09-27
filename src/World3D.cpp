#include "Jobs.h"
#include "Loading.h"
#include "Profile.h"
#include "Profile.h"
#include "Audio.h"
#include "HostProtocol.h"
#include "Render3D.h"
#include "WorldTextures.h"

#include <algorithm>

namespace ludifex
{
namespace
{

// Calls a handler through a copy of it. A handler may add actors, which can
// grow the array it is stored in and move it mid-call; or it may replace
// itself. Either would destroy the running handler; the copy keeps running
// instead.
template <typename Handler, typename Info>
void CallHandler(const Handler& handler, const Info& info)
{
    if (handler)
    {
        const Handler running = handler;
        running(info);
    }
}

using detail::ActorRecord3;
using detail::ShapeKind;
using detail::World3DState;

const std::string g_EmptyName;

Transform3 ReadTransform(b3BodyId body, const Vec3& scale)
{
    Transform3 transform;
    transform.Position = detail::FromB3Pos(b3Body_GetPosition(body));
    transform.Rotation = detail::FromB3(b3Body_GetRotation(body));
    transform.Scale = scale;
    return transform;
}

// Allocates a slot from the free list or grows the pool, bumps the generation,
// and returns the handle. Generation 0 is reserved for "never used", so a
// freshly allocated record always lands on 1 or higher.
ActorId RegisterActor(World3DState& state, b3BodyId body, const Vec3& scale, const std::string& name,
                      ShapeKind shape, bool isSensor = false)
{
    uint32_t index;
    if (!state.FreeIndices.empty())
    {
        index = state.FreeIndices.back();
        state.FreeIndices.pop_back();
    }
    else
    {
        index = static_cast<uint32_t>(state.Actors.size());
        state.Actors.emplace_back();
    }

    ActorRecord3& record = state.Actors[index];
    ++record.Generation;
    if (record.Generation == 0)
    {
        record.Generation = 1;
    }

    record.Alive = true;
    record.Body = body;
    record.Scale = scale;
    record.Name = name;
    record.Shape = shape;
    record.ModelIndex = -1;
    // Sensors start hidden. SetVisible(true) shows one, which helps when
    // tuning trigger volumes.
    // Static and dynamic bodies get different default colours, so a scene is
    // readable before anything has been styled.
    record.Look.Reset((b3Body_GetType(body) == b3_staticBody) ? Color{ 0.28f, 0.31f, 0.38f, 1.0f }
                                                              : Color{ 0.89f, 0.64f, 0.31f, 1.0f });
    record.Look.Visible = !isSensor;
    record.OnCollided = {};
    record.IsSensor = isSensor;
    record.OnEntered = {};
    record.OnExited = {};

    // Half the diagonal of the scaled bounds encloses every primitive whatever
    // its rotation, so culling never has to re-measure when an actor spins.
    record.BoundingRadius =
        0.5f * std::sqrt(scale.X * scale.X + scale.Y * scale.Y + scale.Z * scale.Z);

    // Contact events hand back a shape, not an actor, so the body carries the
    // index needed to get back here.
    b3Body_SetUserData(body, detail::PackActorIndex(index));
    record.Current = ReadTransform(body, scale);
    record.Previous = record.Current;

    ++state.LiveCount;

    return ActorId{ index, record.Generation };
}

b3BodyId CreateBody(World3DState& state, const Vec3& position, const Quat& rotation,
                    BodyType type, bool isBullet)
{
    b3BodyDef bodyDef = b3DefaultBodyDef();
    bodyDef.type = detail::ToB3BodyType(type);
    bodyDef.position = detail::ToB3Pos(position);
    bodyDef.rotation = detail::ToB3(rotation);
    bodyDef.isBullet = isBullet;
    bodyDef.enableSleep = state.Config.EnableSleep;
    return b3CreateBody(state.World, &bodyDef);
}

// Box3D hands over whole tasks rather than ranges, and does its own splitting.
void* EnqueueTask(b3TaskCallback* task, void* taskContext, void* userContext, const char* taskName)
{
    (void)taskName;
    auto* jobs = static_cast<detail::JobSystem*>(userContext);
    return jobs->SubmitTask(task, taskContext);
}

void FinishTask(void* userTask, void* userContext)
{
    auto* jobs = static_cast<detail::JobSystem*>(userContext);
    jobs->Wait(userTask);
}

b3ShapeDef MakeShapeDef(float density, float friction, float restitution, bool isSensor = false)
{
    b3ShapeDef shapeDef = b3DefaultShapeDef();
    shapeDef.density = density;
    shapeDef.baseMaterial.friction = friction;
    shapeDef.baseMaterial.restitution = restitution;

    // Hit events carry a contact point, a normal, and an approach speed, which
    // is what a game needs to scale a sound or an effect to the impact.
    shapeDef.enableHitEvents = true;

    // A sensor reports overlaps and never pushes; everything else keeps its
    // solid behaviour but stays visible to sensors, so any actor can trip one
    // without having been marked in advance.
    shapeDef.isSensor = isSensor;
    shapeDef.enableSensorEvents = true;

    return shapeDef;
}

// Recovers the actor a shape belongs to, or an invalid handle when the body is
// gone or was never registered.
} // namespace

namespace detail
{

ActorId ActorIdForShape(World3DState& state, b3ShapeId shapeId)
{
    if (!b3Shape_IsValid(shapeId))
    {
        return ActorId{};
    }

    const b3BodyId body = b3Shape_GetBody(shapeId);
    if (!b3Body_IsValid(body))
    {
        return ActorId{};
    }

    uint32_t index = 0;
    if (!detail::UnpackActorIndex(b3Body_GetUserData(body), index))
    {
        return ActorId{};
    }

    if (index >= state.Actors.size() || !state.Actors[index].Alive)
    {
        return ActorId{};
    }

    return ActorId{ index, state.Actors[index].Generation };
}

Actor3D MakeActor(World3DState* state, const ActorId& id)
{
    Actor3D actor;
    if (Resolve(state, id, "MakeActor") != nullptr)
    {
        actor.m_State = state;
        actor.m_Id = id;
    }
    return actor;
}

} // namespace detail

namespace
{

// Reads the step's contact events and delivers them. Called after the solver
// has finished, never from inside it, so a handler may create and destroy
// actors freely.
void DispatchCollisions(World3DState& state, World3D& world)
{
    const bool wantsEvents = static_cast<bool>(state.OnAnyCollision);

    const b3ContactEvents events = b3World_GetContactEvents(state.World);

    for (int index = 0; index < events.hitCount; ++index)
    {
        const b3ContactHitEvent& hit = events.hitEvents[index];

        if (hit.approachSpeed < state.CollisionThreshold)
        {
            continue;
        }

        const ActorId idA = detail::ActorIdForShape(state, hit.shapeIdA);
        const ActorId idB = detail::ActorIdForShape(state, hit.shapeIdB);

        if (!idA.IsValid() || !idB.IsValid())
        {
            continue;
        }

        const bool wantsA = static_cast<bool>(state.Actors[idA.Index].OnCollided);
        const bool wantsB = static_cast<bool>(state.Actors[idB.Index].OnCollided);

        if (!wantsA && !wantsB && !wantsEvents)
        {
            continue;
        }

        CollisionInfo info;
        info.Point = detail::FromB3Pos(hit.point);
        info.Normal = detail::FromB3(hit.normal);
        info.ImpactSpeed = hit.approachSpeed;

        // Each side is reported from its own point of view, so a handler can
        // always read Self as "me" and Other as "the thing I hit".
        if (wantsA)
        {
            info.Self = world.GetActor(idA);
            info.Other = world.GetActor(idB);
            CallHandler(state.Actors[idA.Index].OnCollided, info);
        }

        if (wantsB)
        {
            info.Self = world.GetActor(idB);
            info.Other = world.GetActor(idA);
            info.Normal = Vec3{ -info.Normal.X, -info.Normal.Y, -info.Normal.Z };
            CallHandler(state.Actors[idB.Index].OnCollided, info);
        }

        if (wantsEvents)
        {
            info.Self = world.GetActor(idA);
            info.Other = world.GetActor(idB);
            info.Normal = detail::FromB3(hit.normal);
            CallHandler(state.OnAnyCollision, info);
        }
    }
}

// Overlap notifications, dispatched under the same rule as collisions: after
// the step, never inside it. An end event can name a shape that no longer
// exists, so both sides are resolved through the handle system, and a
// destroyed actor does not report leaving.
void DispatchSensors(World3DState& state, World3D& world)
{
    const b3SensorEvents events = b3World_GetSensorEvents(state.World);

    for (int index = 0; index < events.beginCount; ++index)
    {
        const b3SensorBeginTouchEvent& begin = events.beginEvents[index];

        const ActorId sensorId = detail::ActorIdForShape(state, begin.sensorShapeId);
        const ActorId visitorId = detail::ActorIdForShape(state, begin.visitorShapeId);

        if (!sensorId.IsValid() || !visitorId.IsValid())
        {
            continue;
        }

        const bool hasOwn = static_cast<bool>(state.Actors[sensorId.Index].OnEntered);
        if (!hasOwn && !state.OnAnyEnter)
        {
            continue;
        }

        TriggerInfo info;
        info.Sensor = world.GetActor(sensorId);
        info.Other = world.GetActor(visitorId);

        if (hasOwn)
        {
            CallHandler(state.Actors[sensorId.Index].OnEntered, info);
        }

        CallHandler(state.OnAnyEnter, info);
    }

    for (int index = 0; index < events.endCount; ++index)
    {
        const b3SensorEndTouchEvent& end = events.endEvents[index];

        const ActorId sensorId = detail::ActorIdForShape(state, end.sensorShapeId);
        const ActorId visitorId = detail::ActorIdForShape(state, end.visitorShapeId);

        if (!sensorId.IsValid() || !visitorId.IsValid())
        {
            continue;
        }

        const bool hasOwn = static_cast<bool>(state.Actors[sensorId.Index].OnExited);
        if (!hasOwn && !state.OnAnyExit)
        {
            continue;
        }

        TriggerInfo info;
        info.Sensor = world.GetActor(sensorId);
        info.Other = world.GetActor(visitorId);

        if (hasOwn)
        {
            CallHandler(state.Actors[sensorId.Index].OnExited, info);
        }

        CallHandler(state.OnAnyExit, info);
    }
}


// The closest hit along a ray, ignoring sensors. A ray asks what solid thing is
// there, and a trigger volume is not one: without this, aiming at an object
// standing inside a checkpoint would pick the checkpoint.
struct ClosestSolidHit
{
    b3ShapeId Shape{};
    b3Pos Point{};
    b3Vec3 Normal{};
    float Fraction = 1.0f;
    bool Found = false;
};

float ClosestSolidCallback(b3ShapeId shapeId, b3Pos point, b3Vec3 normal, float fraction,
                           uint64_t userMaterialId, int triangleIndex, int childIndex, void* context)
{
    (void)userMaterialId;
    (void)triangleIndex;
    (void)childIndex;

    if (b3Shape_IsSensor(shapeId))
    {
        return -1.0f; // filter it out and keep going
    }

    auto* hit = static_cast<ClosestSolidHit*>(context);
    hit->Shape = shapeId;
    hit->Point = point;
    hit->Normal = normal;
    hit->Fraction = fraction;
    hit->Found = true;

    // Clipping the ray to this hit means only something nearer can replace it.
    return fraction;
}

// Everything a world's audio needs each frame: the camera, where each actor
// is and how it moves, and a ray cast for occlusion.
void UpdateAudio(World3D& world, World3DState& state, float deltaSeconds)
{
    if (state.Audio == nullptr)
    {
        return;
    }

    detail::AudioFrame frame;
    frame.DeltaSeconds = deltaSeconds;
    frame.HasCamera = true;
    frame.CameraPosition = state.Camera.Position;
    frame.CameraForward = detail::NormalizeVector(Vec3{ state.Camera.Target.X - state.Camera.Position.X,
                                                        state.Camera.Target.Y - state.Camera.Position.Y,
                                                        state.Camera.Target.Z - state.Camera.Position.Z });
    frame.CameraUp = state.Camera.Up;

    frame.ResolveActor = [&state](const ActorId& id, Vec3& position, Vec3& velocity, Vec3& forward) {
        if (!id.IsValid() || id.Index >= state.Actors.size())
        {
            return false;
        }
        const ActorRecord3& record = state.Actors[id.Index];
        if (!record.Alive || record.Generation != id.Generation)
        {
            return false;
        }
        const Transform3 pose = detail::LerpTransform(record.Previous, record.Current, state.Alpha);
        position = pose.Position;
        velocity = detail::FromB3(b3Body_GetLinearVelocity(record.Body));

        // An actor faces down its own -Z.
        const Quat& q = pose.Rotation;
        const Quat r = detail::QuatMultiply(detail::QuatMultiply(q, Quat{ 0.0f, 0.0f, -1.0f, 0.0f }),
                                            detail::QuatConjugate(q));
        forward = Vec3{ r.X, r.Y, r.Z };
        return true;
    };

    frame.IsBlocked = [&world](const Vec3& from, const Vec3& to, const ActorId& ignore) {
        const Vec3 offset{ to.X - from.X, to.Y - from.Y, to.Z - from.Z };
        const float distance = std::sqrt(offset.X * offset.X + offset.Y * offset.Y + offset.Z * offset.Z);
        if (distance < 0.05f)
        {
            return false;
        }
        const RayHit hit = world.CastRay(from, offset, distance);
        return hit.Hit && hit.Actor.GetId() != ignore && hit.Distance < distance - 0.05f;
    };

    state.Audio->Update(frame);
}

detail::WorldAudio* EnsureAudio(World3DState& state)
{
    if (state.Audio == nullptr)
    {
        state.Audio = new detail::WorldAudio();
    }
    return state.Audio->IsReady() ? state.Audio : nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// World lifetime
// ---------------------------------------------------------------------------

World3D CreateWorld3D(const World3DConfig& config)
{
    World3D world;

    auto* state = new World3DState();
    state->Config = config;

    if (!detail::IsFinite(config.Gravity))
    {
        LogMessage(LogLevel::Error, "world",
                   "Gravity is not finite; falling back to the default of {0, -10, 0}.");
        state->Config.Gravity = Vec3{ 0.0f, -10.0f, 0.0f };
    }

    if (!(state->Config.FixedTimeStep > 0.0f) || !detail::IsFinite(state->Config.FixedTimeStep))
    {
        LogMessage(LogLevel::Error, "world",
                   "FixedTimeStep must be a positive finite number; falling back to 1/60 s.");
        state->Config.FixedTimeStep = 1.0f / 60.0f;
    }

    state->Config.SubStepCount = std::max(1, state->Config.SubStepCount);
    state->Config.MaxStepsPerFrame = std::max(1, state->Config.MaxStepsPerFrame);

    b3WorldDef worldDef = b3DefaultWorldDef();
    worldDef.gravity = detail::ToB3(state->Config.Gravity);
    worldDef.enableSleep = state->Config.EnableSleep;
    worldDef.enableContinuous = state->Config.EnableContinuous;

    // Box3D subdivides its solver into tasks and hands them to whatever
    // scheduler it is given. One shared pool serves both physics backends, so
    // they never over-subscribe the CPU against each other.
    //
    // Determinism pins this to one worker. Thread count changes how work is
    // split, and reproducing a run exactly is worth more than the speed when
    // you have asked for it.
    uint32_t requested = state->Config.Deterministic ? 1u : state->Config.WorkerCount;

    if (requested != 1)
    {
        detail::JobSystem& jobs = detail::GetJobSystem();
        jobs.Start(requested);

        worldDef.workerCount = jobs.GetWorkerCount();
        worldDef.enqueueTask = &EnqueueTask;
        worldDef.finishTask = &FinishTask;
        worldDef.userTaskContext = &jobs;

        state->Config.WorkerCount = jobs.GetWorkerCount();
    }
    else
    {
        worldDef.workerCount = 1;
        state->Config.WorkerCount = 1;
    }

    state->World = b3CreateWorld(&worldDef);
    if (!b3World_IsValid(state->World))
    {
        LogMessage(LogLevel::Error, "world", "Box3D world creation failed.");
        delete state;
        return world;
    }

    state->PhysicsRunning = state->Config.PhysicsEnabled;

    LogMessage(LogLevel::Info, "world",
               "Created a 3D world: gravity {%.2f, %.2f, %.2f}, %d sub-steps at %.1f Hz%s.",
               static_cast<double>(state->Config.Gravity.X),
               static_cast<double>(state->Config.Gravity.Y),
               static_cast<double>(state->Config.Gravity.Z),
               state->Config.SubStepCount,
               static_cast<double>(1.0f / state->Config.FixedTimeStep),
               state->Config.Deterministic ? ", deterministic" : "");

    world.m_State = state;
    return world;
}

World3D::~World3D()
{
    if (m_State != nullptr)
    {
        if (m_State->Renderer != nullptr)
        {
            m_State->Renderer->Shutdown();
            delete m_State->Renderer;
            m_State->Renderer = nullptr;
        }

        delete m_State->Models;
        m_State->Models = nullptr;

        delete m_State->Audio;
        m_State->Audio = nullptr;

        // Destroying the world releases every body, shape, and joint it owns.
        if (b3World_IsValid(m_State->World))
        {
            b3DestroyWorld(m_State->World);
        }
        delete m_State;
        m_State = nullptr;
    }
}

World3D::World3D(World3D&& other) noexcept
    : m_State(other.m_State)
{
    other.m_State = nullptr;
}

World3D& World3D::operator=(World3D&& other) noexcept
{
    if (this != &other)
    {
        this->~World3D();
        m_State = other.m_State;
        other.m_State = nullptr;
    }
    return *this;
}

// ---------------------------------------------------------------------------
// Actor creation
// ---------------------------------------------------------------------------

Actor3D World3D::AddBox(const BoxDesc& desc)
{
    Actor3D actor;
    if (m_State == nullptr)
    {
        return actor;
    }

    if (!detail::IsFinite(desc.Position) || !detail::IsFinite(desc.Rotation))
    {
        LogMessage(LogLevel::Error, "actor", "AddBox was given a non-finite transform; no actor was created.");
        return actor;
    }

    if (!detail::ValidateExtent(desc.Scale.X, "Box width") ||
        !detail::ValidateExtent(desc.Scale.Y, "Box height") ||
        !detail::ValidateExtent(desc.Scale.Z, "Box depth"))
    {
        return actor;
    }

    const b3BodyId body = CreateBody(*m_State, desc.Position, desc.Rotation, desc.Type, desc.IsBullet);

    b3BoxHull hull = b3MakeBoxHull(desc.Scale.X * 0.5f, desc.Scale.Y * 0.5f, desc.Scale.Z * 0.5f);
    b3ShapeDef shapeDef = MakeShapeDef(desc.Density, desc.Friction, desc.Restitution, desc.IsSensor);
    b3CreateHullShape(body, &shapeDef, &hull.base);

    actor.m_State = m_State;
    actor.m_Id = RegisterActor(*m_State, body, desc.Scale, desc.Name, ShapeKind::Box, desc.IsSensor);
    return actor;
}

Actor3D World3D::AddSphere(const SphereDesc& desc)
{
    Actor3D actor;
    if (m_State == nullptr)
    {
        return actor;
    }

    if (!detail::IsFinite(desc.Position))
    {
        LogMessage(LogLevel::Error, "actor", "AddSphere was given a non-finite position; no actor was created.");
        return actor;
    }

    if (!detail::ValidateExtent(desc.Radius * 2.0f, "Sphere diameter"))
    {
        return actor;
    }

    const b3BodyId body = CreateBody(*m_State, desc.Position, Quat{}, desc.Type, desc.IsBullet);

    b3Sphere sphere;
    sphere.center = b3Vec3{ 0.0f, 0.0f, 0.0f };
    sphere.radius = desc.Radius;

    b3ShapeDef shapeDef = MakeShapeDef(desc.Density, desc.Friction, desc.Restitution, desc.IsSensor);
    b3CreateSphereShape(body, &shapeDef, &sphere);

    const Vec3 scale{ desc.Radius * 2.0f, desc.Radius * 2.0f, desc.Radius * 2.0f };

    actor.m_State = m_State;
    actor.m_Id = RegisterActor(*m_State, body, scale, desc.Name, ShapeKind::Sphere, desc.IsSensor);
    return actor;
}

Actor3D World3D::AddCapsule(const CapsuleDesc& desc)
{
    Actor3D actor;
    if (m_State == nullptr)
    {
        return actor;
    }

    if (!detail::IsFinite(desc.Position))
    {
        LogMessage(LogLevel::Error, "actor", "AddCapsule was given a non-finite position; no actor was created.");
        return actor;
    }

    if (!detail::ValidateExtent(desc.Radius * 2.0f, "Capsule diameter") ||
        !detail::ValidateExtent(desc.Height, "Capsule height"))
    {
        return actor;
    }

    // Height is the total height including both hemispherical caps, so the
    // segment between the cap centers is shorter by one diameter.
    const float halfSegment = std::max(0.0f, desc.Height * 0.5f - desc.Radius);

    const b3BodyId body = CreateBody(*m_State, desc.Position, Quat{}, desc.Type, false);

    b3Capsule capsule;
    capsule.center1 = b3Vec3{ 0.0f, -halfSegment, 0.0f };
    capsule.center2 = b3Vec3{ 0.0f, halfSegment, 0.0f };
    capsule.radius = desc.Radius;

    b3ShapeDef shapeDef = MakeShapeDef(desc.Density, desc.Friction, desc.Restitution, desc.IsSensor);
    b3CreateCapsuleShape(body, &shapeDef, &capsule);

    const Vec3 scale{ desc.Radius * 2.0f, desc.Height, desc.Radius * 2.0f };

    actor.m_State = m_State;
    actor.m_Id = RegisterActor(*m_State, body, scale, desc.Name, ShapeKind::Capsule, desc.IsSensor);
    return actor;
}

namespace
{

// A flat model (a sign, a leaf, a decal) has no depth to fit a box to, and a box of no depth is one the solver cannot use. Its
// collider is thickened to this in any direction the model is thinner.
constexpr float ThinModelCollider = 0.01f;

// The box collider fitted to a model's bounds at a scale: its size and where
// its centre sits. False, with a diagnostic, when the bounds are not numbers
// or the model is too small in every direction to be anything at all.
bool FitModelBox(const detail::ModelMesh& mesh, float scale, const char* name, Vec3& outSize, Vec3& outCentre)
{
    outSize = Vec3{ (mesh.Maximum.X - mesh.Minimum.X) * scale, (mesh.Maximum.Y - mesh.Minimum.Y) * scale,
                    (mesh.Maximum.Z - mesh.Minimum.Z) * scale };
    outCentre = Vec3{ (mesh.Maximum.X + mesh.Minimum.X) * 0.5f * scale,
                      (mesh.Maximum.Y + mesh.Minimum.Y) * 0.5f * scale,
                      (mesh.Maximum.Z + mesh.Minimum.Z) * 0.5f * scale };

    if (!detail::IsFinite(outSize) || !detail::IsFinite(outCentre))
    {
        LogMessage(LogLevel::Error, "physics", "\"%s\" has bounds that are not numbers; it was not added.", name);
        return false;
    }

    const float largest = std::max({ outSize.X, outSize.Y, outSize.Z });
    if (largest < ThinModelCollider)
    {
        return detail::ValidateExtent(largest, "Model size");
    }

    outSize.X = std::max(outSize.X, ThinModelCollider);
    outSize.Y = std::max(outSize.Y, ThinModelCollider);
    outSize.Z = std::max(outSize.Z, ThinModelCollider);
    return detail::ValidateExtent(outSize.X, "Model width") && detail::ValidateExtent(outSize.Y, "Model height") &&
           detail::ValidateExtent(outSize.Z, "Model depth");
}

} // namespace

Actor3D World3D::AddModel(const ModelDesc& desc)
{
    Actor3D actor;
    if (m_State == nullptr)
    {
        return actor;
    }

    if (!detail::IsFinite(desc.Position) || !detail::IsFinite(desc.Rotation) ||
        !detail::IsFinite(desc.Scale) || desc.Scale <= 0.0f)
    {
        LogMessage(LogLevel::Error, "actor", "AddModel was given a non-finite transform or scale.");
        return actor;
    }

    if (m_State->Models == nullptr)
    {
        m_State->Models = new detail::ModelStore();
    }

    // In the background, the actor exists this frame as a box of its own
    // scale and takes the model's shape when the parse lands.
    if (desc.LoadInBackground)
    {
        const int pending = m_State->Models->LoadInBackground(desc.Path);

        Actor3D placeholder = AddBox({
            .Scale = { desc.Scale, desc.Scale, desc.Scale },
            .Position = desc.Position,
            .Rotation = desc.Rotation,
            .Type = desc.Type,
            .Density = desc.Density,
            .Friction = desc.Friction,
            .Restitution = desc.Restitution,
            .Name = desc.Name,
        });

        if (ActorRecord3* record = detail::Resolve(m_State, placeholder.GetId(), "AddModel"))
        {
            record->ModelIndex = pending;
            record->AwaitingModel = pending >= 0;
            record->ModelScale = desc.Scale;
            record->ModelDensity = desc.Density;
            record->ModelFriction = desc.Friction;
            record->ModelRestitution = desc.Restitution;

            // Drawn as a model from the start: the store hands back a
            // placeholder shape until the file lands.
            if (pending >= 0)
            {
                record->Shape = ShapeKind::Model;
                record->Scale = Vec3{ desc.Scale, desc.Scale, desc.Scale };
            }
        }
        return placeholder;
    }

    const int modelIndex = m_State->Models->Load(desc.Path);
    if (modelIndex < 0)
    {
        // A missing model still shows up, as a one-metre cube wearing the
        // missing-texture checkerboard and named after the file, so the
        // program runs and the gap is obvious in the scene and in the name.
        Actor3D placeholder = AddBox({
            .Scale = { desc.Scale, desc.Scale, desc.Scale },
            .Position = desc.Position,
            .Rotation = desc.Rotation,
            .Type = desc.Type,
            .Density = desc.Density,
            .Friction = desc.Friction,
            .Restitution = desc.Restitution,
            .Name = "missing: " + desc.Path,
        });
        placeholder.SetColor(Color{ 1.0f, 1.0f, 1.0f, 1.0f });
        placeholder.SetTexture(detail::GetWorldTextures().GetMissing());
        return placeholder;
    }

    const detail::ModelMesh* mesh = m_State->Models->Get(modelIndex);

    // The collider is a box fitted to the model's bounds. b3MakeOffsetBoxHull
    // places it at the model's centre rather than the body origin, which
    // matters for anything not modelled around zero.
    Vec3 size;
    Vec3 centre;
    if (!FitModelBox(*mesh, desc.Scale, desc.Path.c_str(), size, centre))
    {
        return actor;
    }

    const b3BodyId body =
        CreateBody(*m_State, desc.Position, desc.Rotation, desc.Type, false);

    b3BoxHull hull = b3MakeOffsetBoxHull(size.X * 0.5f, size.Y * 0.5f, size.Z * 0.5f,
                                         detail::ToB3(centre));
    b3ShapeDef shapeDef = MakeShapeDef(desc.Density, desc.Friction, desc.Restitution);
    b3CreateHullShape(body, &shapeDef, &hull.base);

    // The mesh is already in model units, so rendering scales uniformly.
    const Vec3 renderScale{ desc.Scale, desc.Scale, desc.Scale };

    actor.m_State = m_State;
    actor.m_Id = RegisterActor(*m_State, body, renderScale, desc.Name, ShapeKind::Model);

    if (ActorRecord3* record = detail::Resolve(m_State, actor.m_Id, "AddModel"))
    {
        record->ModelIndex = modelIndex;

        // A model's bounds come from its mesh rather than from a unit
        // primitive, and the centre may be well away from the origin.
        const float halfDiagonal =
            0.5f * std::sqrt(size.X * size.X + size.Y * size.Y + size.Z * size.Z);
        const float centreOffset =
            std::sqrt(centre.X * centre.X + centre.Y * centre.Y + centre.Z * centre.Z);

        record->BoundingRadius = halfDiagonal + centreOffset;
    }

    return actor;
}

Actor3D World3D::AddModel(const std::string& path)
{
    ModelDesc desc;
    desc.Path = path;
    return AddModel(desc);
}

Actor3D World3D::AddGround(const GroundDesc& desc)
{
    BoxDesc box;
    box.Scale = Vec3{ desc.Width, desc.Thickness, desc.Depth };

    // Position names the top surface, so the slab's center sits half its
    // thickness below it. The default therefore puts the walkable surface at
    // y = 0 instead of burying it.
    box.Position = Vec3{ desc.Position.X,
                         desc.Position.Y - desc.Thickness * 0.5f,
                         desc.Position.Z };
    box.Type = BodyType::Static;
    box.Friction = desc.Friction;
    box.Density = 1.0f;
    box.Name = desc.Name;

    return AddBox(box);
}

Actor3D World3D::GetActor(ActorId id)
{
    Actor3D actor;
    if (detail::Resolve(m_State, id, "GetActor") != nullptr)
    {
        actor.m_State = m_State;
        actor.m_Id = id;
    }
    return actor;
}

void World3D::ForEachActor(const std::function<void(Actor3D&)>& visit)
{
    if (m_State == nullptr || !visit)
    {
        return;
    }

    for (size_t index = 0; index < m_State->Actors.size(); ++index)
    {
        const ActorRecord3& record = m_State->Actors[index];
        if (!record.Alive)
        {
            continue;
        }

        Actor3D actor;
        actor.m_State = m_State;
        actor.m_Id = ActorId{ static_cast<uint32_t>(index), record.Generation };
        visit(actor);
    }
}

// ---------------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------------

void World3D::ApplyPendingChanges()
{
    if (m_State == nullptr)
    {
        return;
    }

    for (const JointId& id : m_State->PendingJointDestroys)
    {
        if (id.Index >= m_State->Joints.size())
        {
            continue;
        }

        detail::JointRecord3& record = m_State->Joints[id.Index];
        if (!record.Alive || record.Generation != id.Generation)
        {
            continue;
        }

        if (b3Joint_IsValid(record.Joint))
        {
            b3DestroyJoint(record.Joint, true);
        }

        record.Alive = false;
        ++record.Generation;
        if (record.Generation == 0)
        {
            record.Generation = 1;
        }

        m_State->FreeJoints.push_back(id.Index);
        --m_State->LiveJoints;
    }
    m_State->PendingJointDestroys.clear();

    if (!m_State->Commands.empty())
    {

    // Indexed rather than range-based: applying a command may append more.
    for (size_t i = 0; i < m_State->Commands.size(); ++i)
    {
        const detail::Command command = m_State->Commands[i];
        ActorRecord3* record = detail::Resolve(m_State, command.Id, "A queued world change");
        if (record == nullptr)
        {
            continue;
        }

        switch (command.Type)
        {
            case detail::CommandType::Destroy:
            {
                detail::DetachFromHierarchy(*m_State, command.Id, *record);

                if (b3Body_IsValid(record->Body))
                {
                    b3DestroyBody(record->Body);
                }

                record->Alive = false;
                record->Name.clear();

                // Releasing the handler here means a lambda that captured
                // other actors stops holding them the moment this one dies.
                record->OnCollided = {};

                // Bumping the generation is what makes every outstanding handle
                // to this actor detectably stale.
                ++record->Generation;
                if (record->Generation == 0)
                {
                    record->Generation = 1;
                }

                m_State->FreeIndices.push_back(command.Id.Index);
                --m_State->LiveCount;
                break;
            }

            case detail::CommandType::SetBodyType:
            {
                b3Body_SetType(record->Body, detail::ToB3BodyType(command.NewBodyType));
                break;
            }

            case detail::CommandType::SetParent:
            {
                detail::ApplyParent(*m_State, command.Id, command.Parent);
                break;
            }
        }
    }

        m_State->Commands.clear();

        // Anything that was just attached, detached, or destroyed may have
        // left a child somewhere it no longer belongs.
        detail::PropagateHierarchy(*m_State);
    }

    // Last, because destroying an actor takes its joints with it inside Box3D
    // and that happens in the command loop above. Sweeping earlier would miss
    // the joints that just died.
    if (m_State->LiveJoints > 0)
    {
        for (size_t index = 0; index < m_State->Joints.size(); ++index)
        {
            detail::JointRecord3& record = m_State->Joints[index];
            if (record.Alive && !b3Joint_IsValid(record.Joint))
            {
                record.Alive = false;
                ++record.Generation;
                if (record.Generation == 0)
                {
                    record.Generation = 1;
                }
                m_State->FreeJoints.push_back(static_cast<uint32_t>(index));
                --m_State->LiveJoints;
            }
        }
    }
}

void World3D::StepPhysics(float timeStep)
{
    if (m_State == nullptr)
    {
        return;
    }

    if (!(timeStep > 0.0f) || !detail::IsFinite(timeStep))
    {
        LogMessage(LogLevel::Error, "world", "StepPhysics needs a positive finite time step; the call was ignored.");
        return;
    }

    ApplyPendingChanges();

    // The previous snapshot is what rendering interpolates away from.
    for (ActorRecord3& record : m_State->Actors)
    {
        if (record.Alive)
        {
            record.Previous = record.Current;
        }
    }

    {
        LUDIFEX_PROFILE("physics");
        {
        LUDIFEX_PROFILE("physics");
        b3World_Step(m_State->World, timeStep, m_State->Config.SubStepCount);
    }
    }

    for (ActorRecord3& record : m_State->Actors)
    {
        if (record.Alive)
        {
            record.Current = ReadTransform(record.Body, record.Scale);
        }
    }

    // Children follow their parents once the solver has finished moving them,
    // so a carried actor is never a frame behind what carries it.
    {
        LUDIFEX_PROFILE("hierarchy");
        detail::PropagateHierarchy(*m_State);
    }

    ++m_State->StepCount;

    // Events are delivered between the solver finishing and the sync point, so
    // a handler that destroys an actor has its request applied immediately
    // afterwards rather than a frame later.
    {
        LUDIFEX_PROFILE("events");
        DispatchCollisions(*m_State, *this);
        DispatchSensors(*m_State, *this);
    }

    // A second sync point, for changes queued from inside event callbacks.
    ApplyPendingChanges();
}

// A model that arrived from the loading thread: the actor that was waiting
// for it gets the model's bounds, its collider, and its own scale.
// A model whose file changed is re-parsed by the store; what is left is every
// actor that was built around its old bounds. Those actors are marked as
// awaiting their model again, so the code that fits a background load fits
// them too.
void ReloadChangedAssets(World3DState& state)
{
    if (!detail::ShouldCheckForChanges())
    {
        return;
    }

    if (state.Models != nullptr)
    {
        std::vector<int> changed;
        if (state.Models->ReloadChanged(changed) > 0)
        {
            for (ActorRecord3& record : state.Actors)
            {
                if (record.Alive && record.ModelIndex >= 0 &&
                    std::find(changed.begin(), changed.end(), record.ModelIndex) != changed.end())
                {
                    record.AwaitingModel = true;
                }
            }
        }
    }

    // Textures need nothing refitted: an image is the same shape whatever is
    // in it, so letting go of the GPU texture is the whole of it.
    detail::GetWorldTextures().ReloadChanged();
}

void FitArrivedModels(World3DState& state)
{
    if (state.Models == nullptr)
    {
        return;
    }

    for (ActorRecord3& record : state.Actors)
    {
        if (!record.Alive || !record.AwaitingModel)
        {
            continue;
        }

        const detail::ModelMesh* mesh = state.Models->Get(record.ModelIndex);
        if (mesh == nullptr || !mesh->Ready)
        {
            continue;
        }

        record.AwaitingModel = false;

        if (mesh->Indices.empty())
        {
            // The file could not be read. The placeholder box stays.
            record.Shape = ShapeKind::Box;
            record.ModelIndex = -1;
            continue;
        }

        const float scale = record.ModelScale;
        Vec3 size;
        Vec3 centre;
        if (!FitModelBox(*mesh, scale, mesh->Path.c_str(), size, centre))
        {
            // Nothing sensible to fit; the placeholder box stays, as it does
            // for a file that could not be read.
            record.Shape = ShapeKind::Box;
            record.ModelIndex = -1;
            continue;
        }

        // The placeholder's collider goes; the model's own takes its place.
        b3ShapeId shapes[8]{};
        const int count = b3Body_GetShapes(record.Body, shapes, 8);
        for (int index = 0; index < count; ++index)
        {
            b3DestroyShape(shapes[index], false);
        }

        b3BoxHull hull = b3MakeOffsetBoxHull(size.X * 0.5f, size.Y * 0.5f, size.Z * 0.5f,
                                             detail::ToB3(centre));
        b3ShapeDef shapeDef =
            MakeShapeDef(record.ModelDensity, record.ModelFriction, record.ModelRestitution);
        b3CreateHullShape(record.Body, &shapeDef, &hull.base);
        b3Body_ApplyMassFromShapes(record.Body);

        record.Scale = Vec3{ scale, scale, scale };

        const float halfDiagonal = 0.5f * std::sqrt(size.X * size.X + size.Y * size.Y + size.Z * size.Z);
        const float centreOffset =
            std::sqrt(centre.X * centre.X + centre.Y * centre.Y + centre.Z * centre.Z);
        record.BoundingRadius = halfDiagonal + centreOffset;
    }
}

namespace
{

// Advances every animated actor and rebuilds the joint matrices that moved.
//
// Uses frame time instead of the fixed physics step, since animation is
// presentation.
//
// Spread across the scheduler because each actor's pose is independent of the
// others'. Actors that are not animated are skipped with a test of an empty
// vector.
void AdvanceAnimations(detail::World3DState& state, float deltaSeconds)
{
    if (state.Models == nullptr || state.Actors.empty())
    {
        return;
    }

    // Found first, then advanced, so a world with nothing to animate never
    // goes through the scheduler. The scan is a test of an empty vector per
    // actor.
    state.AnimatedActors.clear();
    for (uint32_t index = 0; index < state.Actors.size(); ++index)
    {
        const detail::ActorRecord3& record = state.Actors[index];
        if (record.Alive && !record.Animation.Layers.empty() && record.ModelIndex >= 0)
        {
            state.AnimatedActors.push_back(index);
        }
    }

    if (state.AnimatedActors.empty())
    {
        return;
    }

    LUDIFEX_PROFILE("animation");

    struct Work
    {
        detail::World3DState* State;
        float DeltaSeconds;
    } work{ &state, deltaSeconds };

    auto Advance = [](int startIndex, int endIndex, uint32_t, void* context) {
        Work& work = *static_cast<Work*>(context);
        detail::World3DState& state = *work.State;

        for (int slot = startIndex; slot < endIndex; ++slot)
        {
            detail::ActorRecord3& record =
                state.Actors[state.AnimatedActors[static_cast<size_t>(slot)]];

            const detail::ModelMesh* mesh = state.Models->Get(record.ModelIndex);
            if (mesh == nullptr || !mesh->Ready || !mesh->IsAnimated())
            {
                continue;
            }

            detail::AdvancePose(record.Animation, mesh->Animations, work.DeltaSeconds);
            detail::BuildPose(record.Animation, mesh->Joints, mesh->SkinJoints, mesh->MorphTargets,
                              mesh->Animations);
        }
    };

    const int count = static_cast<int>(state.AnimatedActors.size());

    // Eight characters a chunk, and only worth dividing at all past thirty-two
    // of them: sampling a skeleton is real work, but not so much that two of
    // them are worth a trip through the scheduler.
    detail::JobSystem& jobs = detail::GetJobSystem();
    if (jobs.IsRunning() && count >= 32)
    {
        void* handle = jobs.SubmitRange(Advance, count, 8, &work);
        jobs.Wait(handle);
    }
    else
    {
        Advance(0, count, 0, &work);
    }
}

} // namespace

void World3D::Update(float deltaSeconds)
{
    if (m_State == nullptr)
    {
        return;
    }

    // A frame starts here: what was gathered becomes the last frame's shape.
    detail::Profiler::Get().BeginFrame();
    LUDIFEX_PROFILE("world update");

    // Anything the loading thread finished is brought in here, on this thread,
    // where touching the world and the GPU is safe.
    {
        LUDIFEX_PROFILE("assets arriving");
        detail::LoadQueue::Get().Collect();
        ReloadChangedAssets(*m_State);
        FitArrivedModels(*m_State);
    }

    // Physics first, then sound: the audio update reads the poses the steps
    // just produced, so a sound on a moving actor is where the actor is.
    [&]() {
        ApplyPendingChanges();

        if (m_State->Mode == PhysicsMode::Manual || !m_State->PhysicsRunning)
        {
            return;
        }

        if (!detail::IsFinite(deltaSeconds) || deltaSeconds <= 0.0f)
        {
            return;
        }

        const float fixedStep = m_State->Config.FixedTimeStep;
        const int maxSteps = m_State->Config.MaxStepsPerFrame;

        m_State->Accumulator += deltaSeconds;

        // The accumulator is clamped so a slow frame cannot request extra steps
        // that make the next frame slower still.
        const float maximumAccumulated = fixedStep * static_cast<float>(maxSteps);
        if (m_State->Accumulator > maximumAccumulated)
        {
            m_State->Accumulator = maximumAccumulated;
        }

        int steps = 0;
        while (m_State->Accumulator >= fixedStep && steps < maxSteps)
        {
            StepPhysics(fixedStep);
            m_State->Accumulator -= fixedStep;
            ++steps;
        }

        m_State->Alpha = m_State->Accumulator / fixedStep;
    }();

    const float wallClock = detail::IsFinite(deltaSeconds) ? std::max(0.0f, deltaSeconds) : 0.0f;

    AdvanceAnimations(*m_State, wallClock);

    UpdateAudio(*this, *m_State, wallClock);
}

void World3D::StartPhysics()
{
    if (m_State != nullptr)
    {
        m_State->PhysicsRunning = true;
        // Time accumulated while paused is discarded rather than applied as one
        // large catch-up, which would look like a jump.
        m_State->Accumulator = 0.0f;
    }
}

void World3D::StopPhysics()
{
    if (m_State != nullptr)
    {
        m_State->PhysicsRunning = false;
    }
}

bool World3D::IsPhysicsRunning() const
{
    return m_State != nullptr && m_State->PhysicsRunning;
}

void World3D::SetPhysicsMode(PhysicsMode mode)
{
    if (m_State != nullptr)
    {
        m_State->Mode = mode;
        m_State->Accumulator = 0.0f;
    }
}

PhysicsMode World3D::GetPhysicsMode() const
{
    return m_State != nullptr ? m_State->Mode : PhysicsMode::Automatic;
}

void World3D::SetGravity(Vec3 gravity)
{
    if (m_State == nullptr)
    {
        return;
    }

    if (!detail::IsFinite(gravity))
    {
        LogMessage(LogLevel::Error, "world", "SetGravity was given a non-finite vector; the call was ignored.");
        return;
    }

    m_State->Config.Gravity = gravity;
    b3World_SetGravity(m_State->World, detail::ToB3(gravity));
}

Vec3 World3D::GetGravity() const
{
    return m_State != nullptr ? m_State->Config.Gravity : Vec3{};
}

void World3D::SetFixedTimeStep(float seconds)
{
    if (m_State == nullptr)
    {
        return;
    }

    if (!(seconds > 0.0f) || !detail::IsFinite(seconds))
    {
        LogMessage(LogLevel::Error, "world", "SetFixedTimeStep needs a positive finite value; the call was ignored.");
        return;
    }

    m_State->Config.FixedTimeStep = seconds;
    m_State->Accumulator = 0.0f;
}

float World3D::GetFixedTimeStep() const
{
    return m_State != nullptr ? m_State->Config.FixedTimeStep : 0.0f;
}

void World3D::SetSubStepCount(int count)
{
    if (m_State != nullptr)
    {
        m_State->Config.SubStepCount = std::max(1, count);
    }
}

int World3D::GetSubStepCount() const
{
    return m_State != nullptr ? m_State->Config.SubStepCount : 0;
}

void World3D::SetInterpolationAlpha(float alpha)
{
    if (m_State != nullptr && detail::IsFinite(alpha))
    {
        m_State->Alpha = std::clamp(alpha, 0.0f, 1.0f);
    }
}

float World3D::GetInterpolationAlpha() const
{
    return m_State != nullptr ? m_State->Alpha : 0.0f;
}

size_t World3D::GetActorCount() const
{
    return m_State != nullptr ? m_State->LiveCount : 0;
}

uint64_t World3D::GetStepCount() const
{
    return m_State != nullptr ? m_State->StepCount : 0;
}

uint32_t World3D::GetWorkerCount() const
{
    return m_State != nullptr ? m_State->Config.WorkerCount : 0;
}

void World3D::WhenActorCollided(CollisionHandler handler)
{
    if (m_State != nullptr)
    {
        m_State->OnAnyCollision = std::move(handler);
    }
}

void World3D::SetCollisionThreshold(float metersPerSecond)
{
    if (m_State != nullptr)
    {
        m_State->CollisionThreshold = std::max(0.0f, metersPerSecond);
    }
}

void World3D::WhenActorEnteredTrigger(TriggerHandler handler)
{
    if (m_State != nullptr)
    {
        m_State->OnAnyEnter = std::move(handler);
    }
}

void World3D::WhenActorLeftTrigger(TriggerHandler handler)
{
    if (m_State != nullptr)
    {
        m_State->OnAnyExit = std::move(handler);
    }
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

RayHit World3D::CastRay(Vec3 origin, Vec3 direction, float maxDistance)
{
    RayHit result;

    if (m_State == nullptr)
    {
        return result;
    }

    if (!detail::IsFinite(origin) || !detail::IsFinite(direction) ||
        !detail::IsFinite(maxDistance) || maxDistance <= 0.0f)
    {
        LogMessage(LogLevel::Error, "world", "CastRay was given non-finite arguments; it found nothing.");
        return result;
    }

    const float lengthSquared =
        direction.X * direction.X + direction.Y * direction.Y + direction.Z * direction.Z;
    if (lengthSquared <= 1e-12f)
    {
        LogMessage(LogLevel::Error, "world", "CastRay was given a zero-length direction.");
        return result;
    }

    // The direction is normalized here so callers can pass an unnormalized
    // vector and still get a distance in meters.
    const float inverseLength = 1.0f / std::sqrt(lengthSquared);
    const Vec3 translation{ direction.X * inverseLength * maxDistance,
                            direction.Y * inverseLength * maxDistance,
                            direction.Z * inverseLength * maxDistance };

    ClosestSolidHit hit;
    b3World_CastRay(m_State->World, detail::ToB3Pos(origin), detail::ToB3(translation),
                    b3DefaultQueryFilter(), ClosestSolidCallback, &hit);

    if (!hit.Found)
    {
        return result;
    }

    const ActorId id = detail::ActorIdForShape(*m_State, hit.Shape);
    if (!id.IsValid())
    {
        return result;
    }

    result.Hit = true;
    result.Actor = GetActor(id);
    result.Point = detail::FromB3Pos(hit.Point);
    result.Normal = detail::FromB3(hit.Normal);
    result.Fraction = hit.Fraction;
    result.Distance = hit.Fraction * maxDistance;
    return result;
}

RayHit World3D::PickFromView(float normalizedX, float normalizedY, float maxDistance)
{
    RayHit result;

    if (m_State == nullptr || m_State->RenderWidth <= 0 || m_State->RenderHeight <= 0)
    {
        return result;
    }

    const Camera3D& camera = m_State->Camera;

    // Rebuild the camera basis, then walk out to the near plane and offset by
    // the point's position on it. This is the inverse of what the vertex stage
    // does, without needing a matrix inversion.
    const Vec3 forward = detail::NormalizeVector(
        Vec3{ camera.Target.X - camera.Position.X, camera.Target.Y - camera.Position.Y,
              camera.Target.Z - camera.Position.Z });

    const Vec3 right = detail::NormalizeVector(detail::CrossProduct(forward, camera.Up));
    const Vec3 up = detail::CrossProduct(right, forward);

    const float aspect =
        static_cast<float>(m_State->RenderWidth) / static_cast<float>(m_State->RenderHeight);
    const float halfHeight = std::tan(camera.FieldOfViewDegrees * 3.14159265358979323846f / 360.0f);
    const float halfWidth = halfHeight * aspect;

    // Normalized view coordinates run left to right and top to bottom, so the
    // vertical axis is flipped into the camera's up direction.
    const float viewX = (normalizedX * 2.0f - 1.0f) * halfWidth;
    const float viewY = (1.0f - normalizedY * 2.0f) * halfHeight;

    const Vec3 direction{ forward.X + right.X * viewX + up.X * viewY,
                          forward.Y + right.Y * viewX + up.Y * viewY,
                          forward.Z + right.Z * viewX + up.Z * viewY };

    return CastRay(camera.Position, direction, maxDistance);
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void World3D::SetRenderSize(int width, int height)
{
    if (m_State == nullptr || width <= 0 || height <= 0)
    {
        return;
    }

    if (m_State->Renderer == nullptr)
    {
        SDL_GPUDevice* device = detail::EnsureDevice();
        if (device == nullptr)
        {
            return;
        }

        auto* renderer = new detail::Renderer3D();
        if (!renderer->Initialize(device))
        {
            renderer->Shutdown();
            delete renderer;
            LogMessage(LogLevel::Error, "render", "The world renderer could not start.");
            return;
        }
        m_State->Renderer = renderer;
    }

    m_State->RenderWidth = width;
    m_State->RenderHeight = height;
    m_State->Renderer->SetSize(width, height, m_State->Render);
}

void World3D::Render()
{
    if (m_State == nullptr || m_State->Renderer == nullptr)
    {
        return;
    }

    // Picks up a change to the anti-aliasing setting without the caller having
    // to resize first.
    m_State->Renderer->SetSize(m_State->RenderWidth, m_State->RenderHeight, m_State->Render);
    m_State->Renderer->Render(*m_State);
}

void World3D::Run()
{
    if (m_State == nullptr)
    {
        return;
    }
    detail::RunWorld(detail::MakeHostHooks(this), "ludifex");
}

void* World3D::GetRenderTarget() const
{
    if (m_State == nullptr || m_State->Renderer == nullptr)
    {
        return nullptr;
    }
    return m_State->Renderer->GetResolvedTexture();
}

Camera3D& World3D::GetCamera()
{
    static Camera3D fallback;
    return m_State != nullptr ? m_State->Camera : fallback;
}

const Camera3D& World3D::GetCamera() const
{
    static const Camera3D fallback;
    return m_State != nullptr ? m_State->Camera : fallback;
}

void World3D::SetCamera(const Camera3D& camera)
{
    if (m_State != nullptr)
    {
        m_State->Camera = camera;
    }
}

RenderSettings& World3D::GetRenderSettings()
{
    static RenderSettings fallback;
    return m_State != nullptr ? m_State->Render : fallback;
}

void World3D::SetRenderSettings(const RenderSettings& settings)
{
    if (m_State != nullptr)
    {
        m_State->Render = settings;
    }
}

void World3D::SetAntiAliasing(AntiAliasing mode)
{
    if (m_State != nullptr)
    {
        m_State->Render.Mode = mode;
    }
}

void World3D::SetGraphicsQuality(GraphicsQuality quality)
{
    if (m_State != nullptr)
    {
        ApplyGraphicsQuality(m_State->Render, quality);
    }
}

void World3D::SetBackground(TextureId texture)
{
    if (m_State != nullptr)
    {
        m_State->Extras.Background = texture;
    }
}

void World3D::SetBackground(const std::string& path)
{
    if (m_State != nullptr)
    {
        m_State->Extras.Background = path.empty() ? TextureId{} : LoadTexture(path);
    }
}

// ---------------------------------------------------------------------------
// Lights
// ---------------------------------------------------------------------------

Light3D World3D::AddPointLight(const PointLightDesc& desc)
{
    Light3D light;
    if (m_State == nullptr)
    {
        return light;
    }

    if (!detail::IsFinite(desc.Position) || !detail::IsFinite(desc.Intensity) || !detail::IsFinite(desc.Range) ||
        desc.Range <= 0.0f)
    {
        LogMessage(LogLevel::Error, "light",
                   "AddPointLight was given a non-finite position or a range that is not positive; no light was "
                   "created.");
        return light;
    }

    uint32_t index;
    if (!m_State->FreeLights.empty())
    {
        index = m_State->FreeLights.back();
        m_State->FreeLights.pop_back();
    }
    else
    {
        index = static_cast<uint32_t>(m_State->Lights.size());
        m_State->Lights.emplace_back();
    }

    detail::LightRecord3& record = m_State->Lights[index];
    ++record.Generation;
    if (record.Generation == 0)
    {
        record.Generation = 1;
    }
    record.Alive = true;
    record.Desc = desc;
    record.AttachedTo = ActorId{};
    record.Offset = Vec3{};
    ++m_State->LiveLights;

    light.m_State = m_State;
    light.m_Id = LightId{ index, record.Generation };
    return light;
}

size_t World3D::GetLightCount() const
{
    return m_State != nullptr ? m_State->LiveLights : 0;
}

bool Light3D::IsValid() const
{
    return m_State != nullptr && m_Id.IsValid() && m_Id.Index < m_State->Lights.size() &&
           m_State->Lights[m_Id.Index].Alive && m_State->Lights[m_Id.Index].Generation == m_Id.Generation;
}

void Light3D::SetPosition(Vec3 position)
{
    detail::LightRecord3* record = detail::Resolve(m_State, m_Id, "SetPosition");
    if (record != nullptr && detail::IsFinite(position))
    {
        record->Desc.Position = position;
        record->AttachedTo = ActorId{};
    }
}

Vec3 Light3D::GetPosition() const
{
    const detail::LightRecord3* record = detail::Resolve(m_State, m_Id, "GetPosition");
    return record != nullptr ? record->Desc.Position : Vec3{};
}

void Light3D::SetColor(Color color)
{
    if (detail::LightRecord3* record = detail::Resolve(m_State, m_Id, "SetColor"))
    {
        record->Desc.Tint = color;
    }
}

Color Light3D::GetColor() const
{
    const detail::LightRecord3* record = detail::Resolve(m_State, m_Id, "GetColor");
    return record != nullptr ? record->Desc.Tint : Color{};
}

void Light3D::SetIntensity(float intensity)
{
    detail::LightRecord3* record = detail::Resolve(m_State, m_Id, "SetIntensity");
    if (record != nullptr && detail::IsFinite(intensity))
    {
        record->Desc.Intensity = std::max(0.0f, intensity);
    }
}

float Light3D::GetIntensity() const
{
    const detail::LightRecord3* record = detail::Resolve(m_State, m_Id, "GetIntensity");
    return record != nullptr ? record->Desc.Intensity : 0.0f;
}

void Light3D::SetRange(float range)
{
    detail::LightRecord3* record = detail::Resolve(m_State, m_Id, "SetRange");
    if (record != nullptr && detail::IsFinite(range) && range > 0.0f)
    {
        record->Desc.Range = range;
    }
}

float Light3D::GetRange() const
{
    const detail::LightRecord3* record = detail::Resolve(m_State, m_Id, "GetRange");
    return record != nullptr ? record->Desc.Range : 0.0f;
}

void Light3D::AttachTo(Actor3D actor, Vec3 offset)
{
    detail::LightRecord3* record = detail::Resolve(m_State, m_Id, "AttachTo");
    if (record == nullptr)
    {
        return;
    }
    if (!actor.IsValid())
    {
        LogMessage(LogLevel::Warning, "light",
                   "AttachTo was given an actor that does not exist; the light stays where it is.");
        return;
    }
    record->AttachedTo = actor.GetId();
    record->Offset = offset;
}

void Light3D::Detach()
{
    if (detail::LightRecord3* record = detail::Resolve(m_State, m_Id, "Detach"))
    {
        record->AttachedTo = ActorId{};
    }
}

void Light3D::Destroy()
{
    detail::LightRecord3* record = detail::Resolve(m_State, m_Id, "Destroy");
    if (record == nullptr)
    {
        return;
    }
    record->Alive = false;
    ++record->Generation;
    if (record->Generation == 0)
    {
        record->Generation = 1;
    }
    m_State->FreeLights.push_back(m_Id.Index);
    --m_State->LiveLights;
}

// ---------------------------------------------------------------------------
// Post-processing and debug drawing
// ---------------------------------------------------------------------------

void World3D::AddPostProcess(MaterialId material, PassPoint point)
{
    if (m_State == nullptr)
    {
        return;
    }
    if (detail::GetWorldMaterials().Resolve(material) == nullptr)
    {
        LogMessage(LogLevel::Warning, "material", "AddPostProcess was given a material that does not exist.");
        return;
    }
    m_State->Extras.PostProcess.push_back(detail::PostProcessEntry{ material, point });
}

void World3D::RemovePostProcess(MaterialId material)
{
    if (m_State != nullptr)
    {
        detail::RemovePostProcessEntries(m_State->Extras, material);
    }
}

void World3D::ClearPostProcess()
{
    if (m_State != nullptr)
    {
        m_State->Extras.PostProcess.clear();
    }
}

void World3D::DrawLine(Vec3 from, Vec3 to, Color color)
{
    if (m_State != nullptr && detail::IsFinite(from) && detail::IsFinite(to))
    {
        detail::PushDebugLine(m_State->Extras, from, to, color);
    }
}

void World3D::DrawBox(Vec3 center, Vec3 size, Color color)
{
    if (m_State == nullptr || !detail::IsFinite(center) || !detail::IsFinite(size))
    {
        return;
    }
    const Vec3 h{ size.X * 0.5f, size.Y * 0.5f, size.Z * 0.5f };
    Vec3 corners[8];
    for (int corner = 0; corner < 8; ++corner)
    {
        corners[corner] = Vec3{ center.X + ((corner & 1) ? h.X : -h.X), center.Y + ((corner & 2) ? h.Y : -h.Y),
                                center.Z + ((corner & 4) ? h.Z : -h.Z) };
    }
    const int edges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 },
                               { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
    for (const auto& edge : edges)
    {
        detail::PushDebugLine(m_State->Extras, corners[edge[0]], corners[edge[1]], color);
    }
}

void World3D::DrawSphere(Vec3 center, float radius, Color color)
{
    if (m_State == nullptr || !detail::IsFinite(center) || !detail::IsFinite(radius))
    {
        return;
    }

    // Three great circles, one in each axis plane.
    constexpr int Segments = 32;
    constexpr float Step = 2.0f * 3.14159265358979323846f / static_cast<float>(Segments);
    for (int axis = 0; axis < 3; ++axis)
    {
        auto Point = [&](float angle) {
            const float c = std::cos(angle) * radius;
            const float s = std::sin(angle) * radius;
            switch (axis)
            {
                case 0:  return Vec3{ center.X, center.Y + c, center.Z + s };
                case 1:  return Vec3{ center.X + c, center.Y, center.Z + s };
                default: return Vec3{ center.X + c, center.Y + s, center.Z };
            }
        };
        for (int segment = 0; segment < Segments; ++segment)
        {
            detail::PushDebugLine(m_State->Extras, Point(Step * static_cast<float>(segment)),
                                  Point(Step * static_cast<float>(segment + 1)), color);
        }
    }
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

VoiceId World3D::PlaySound(SoundId sound, const SoundSettings& settings)
{
    detail::WorldAudio* audio = m_State != nullptr ? EnsureAudio(*m_State) : nullptr;
    return audio != nullptr ? audio->Play(sound, settings, detail::VoicePlacement::None, Vec3{}, ActorId{}) : VoiceId{};
}

VoiceId World3D::PlaySoundAt(SoundId sound, Vec3 position, const SoundSettings& settings)
{
    if (!detail::IsFinite(position))
    {
        LogMessage(LogLevel::Error, "audio", "PlaySoundAt was given a non-finite position; nothing played.");
        return VoiceId{};
    }
    detail::WorldAudio* audio = m_State != nullptr ? EnsureAudio(*m_State) : nullptr;
    return audio != nullptr ? audio->Play(sound, settings, detail::VoicePlacement::Point, position, ActorId{})
                            : VoiceId{};
}

VoiceId World3D::PlaySoundAt(SoundId sound, Actor3D emitter, const SoundSettings& settings)
{
    ActorRecord3* record = detail::Resolve(m_State, emitter.GetId(), "PlaySoundAt");
    if (record == nullptr)
    {
        return VoiceId{};
    }
    detail::WorldAudio* audio = EnsureAudio(*m_State);
    return audio != nullptr ? audio->Play(sound, settings, detail::VoicePlacement::Actor, record->Current.Position,
                                          emitter.GetId())
                            : VoiceId{};
}

void World3D::StopSound(VoiceId voice, float fadeSeconds)
{
    if (m_State != nullptr && m_State->Audio != nullptr)
    {
        m_State->Audio->Stop(voice, fadeSeconds);
    }
}

void World3D::StopAllSounds(float fadeSeconds)
{
    if (m_State != nullptr && m_State->Audio != nullptr)
    {
        m_State->Audio->StopAll(fadeSeconds);
    }
}

bool World3D::IsAudible(VoiceId voice) const
{
    return m_State != nullptr && m_State->Audio != nullptr && m_State->Audio->IsAudible(voice);
}

bool World3D::IsOccluded(VoiceId voice) const
{
    return m_State != nullptr && m_State->Audio != nullptr && m_State->Audio->IsOccluded(voice);
}

bool World3D::IsPlaying(VoiceId voice) const
{
    return m_State != nullptr && m_State->Audio != nullptr && m_State->Audio->IsPlaying(voice);
}

void World3D::SetVoiceVolume(VoiceId voice, float volume)
{
    if (m_State != nullptr && m_State->Audio != nullptr)
    {
        m_State->Audio->SetVolume(voice, volume);
    }
}

void World3D::SetVoicePosition(VoiceId voice, Vec3 position)
{
    if (m_State != nullptr && m_State->Audio != nullptr)
    {
        m_State->Audio->SetPosition(voice, position);
    }
}

void World3D::PlayMusic(const std::string& path, float volume, float fadeSeconds)
{
    detail::WorldAudio* audio = m_State != nullptr ? EnsureAudio(*m_State) : nullptr;
    if (audio != nullptr)
    {
        audio->PlayMusic(path, volume, fadeSeconds);
    }
}

void World3D::StopMusic(float fadeSeconds)
{
    if (m_State != nullptr && m_State->Audio != nullptr)
    {
        m_State->Audio->StopMusic(fadeSeconds);
    }
}

void World3D::SetListener(Vec3 position, Vec3 forward, Vec3 up)
{
    detail::WorldAudio* audio = m_State != nullptr ? EnsureAudio(*m_State) : nullptr;
    if (audio != nullptr && detail::IsFinite(position) && detail::IsFinite(forward) && detail::IsFinite(up))
    {
        audio->SetListener(position, detail::NormalizeVector(forward), detail::NormalizeVector(up));
    }
}

AudioSettings& World3D::GetAudioSettings()
{
    static AudioSettings fallback;
    detail::WorldAudio* audio = m_State != nullptr ? EnsureAudio(*m_State) : nullptr;
    return audio != nullptr ? audio->Settings : fallback;
}

void World3D::SetAudioSettings(const AudioSettings& settings)
{
    detail::WorldAudio* audio = m_State != nullptr ? EnsureAudio(*m_State) : nullptr;
    if (audio != nullptr)
    {
        audio->Settings = settings;
    }
}

AudioStats World3D::GetAudioStats() const
{
    if (m_State == nullptr || m_State->Audio == nullptr)
    {
        return AudioStats{};
    }
    return m_State->Audio->GetStats();
}

void World3D::SetPhysicsDebugDraw(bool enabled)
{
    if (m_State != nullptr)
    {
        m_State->PhysicsDebugDraw = enabled;
    }
}

uint32_t World3D::GetLastDrawCallCount() const
{
    if (m_State == nullptr || m_State->Renderer == nullptr)
    {
        return 0;
    }
    return m_State->Renderer->GetDrawCallCount();
}

uint32_t World3D::GetDrawnInstanceCount() const
{
    if (m_State == nullptr || m_State->Renderer == nullptr)
    {
        return 0;
    }
    return m_State->Renderer->GetInstanceCount();
}

size_t World3D::GetPendingLoadCount() const
{
    return detail::LoadQueue::Get().Pending();
}

void World3D::WaitForLoads()
{
    detail::LoadQueue::Get().WaitForAll();
    if (m_State != nullptr)
    {
        FitArrivedModels(*m_State);
    }
}

uint32_t World3D::GetLargestBatchSize() const
{
    return (m_State != nullptr && m_State->Renderer != nullptr) ? m_State->Renderer->GetLargestBatch()
                                                                : 0;
}

bool World3D::IsRayTracingActive() const
{
    return m_State != nullptr && m_State->Renderer != nullptr && m_State->Renderer->IsRayTracingActive();
}

uint32_t World3D::GetPathTracedSampleCount() const
{
    return (m_State != nullptr && m_State->Renderer != nullptr) ? m_State->Renderer->GetPathTracedSamples()
                                                                : 0;
}

uint32_t World3D::GetCulledCount() const
{
    if (m_State == nullptr || m_State->Renderer == nullptr)
    {
        return 0;
    }
    return m_State->Renderer->GetCulledCount();
}

// ---------------------------------------------------------------------------
// Actor3D
// ---------------------------------------------------------------------------

// --- animation ---------------------------------------------------------------

namespace
{

// The model behind an actor when it has anything to animate (a skeleton,
// morph targets, or both), or null for a primitive, a rigid model, or one
// still loading in the background.
const detail::ModelMesh* AnimatedMesh(detail::World3DState* state, const ActorRecord3* record)
{
    if (state == nullptr || state->Models == nullptr || record == nullptr || record->ModelIndex < 0)
    {
        return nullptr;
    }

    const detail::ModelMesh* mesh = state->Models->Get(record->ModelIndex);
    if (mesh == nullptr || !mesh->Ready || !mesh->IsAnimated())
    {
        return nullptr;
    }
    return mesh;
}

// The layer contributing most to what is on screen, which is the one a
// question like "how far into the animation are we?" is really about.
detail::PoseLayer* LoudestLayer(detail::Pose& pose)
{
    detail::PoseLayer* loudest = nullptr;
    for (detail::PoseLayer& layer : pose.Layers)
    {
        if (loudest == nullptr || layer.Weight > loudest->Weight)
        {
            loudest = &layer;
        }
    }
    return loudest;
}

} // namespace

int Actor3D::GetAnimationCount() const
{
    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "GetAnimationCount");
    const detail::ModelMesh* mesh = AnimatedMesh(m_State, record);
    return mesh != nullptr ? static_cast<int>(mesh->Animations.size()) : 0;
}

const char* Actor3D::GetAnimationName(int index) const
{
    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "GetAnimationName");
    const detail::ModelMesh* mesh = AnimatedMesh(m_State, record);

    if (mesh == nullptr || index < 0 || index >= static_cast<int>(mesh->Animations.size()))
    {
        return "";
    }
    return mesh->Animations[static_cast<size_t>(index)].Name.c_str();
}

int Actor3D::FindAnimation(const char* name) const
{
    if (name == nullptr)
    {
        return -1;
    }

    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "FindAnimation");
    const detail::ModelMesh* mesh = AnimatedMesh(m_State, record);
    if (mesh == nullptr)
    {
        return -1;
    }

    for (size_t index = 0; index < mesh->Animations.size(); ++index)
    {
        if (mesh->Animations[index].Name == name)
        {
            return static_cast<int>(index);
        }
    }
    return -1;
}

float Actor3D::GetAnimationDuration(int index) const
{
    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "GetAnimationDuration");
    const detail::ModelMesh* mesh = AnimatedMesh(m_State, record);

    if (mesh == nullptr || index < 0 || index >= static_cast<int>(mesh->Animations.size()))
    {
        return 0.0f;
    }
    return mesh->Animations[static_cast<size_t>(index)].Duration;
}

void Actor3D::Play(const AnimationPlay& play)
{
    ActorRecord3* record = detail::Resolve(m_State, m_Id, "Play");
    if (record == nullptr)
    {
        return;
    }

    const detail::ModelMesh* mesh = AnimatedMesh(m_State, record);
    if (mesh == nullptr)
    {
        // A model still loading in the background is not an error: this is the
        // frame before it lands, and the caller has no way to know that.
        if (record->AwaitingModel)
        {
            return;
        }

        LogMessage(LogLevel::Warning, "actor",
                           "Play on \"%s\", which has no skeleton or morph targets to animate.",
                           record->Name.c_str());
        return;
    }

    int index = play.Index;
    if (play.Name != nullptr && play.Name[0] != '\0')
    {
        index = FindAnimation(play.Name);
        if (index < 0)
        {
            LogMessage(LogLevel::Warning, "actor",
                               "\"%s\" has no animation named \"%s\". It has %zu: try GetAnimationName.",
                               record->Name.c_str(), play.Name, mesh->Animations.size());
            return;
        }
    }

    if (index < 0 || index >= static_cast<int>(mesh->Animations.size()))
    {
        LogMessage(LogLevel::Warning, "actor",
                           "Play on \"%s\" asked for animation %d of %zu.", record->Name.c_str(), index,
                           mesh->Animations.size());
        return;
    }

    if (!detail::IsFinite(play.Speed) || !detail::IsFinite(play.Fade) ||
        !detail::IsFinite(play.Weight) || !detail::IsFinite(play.StartTime))
    {
        LogMessage(LogLevel::Error, "actor",
                   "Play on \"%s\" was given a speed, fade, weight, or start time that is not a number.",
                   record->Name.c_str());
        return;
    }

    detail::StartLayer(record->Animation, index, play.Loop, play.Speed, play.StartTime,
                       std::max(0.0f, play.Fade), play.Weight);
}

void Actor3D::StopAnimation(float fadeSeconds)
{
    if (ActorRecord3* record = detail::Resolve(m_State, m_Id, "StopAnimation"))
    {
        detail::StopLayers(record->Animation, std::max(0.0f, fadeSeconds));
    }
}

bool Actor3D::IsAnimating() const
{
    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "IsAnimating");
    return record != nullptr && !record->Animation.Layers.empty();
}

bool Actor3D::IsAnimationFinished() const
{
    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "IsAnimationFinished");
    if (record == nullptr || record->Animation.Layers.empty())
    {
        return false;
    }

    for (const detail::PoseLayer& layer : record->Animation.Layers)
    {
        if (!layer.Finished)
        {
            return false;
        }
    }
    return true;
}

float Actor3D::GetAnimationTime() const
{
    ActorRecord3* record = detail::Resolve(m_State, m_Id, "GetAnimationTime");
    if (record == nullptr)
    {
        return 0.0f;
    }

    const detail::PoseLayer* loudest = LoudestLayer(record->Animation);
    return loudest != nullptr ? loudest->Time : 0.0f;
}

void Actor3D::SetAnimationTime(float seconds)
{
    ActorRecord3* record = detail::Resolve(m_State, m_Id, "SetAnimationTime");
    if (record == nullptr || !detail::IsFinite(seconds))
    {
        return;
    }

    if (detail::PoseLayer* loudest = LoudestLayer(record->Animation))
    {
        loudest->Time = seconds;
        loudest->Finished = false;
        record->Animation.Dirty = true;
    }
}

int Actor3D::GetJointCount() const
{
    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "GetJointCount");
    const detail::ModelMesh* mesh = AnimatedMesh(m_State, record);
    return mesh != nullptr ? static_cast<int>(mesh->Joints.size()) : 0;
}

const char* Actor3D::GetJointName(int index) const
{
    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "GetJointName");
    const detail::ModelMesh* mesh = AnimatedMesh(m_State, record);

    if (mesh == nullptr || index < 0 || index >= static_cast<int>(mesh->Joints.size()))
    {
        return "";
    }
    return mesh->Joints[static_cast<size_t>(index)].Name.c_str();
}

int Actor3D::FindJoint(const char* name) const
{
    if (name == nullptr)
    {
        return -1;
    }

    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "FindJoint");
    const detail::ModelMesh* mesh = AnimatedMesh(m_State, record);
    if (mesh == nullptr)
    {
        return -1;
    }

    for (size_t index = 0; index < mesh->Joints.size(); ++index)
    {
        if (mesh->Joints[index].Name == name)
        {
            return static_cast<int>(index);
        }
    }
    return -1;
}

Transform3 Actor3D::GetJointTransform(int index) const
{
    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "GetJointTransform");
    if (record == nullptr || index < 0 ||
        index >= static_cast<int>(record->Animation.Model.size()))
    {
        return Transform3{};
    }

    // The joint's place in the model, then the actor's own transform on top of
    // it, so the answer is where the hand is in the world rather than where it
    // is relative to the hips.
    const Mat4 world = Mat4::FromTransform(GetInterpolatedTransform()) *
                       record->Animation.Model[static_cast<size_t>(index)];
    return detail::DecomposeMatrix(world);
}

int Actor3D::GetMorphTargetCount() const
{
    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "GetMorphTargetCount");
    const detail::ModelMesh* mesh = AnimatedMesh(m_State, record);
    return mesh != nullptr ? static_cast<int>(mesh->MorphTargets.size()) : 0;
}

const char* Actor3D::GetMorphTargetName(int index) const
{
    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "GetMorphTargetName");
    const detail::ModelMesh* mesh = AnimatedMesh(m_State, record);
    if (mesh == nullptr || index < 0 || index >= static_cast<int>(mesh->MorphTargets.size()))
    {
        return "";
    }
    return mesh->MorphTargets[static_cast<size_t>(index)].Name.c_str();
}

int Actor3D::FindMorphTarget(const char* name) const
{
    if (name == nullptr)
    {
        return -1;
    }

    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "FindMorphTarget");
    const detail::ModelMesh* mesh = AnimatedMesh(m_State, record);
    if (mesh == nullptr)
    {
        return -1;
    }

    for (size_t index = 0; index < mesh->MorphTargets.size(); ++index)
    {
        if (mesh->MorphTargets[index].Name == name)
        {
            return static_cast<int>(index);
        }
    }
    return -1;
}

void Actor3D::SetMorphWeight(int index, float weight)
{
    ActorRecord3* record = detail::Resolve(m_State, m_Id, "SetMorphWeight");
    const detail::ModelMesh* mesh = AnimatedMesh(m_State, record);
    if (mesh == nullptr)
    {
        if (record != nullptr && !record->AwaitingModel)
        {
            LogMessage(LogLevel::Warning, "actor", "SetMorphWeight on \"%s\", which has no morph targets.",
                       record->Name.c_str());
        }
        return;
    }

    if (index < 0 || index >= static_cast<int>(mesh->MorphTargets.size()))
    {
        LogMessage(LogLevel::Warning, "actor", "SetMorphWeight on \"%s\" asked for target %d of %zu.",
                   record->Name.c_str(), index, mesh->MorphTargets.size());
        return;
    }

    if (!detail::IsFinite(weight))
    {
        LogMessage(LogLevel::Error, "actor", "SetMorphWeight on \"%s\" was given a weight that is not a number.",
                   record->Name.c_str());
        return;
    }

    detail::Pose& pose = record->Animation;
    detail::EnsureMorphWeights(pose, mesh->MorphTargets);
    pose.MorphBase[static_cast<size_t>(index)] = weight;

    // With nothing playing there is nothing to mix: the weight is what is
    // drawn. Otherwise the next frame's blend takes it in.
    if (pose.Layers.empty())
    {
        pose.MorphWeights[static_cast<size_t>(index)] = weight;
    }
    else
    {
        pose.Dirty = true;
    }
}

void Actor3D::SetMorphWeight(const char* name, float weight)
{
    const int index = FindMorphTarget(name);
    if (index < 0)
    {
        const ActorRecord3* record = detail::Resolve(m_State, m_Id, "SetMorphWeight");
        if (record != nullptr && !record->AwaitingModel)
        {
            LogMessage(LogLevel::Warning, "actor",
                       "\"%s\" has no morph target named \"%s\". It has %d: try GetMorphTargetName.",
                       record->Name.c_str(), name != nullptr ? name : "", GetMorphTargetCount());
        }
        return;
    }
    SetMorphWeight(index, weight);
}

float Actor3D::GetMorphWeight(int index) const
{
    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "GetMorphWeight");
    const detail::ModelMesh* mesh = AnimatedMesh(m_State, record);
    if (mesh == nullptr || index < 0 || index >= static_cast<int>(mesh->MorphTargets.size()))
    {
        return 0.0f;
    }

    const std::vector<float>& weights = record->Animation.MorphWeights;
    return weights.size() == mesh->MorphTargets.size() ? weights[static_cast<size_t>(index)]
                                                       : mesh->MorphTargets[static_cast<size_t>(index)].DefaultWeight;
}

void Actor3D::SetNormalMap(TextureId texture, float strength)
{
    if (ActorRecord3* record = detail::Resolve(m_State, m_Id, "SetNormalMap"))
    {
        if (!detail::IsFinite(strength))
        {
            LogMessage(LogLevel::Error, "actor", "SetNormalMap on \"%s\" was given a strength that is not a number.",
                       record->Name.c_str());
            return;
        }
        record->Look.NormalMap = texture;
        record->Look.NormalStrength = std::max(0.0f, strength);
    }
}

void Actor3D::SetEmission(Color color, float strength)
{
    if (ActorRecord3* record = detail::Resolve(m_State, m_Id, "SetEmission"))
    {
        if (!detail::IsFinite(strength) || !detail::IsFinite(color.R) || !detail::IsFinite(color.G) ||
            !detail::IsFinite(color.B))
        {
            LogMessage(LogLevel::Error, "actor", "SetEmission on \"%s\" was given a value that is not a number.",
                       record->Name.c_str());
            return;
        }
        const float scale = std::max(0.0f, strength);
        record->Look.Emission = Color{ color.R * scale, color.G * scale, color.B * scale, 1.0f };
        record->Look.EmissionOverridden = true;
    }
}

void Actor3D::WhenCollided(CollisionHandler handler)
{
    if (ActorRecord3* record = detail::Resolve(m_State, m_Id, "WhenCollided"))
    {
        record->OnCollided = std::move(handler);
    }
}

void Actor3D::WhenEntered(TriggerHandler handler)
{
    if (ActorRecord3* record = detail::Resolve(m_State, m_Id, "WhenEntered"))
    {
        record->OnEntered = std::move(handler);
    }
}

void Actor3D::WhenExited(TriggerHandler handler)
{
    if (ActorRecord3* record = detail::Resolve(m_State, m_Id, "WhenExited"))
    {
        record->OnExited = std::move(handler);
    }
}

bool Actor3D::IsSensor() const
{
    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "IsSensor");
    return record != nullptr && record->IsSensor;
}

void Actor3D::SetCollisionFilter(uint64_t category, uint64_t mask)
{
    ActorRecord3* record = detail::Resolve(m_State, m_Id, "SetCollisionFilter");
    if (record == nullptr)
    {
        return;
    }

    const int shapeCount = b3Body_GetShapeCount(record->Body);
    if (shapeCount <= 0)
    {
        return;
    }

    // An actor may carry several shapes; the filter applies to all of them,
    // because filtering is a property of the actor rather than of one piece of
    // its collision geometry.
    std::vector<b3ShapeId> shapes(static_cast<size_t>(shapeCount));
    const int written = b3Body_GetShapes(record->Body, shapes.data(), shapeCount);

    for (int index = 0; index < written; ++index)
    {
        b3Filter filter = b3Shape_GetFilter(shapes[static_cast<size_t>(index)]);
        filter.categoryBits = category;
        filter.maskBits = mask;

        // Re-evaluating contacts immediately means a filter change takes effect
        // this step rather than whenever the pair happens to be revisited.
        b3Shape_SetFilter(shapes[static_cast<size_t>(index)], filter, true);
    }
}

Color Actor3D::GetColor() const
{
    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "GetColor");
    return record != nullptr ? record->Look.Tint : Color{};
}

void Actor3D::SetColor(Color color)
{
    if (ActorRecord3* record = detail::Resolve(m_State, m_Id, "SetColor"))
    {
        record->Look.Tint = color;
    }
}

void Actor3D::SetMaterial(MaterialId material)
{
    if (ActorRecord3* record = detail::Resolve(m_State, m_Id, "SetMaterial"))
    {
        // Slots mean different things in a different material, so per-actor
        // values do not carry across.
        if (record->Look.Material != material)
        {
            record->Look.ParamMask = 0;
        }
        record->Look.Material = material;
    }
}

MaterialId Actor3D::GetMaterial() const
{
    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "GetMaterial");
    return record != nullptr ? record->Look.Material : MaterialId{};
}

void Actor3D::SetUniform(const std::string& name, float x, float y, float z, float w)
{
    if (ActorRecord3* record = detail::Resolve(m_State, m_Id, "SetUniform"))
    {
        detail::SetActorUniform(record->Look, name, Color{ x, y, z, w }, false);
    }
}

void Actor3D::SetUniform(const std::string& name, Color color)
{
    if (ActorRecord3* record = detail::Resolve(m_State, m_Id, "SetUniform"))
    {
        detail::SetActorUniform(record->Look, name, color, true);
    }
}

void Actor3D::ClearUniforms()
{
    if (ActorRecord3* record = detail::Resolve(m_State, m_Id, "ClearUniforms"))
    {
        record->Look.ParamMask = 0;
    }
}

void Actor3D::SetTexture(TextureId texture)
{
    if (ActorRecord3* record = detail::Resolve(m_State, m_Id, "SetTexture"))
    {
        record->Look.Texture = texture;
    }
}

TextureId Actor3D::GetTexture() const
{
    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "GetTexture");
    return record != nullptr ? record->Look.Texture : TextureId{};
}

void Actor3D::SetTextureTiling(Vec2 repeat, Vec2 offset)
{
    if (ActorRecord3* record = detail::Resolve(m_State, m_Id, "SetTextureTiling"))
    {
        if (detail::IsFinite(repeat) && detail::IsFinite(offset))
        {
            record->Look.UVScale = repeat;
            record->Look.UVOffset = offset;
        }
    }
}

void Actor3D::SetRoughness(float roughness)
{
    if (ActorRecord3* record = detail::Resolve(m_State, m_Id, "SetRoughness"))
    {
        if (detail::IsFinite(roughness))
        {
            record->Look.Roughness = std::clamp(roughness, 0.0f, 1.0f);
            record->Look.SurfaceOverridden = true;
        }
    }
}

float Actor3D::GetRoughness() const
{
    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "GetRoughness");
    return record != nullptr ? record->Look.Roughness : 0.0f;
}

void Actor3D::SetMetallic(float metallic)
{
    if (ActorRecord3* record = detail::Resolve(m_State, m_Id, "SetMetallic"))
    {
        if (detail::IsFinite(metallic))
        {
            record->Look.Metallic = std::clamp(metallic, 0.0f, 1.0f);
            record->Look.SurfaceOverridden = true;
        }
    }
}

float Actor3D::GetMetallic() const
{
    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "GetMetallic");
    return record != nullptr ? record->Look.Metallic : 0.0f;
}

bool Actor3D::IsVisible() const
{
    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "IsVisible");
    return record != nullptr && record->Look.Visible;
}

void Actor3D::SetVisible(bool visible)
{
    if (ActorRecord3* record = detail::Resolve(m_State, m_Id, "SetVisible"))
    {
        record->Look.Visible = visible;
    }
}

bool Actor3D::IsValid() const
{
    if (m_State == nullptr || !m_Id.IsValid() || m_Id.Index >= m_State->Actors.size())
    {
        return false;
    }

    const ActorRecord3& record = m_State->Actors[m_Id.Index];
    return record.Alive && record.Generation == m_Id.Generation;
}

const std::string& Actor3D::GetName() const
{
    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "GetName");
    return record != nullptr ? record->Name : g_EmptyName;
}

void Actor3D::SetName(const std::string& name)
{
    if (ActorRecord3* record = detail::Resolve(m_State, m_Id, "SetName"))
    {
        record->Name = name;
    }
}

Vec3 Actor3D::GetPosition() const
{
    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "GetPosition");
    return record != nullptr ? detail::FromB3Pos(b3Body_GetPosition(record->Body)) : Vec3{};
}

void Actor3D::SetPosition(Vec3 position)
{
    ActorRecord3* record = detail::Resolve(m_State, m_Id, "SetPosition");
    if (record == nullptr)
    {
        return;
    }

    if (!detail::IsFinite(position))
    {
        LogMessage(LogLevel::Error, "actor",
                   "SetPosition on \"%s\" was given a non-finite value and was rejected.",
                   record->Name.empty() ? "<unnamed>" : record->Name.c_str());
        return;
    }

    b3Body_SetTransform(record->Body, detail::ToB3Pos(position), b3Body_GetRotation(record->Body));

    // Teleporting is instantaneous, so both snapshots move together. Blending
    // from the old position would draw the actor sliding across the gap.
    record->Current = ReadTransform(record->Body, record->Scale);
    record->Previous = record->Current;
}

Quat Actor3D::GetRotation() const
{
    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "GetRotation");
    return record != nullptr ? detail::FromB3(b3Body_GetRotation(record->Body)) : Quat{};
}

void Actor3D::SetRotation(Quat rotation)
{
    ActorRecord3* record = detail::Resolve(m_State, m_Id, "SetRotation");
    if (record == nullptr)
    {
        return;
    }

    if (!detail::IsFinite(rotation))
    {
        LogMessage(LogLevel::Error, "actor",
                   "SetRotation on \"%s\" was given a non-finite value and was rejected.",
                   record->Name.empty() ? "<unnamed>" : record->Name.c_str());
        return;
    }

    b3Body_SetTransform(record->Body, b3Body_GetPosition(record->Body), detail::ToB3(rotation));
    record->Current = ReadTransform(record->Body, record->Scale);
    record->Previous = record->Current;
}

Vec3 Actor3D::GetScale() const
{
    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "GetScale");
    return record != nullptr ? record->Scale : Vec3{ 1.0f, 1.0f, 1.0f };
}

Vec3 Actor3D::GetLinearVelocity() const
{
    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "GetLinearVelocity");
    return record != nullptr ? detail::FromB3(b3Body_GetLinearVelocity(record->Body)) : Vec3{};
}

void Actor3D::SetLinearVelocity(Vec3 velocity)
{
    ActorRecord3* record = detail::Resolve(m_State, m_Id, "SetLinearVelocity");
    if (record == nullptr || !detail::IsFinite(velocity))
    {
        return;
    }
    b3Body_SetLinearVelocity(record->Body, detail::ToB3(velocity));
}

Vec3 Actor3D::GetAngularVelocity() const
{
    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "GetAngularVelocity");
    return record != nullptr ? detail::FromB3(b3Body_GetAngularVelocity(record->Body)) : Vec3{};
}

void Actor3D::SetAngularVelocity(Vec3 velocity)
{
    ActorRecord3* record = detail::Resolve(m_State, m_Id, "SetAngularVelocity");
    if (record == nullptr || !detail::IsFinite(velocity))
    {
        return;
    }
    b3Body_SetAngularVelocity(record->Body, detail::ToB3(velocity));
}

void Actor3D::ApplyForce(Vec3 force)
{
    ActorRecord3* record = detail::Resolve(m_State, m_Id, "ApplyForce");
    if (record == nullptr || !detail::IsFinite(force))
    {
        return;
    }
    b3Body_ApplyForceToCenter(record->Body, detail::ToB3(force), true);
}

void Actor3D::ApplyImpulse(Vec3 impulse)
{
    ActorRecord3* record = detail::Resolve(m_State, m_Id, "ApplyImpulse");
    if (record == nullptr || !detail::IsFinite(impulse))
    {
        return;
    }
    b3Body_ApplyLinearImpulseToCenter(record->Body, detail::ToB3(impulse), true);
}

void Actor3D::ApplyTorque(Vec3 torque)
{
    ActorRecord3* record = detail::Resolve(m_State, m_Id, "ApplyTorque");
    if (record == nullptr || !detail::IsFinite(torque))
    {
        return;
    }
    b3Body_ApplyTorque(record->Body, detail::ToB3(torque), true);
}

float Actor3D::GetMass() const
{
    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "GetMass");
    return record != nullptr ? b3Body_GetMass(record->Body) : 0.0f;
}

bool Actor3D::IsAwake() const
{
    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "IsAwake");
    return record != nullptr && b3Body_IsAwake(record->Body);
}

void Actor3D::SetAwake(bool awake)
{
    if (ActorRecord3* record = detail::Resolve(m_State, m_Id, "SetAwake"))
    {
        b3Body_SetAwake(record->Body, awake);
    }
}

Transform3 Actor3D::GetInterpolatedTransform() const
{
    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "GetInterpolatedTransform");
    if (record == nullptr)
    {
        return Transform3{};
    }
    return detail::LerpTransform(record->Previous, record->Current, m_State->Alpha);
}

void Actor3D::SetBodyType(BodyType type)
{
    if (detail::Resolve(m_State, m_Id, "SetBodyType") == nullptr)
    {
        return;
    }

    detail::Command command;
    command.Type = detail::CommandType::SetBodyType;
    command.Id = m_Id;
    command.NewBodyType = type;
    m_State->Commands.push_back(command);
}

void Actor3D::Destroy()
{
    if (detail::Resolve(m_State, m_Id, "Destroy") == nullptr)
    {
        return;
    }

    // Deferred, so this is safe to call from a callback that is iterating the
    // world. The actor stays usable until the next sync point; after that
    // every handle to it is stale.
    //
    // Anything parented to it goes too, deepest first.
    detail::QueueSubtreeDestroy(*m_State, m_Id, m_State->Commands);
}

} // namespace ludifex
