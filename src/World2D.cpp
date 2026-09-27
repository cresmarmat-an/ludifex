#include "Jobs.h"
#include "Audio.h"
#include "HostProtocol.h"
#include "Render3D.h"
#include "WorldTextures.h"

#include <algorithm>
#include <cmath>
#include <vector>

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

using detail::ActorRecord2;
using detail::ShapeKind2;
using detail::World2DState;

const std::string g_EmptyName;

Transform2 ReadTransform(b2BodyId body, const Vec2& scale)
{
    Transform2 transform;
    transform.Position = detail::FromB2(b2Body_GetPosition(body));
    transform.Rotation = b2Rot_GetAngle(b2Body_GetRotation(body));
    transform.Scale = scale;
    return transform;
}

ActorId RegisterActor(World2DState& state, b2BodyId body, const Vec2& scale, const std::string& name,
                      ShapeKind2 shape, bool isSensor = false)
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

    ActorRecord2& record = state.Actors[index];
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
    record.IsSprite = false;
    record.Sequence = state.NextSequence++;

    // Static geometry reads as the ground and dynamic bodies as objects, which
    // makes a scene legible before anything has been styled.
    record.Look.Reset((b2Body_GetType(body) == b2_staticBody) ? Color{ 0.28f, 0.31f, 0.38f, 1.0f }
                                                              : Color{ 0.89f, 0.64f, 0.31f, 1.0f });
    record.OnCollided = {};
    record.IsSensor = isSensor;
    record.IsCharacter = false;
    record.OnEntered = {};
    record.OnExited = {};

    // A trigger volume is usually meant to be felt, not seen, so a sensor
    // starts hidden. SetVisible(true) puts it back on screen.
    record.Look.Visible = !isSensor;

    // Contact events hand back a shape, not an actor, so the body carries the
    // index needed to get back here.
    b2Body_SetUserData(body, detail::PackActorIndex(index));
    record.Current = ReadTransform(body, scale);
    record.Previous = record.Current;

    ++state.LiveCount;

    return ActorId{ index, record.Generation };
}

b2BodyId CreateBody(World2DState& state, const Vec2& position, float rotation,
                    BodyType type, bool isBullet)
{
    b2BodyDef bodyDef = b2DefaultBodyDef();
    bodyDef.type = detail::ToB2BodyType(type);
    bodyDef.position = detail::ToB2(position);
    bodyDef.rotation = b2MakeRot(rotation);
    bodyDef.isBullet = isBullet;
    bodyDef.enableSleep = state.Config.EnableSleep;
    return b2CreateBody(state.World, &bodyDef);
}

// Box2D hands over a range to split, unlike Box3D's whole-task model, which is
// why the scheduler offers both shapes.
void* EnqueueTask(b2TaskCallback* task, int itemCount, int minRange, void* taskContext,
                  void* userContext)
{
    auto* jobs = static_cast<detail::JobSystem*>(userContext);
    return jobs->SubmitRange(task, itemCount, minRange, taskContext);
}

void FinishTask(void* userTask, void* userContext)
{
    auto* jobs = static_cast<detail::JobSystem*>(userContext);
    jobs->Wait(userTask);
}

b2ShapeDef MakeShapeDef(float density, float friction, float restitution, bool isSensor = false)
{
    b2ShapeDef shapeDef = b2DefaultShapeDef();
    shapeDef.density = density;
    shapeDef.material.friction = friction;
    shapeDef.material.restitution = restitution;

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

// Impacts, dispatched after the step rather than from inside it, so a handler
// may create and destroy actors freely.
void DispatchCollisions(World2DState& state, World2D& world)
{
    const bool wantsEvents = static_cast<bool>(state.OnAnyCollision);

    const b2ContactEvents events = b2World_GetContactEvents(state.World);

    for (int index = 0; index < events.hitCount; ++index)
    {
        const b2ContactHitEvent& hit = events.hitEvents[index];

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

        CollisionInfo2D info;
        info.Point = detail::FromB2(hit.point);
        info.Normal = detail::FromB2(hit.normal);
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
            info.Normal = Vec2{ -info.Normal.X, -info.Normal.Y };
            CallHandler(state.Actors[idB.Index].OnCollided, info);
        }

        if (wantsEvents)
        {
            info.Self = world.GetActor(idA);
            info.Other = world.GetActor(idB);
            info.Normal = detail::FromB2(hit.normal);
            CallHandler(state.OnAnyCollision, info);
        }
    }
}

// Overlap notifications, under the same rule as collisions. An end event can
// name a shape that no longer exists, so both sides are resolved through the
// handle system and a destroyed actor does not report leaving.
void DispatchSensors(World2DState& state, World2D& world)
{
    const b2SensorEvents events = b2World_GetSensorEvents(state.World);

    for (int index = 0; index < events.beginCount; ++index)
    {
        const b2SensorBeginTouchEvent& begin = events.beginEvents[index];

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

        TriggerInfo2D info;
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
        const b2SensorEndTouchEvent& end = events.endEvents[index];

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

        TriggerInfo2D info;
        info.Sensor = world.GetActor(sensorId);
        info.Other = world.GetActor(visitorId);

        if (hasOwn)
        {
            CallHandler(state.Actors[sensorId.Index].OnExited, info);
        }

        CallHandler(state.OnAnyExit, info);
    }
}


// The closest hit along a ray, ignoring sensors, for the same reason as in a
// 3D world: a trigger volume is not something a ray should stop at.
struct ClosestSolidHit
{
    b2ShapeId Shape{};
    b2Vec2 Point{};
    b2Vec2 Normal{};
    float Fraction = 1.0f;
    bool Found = false;
};

float ClosestSolidCallback(b2ShapeId shapeId, b2Vec2 point, b2Vec2 normal, float fraction, void* context)
{
    if (b2Shape_IsSensor(shapeId))
    {
        return -1.0f; // filter it out and keep going
    }

    auto* hit = static_cast<ClosestSolidHit*>(context);
    hit->Shape = shapeId;
    hit->Point = point;
    hit->Normal = normal;
    hit->Fraction = fraction;
    hit->Found = true;
    return fraction;
}

// Everything a 2D world's audio needs each frame. The listener sits at the
// camera's centre in the plane, facing into the screen, so a sound to the
// right of the view is heard on the right.
void UpdateAudio(World2D& world, World2DState& state, float deltaSeconds)
{
    if (state.Audio == nullptr)
    {
        return;
    }

    detail::AudioFrame frame;
    frame.DeltaSeconds = deltaSeconds;
    frame.HasCamera = true;
    frame.CameraPosition = Vec3{ state.Camera.Center.X, state.Camera.Center.Y, 0.0f };
    frame.CameraForward = Vec3{ 0.0f, 0.0f, -1.0f };
    frame.CameraUp = Vec3{ 0.0f, 1.0f, 0.0f };

    frame.ResolveActor = [&state](const ActorId& id, Vec3& position, Vec3& velocity, Vec3& forward) {
        if (!id.IsValid() || id.Index >= state.Actors.size())
        {
            return false;
        }
        const ActorRecord2& record = state.Actors[id.Index];
        if (!record.Alive || record.Generation != id.Generation)
        {
            return false;
        }
        const Transform2 pose = detail::LerpTransform(record.Previous, record.Current, state.Alpha);
        position = Vec3{ pose.Position.X, pose.Position.Y, 0.0f };
        const b2Vec2 linear = b2Body_GetLinearVelocity(record.Body);
        velocity = Vec3{ linear.x, linear.y, 0.0f };

        // A 2D emitter faces along its own +X.
        forward = Vec3{ std::cos(pose.Rotation), std::sin(pose.Rotation), 0.0f };
        return true;
    };

    frame.IsBlocked = [&world](const Vec3& from, const Vec3& to, const ActorId& ignore) {
        const Vec2 offset{ to.X - from.X, to.Y - from.Y };
        const float distance = std::sqrt(offset.X * offset.X + offset.Y * offset.Y);
        if (distance < 0.05f)
        {
            return false;
        }
        const RayHit2D hit = world.CastRay(Vec2{ from.X, from.Y }, offset, distance);
        return hit.Hit && hit.Actor.GetId() != ignore && hit.Distance < distance - 0.05f;
    };

    state.Audio->Update(frame);
}

detail::WorldAudio* EnsureAudio(World2DState& state)
{
    if (state.Audio == nullptr)
    {
        state.Audio = new detail::WorldAudio();
    }
    return state.Audio->IsReady() ? state.Audio : nullptr;
}

} // namespace

namespace detail
{

// Recovers the actor a shape belongs to, or an invalid handle when the body is
// gone or was never registered.
ActorId ActorIdForShape(World2DState& state, b2ShapeId shapeId)
{
    if (!b2Shape_IsValid(shapeId))
    {
        return ActorId{};
    }

    const b2BodyId body = b2Shape_GetBody(shapeId);
    if (!b2Body_IsValid(body))
    {
        return ActorId{};
    }

    uint32_t index = 0;
    if (!detail::UnpackActorIndex(b2Body_GetUserData(body), index))
    {
        return ActorId{};
    }

    if (index >= state.Actors.size() || !state.Actors[index].Alive)
    {
        return ActorId{};
    }

    return ActorId{ index, state.Actors[index].Generation };
}

Actor2D MakeActor(World2DState* state, const ActorId& id)
{
    Actor2D actor;
    if (Resolve(state, id, "MakeActor") != nullptr)
    {
        actor.m_State = state;
        actor.m_Id = id;
    }
    return actor;
}

} // namespace detail

// ---------------------------------------------------------------------------
// World lifetime
// ---------------------------------------------------------------------------

World2D CreateWorld2D(const World2DConfig& config)
{
    World2D world;

    auto* state = new World2DState();
    state->Config = config;

    // A 2D scene is seen face on, so its light comes from in front of the
    // plane, above and to one side: shapes read bright with a modelled edge,
    // where the 3D default (a sun from above) would only graze them.
    state->Render.Light.Direction = Vec3{ -0.35f, -0.5f, -1.0f };

    // A 2D scene has no ground plane to catch directional shadows.
    state->Render.Shadows.Enabled = false;

    if (!detail::IsFinite(config.Gravity))
    {
        LogMessage(LogLevel::Error, "world",
                   "Gravity is not finite; falling back to the default of {0, -10}.");
        state->Config.Gravity = Vec2{ 0.0f, -10.0f };
    }

    if (!(state->Config.FixedTimeStep > 0.0f) || !detail::IsFinite(state->Config.FixedTimeStep))
    {
        LogMessage(LogLevel::Error, "world",
                   "FixedTimeStep must be a positive finite number; falling back to 1/60 s.");
        state->Config.FixedTimeStep = 1.0f / 60.0f;
    }

    state->Config.SubStepCount = std::max(1, state->Config.SubStepCount);
    state->Config.MaxStepsPerFrame = std::max(1, state->Config.MaxStepsPerFrame);

    b2WorldDef worldDef = b2DefaultWorldDef();
    worldDef.gravity = detail::ToB2(state->Config.Gravity);
    worldDef.enableSleep = state->Config.EnableSleep;
    worldDef.enableContinuous = state->Config.EnableContinuous;

    // Determinism pins this to one worker; see the note in World3D.
    uint32_t requested = state->Config.Deterministic ? 1u : state->Config.WorkerCount;

    if (requested != 1)
    {
        detail::JobSystem& jobs = detail::GetJobSystem();
        jobs.Start(requested);

        worldDef.workerCount = static_cast<int>(jobs.GetWorkerCount());
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

    state->World = b2CreateWorld(&worldDef);
    if (!b2World_IsValid(state->World))
    {
        LogMessage(LogLevel::Error, "world", "Box2D world creation failed.");
        delete state;
        return world;
    }

    state->PhysicsRunning = state->Config.PhysicsEnabled;

    LogMessage(LogLevel::Info, "world",
               "Created a 2D world: gravity {%.2f, %.2f}, %d sub-steps at %.1f Hz%s.",
               static_cast<double>(state->Config.Gravity.X),
               static_cast<double>(state->Config.Gravity.Y),
               state->Config.SubStepCount,
               static_cast<double>(1.0f / state->Config.FixedTimeStep),
               state->Config.Deterministic ? ", deterministic" : "");

    world.m_State = state;
    return world;
}

World2D::~World2D()
{
    if (m_State != nullptr)
    {
        delete m_State->Audio;
        m_State->Audio = nullptr;

        if (m_State->Renderer != nullptr)
        {
            m_State->Renderer->Shutdown();
            delete m_State->Renderer;
            m_State->Renderer = nullptr;
        }

        if (b2World_IsValid(m_State->World))
        {
            b2DestroyWorld(m_State->World);
        }
        delete m_State;
        m_State = nullptr;
    }
}

World2D::World2D(World2D&& other) noexcept
    : m_State(other.m_State)
{
    other.m_State = nullptr;
}

World2D& World2D::operator=(World2D&& other) noexcept
{
    if (this != &other)
    {
        this->~World2D();
        m_State = other.m_State;
        other.m_State = nullptr;
    }
    return *this;
}

// ---------------------------------------------------------------------------
// Actor creation
// ---------------------------------------------------------------------------

Actor2D World2D::AddRectangle(const RectangleDesc& desc)
{
    Actor2D actor;
    if (m_State == nullptr)
    {
        return actor;
    }

    if (!detail::IsFinite(desc.Position) || !detail::IsFinite(desc.Rotation))
    {
        LogMessage(LogLevel::Error, "actor",
                   "AddRectangle was given a non-finite transform; no actor was created.");
        return actor;
    }

    if (!detail::ValidateExtent(desc.Width, "Rectangle width") ||
        !detail::ValidateExtent(desc.Height, "Rectangle height"))
    {
        return actor;
    }

    const b2BodyId body = CreateBody(*m_State, desc.Position, desc.Rotation, desc.Type, desc.IsBullet);

    const b2Polygon polygon = b2MakeBox(desc.Width * 0.5f, desc.Height * 0.5f);
    b2ShapeDef shapeDef = MakeShapeDef(desc.Density, desc.Friction, desc.Restitution, desc.IsSensor);
    b2CreatePolygonShape(body, &shapeDef, &polygon);

    actor.m_State = m_State;
    actor.m_Id = RegisterActor(*m_State, body, Vec2{ desc.Width, desc.Height }, desc.Name,
                               ShapeKind2::Rectangle, desc.IsSensor);
    return actor;
}

Actor2D World2D::AddCircle(const CircleDesc& desc)
{
    Actor2D actor;
    if (m_State == nullptr)
    {
        return actor;
    }

    if (!detail::IsFinite(desc.Position))
    {
        LogMessage(LogLevel::Error, "actor", "AddCircle was given a non-finite position; no actor was created.");
        return actor;
    }

    if (!detail::ValidateExtent(desc.Radius * 2.0f, "Circle diameter"))
    {
        return actor;
    }

    const b2BodyId body = CreateBody(*m_State, desc.Position, 0.0f, desc.Type, desc.IsBullet);

    b2Circle circle;
    circle.center = b2Vec2{ 0.0f, 0.0f };
    circle.radius = desc.Radius;

    b2ShapeDef shapeDef = MakeShapeDef(desc.Density, desc.Friction, desc.Restitution, desc.IsSensor);
    b2CreateCircleShape(body, &shapeDef, &circle);

    const Vec2 scale{ desc.Radius * 2.0f, desc.Radius * 2.0f };

    actor.m_State = m_State;
    actor.m_Id = RegisterActor(*m_State, body, scale, desc.Name, ShapeKind2::Circle, desc.IsSensor);
    return actor;
}

Actor2D World2D::AddCapsule(const Capsule2DDesc& desc)
{
    Actor2D actor;
    if (m_State == nullptr)
    {
        return actor;
    }

    if (!detail::IsFinite(desc.Position) || !detail::IsFinite(desc.Rotation))
    {
        LogMessage(LogLevel::Error, "actor", "AddCapsule was given a non-finite transform; no actor was created.");
        return actor;
    }

    if (!detail::ValidateExtent(desc.Radius * 2.0f, "Capsule diameter") ||
        !detail::ValidateExtent(desc.Height, "Capsule height"))
    {
        return actor;
    }

    // A capsule shorter than its own width is a circle; it is made one rather
    // than refused.
    const float radius = desc.Radius;
    const float height = std::max(desc.Height, radius * 2.0f);
    const float half = height * 0.5f - radius;

    const b2BodyId body = CreateBody(*m_State, desc.Position, desc.Rotation, desc.Type, desc.IsBullet);

    b2ShapeDef shapeDef = MakeShapeDef(desc.Density, desc.Friction, desc.Restitution, desc.IsSensor);
    if (half > 1e-4f)
    {
        b2Capsule capsule;
        capsule.center1 = b2Vec2{ 0.0f, -half };
        capsule.center2 = b2Vec2{ 0.0f, half };
        capsule.radius = radius;
        b2CreateCapsuleShape(body, &shapeDef, &capsule);
    }
    else
    {
        b2Circle circle;
        circle.center = b2Vec2{ 0.0f, 0.0f };
        circle.radius = radius;
        b2CreateCircleShape(body, &shapeDef, &circle);
    }

    actor.m_State = m_State;
    actor.m_Id = RegisterActor(*m_State, body, Vec2{ radius * 2.0f, height }, desc.Name, ShapeKind2::Capsule,
                               desc.IsSensor);
    return actor;
}

Actor2D World2D::AddGround(const Ground2DDesc& desc)
{
    RectangleDesc rectangle;
    rectangle.Width = desc.Width;
    rectangle.Height = desc.Thickness;

    // As in 3D, Position names the top surface rather than the center.
    rectangle.Position = Vec2{ desc.Position.X, desc.Position.Y - desc.Thickness * 0.5f };
    rectangle.Type = BodyType::Static;
    rectangle.Friction = desc.Friction;
    rectangle.Name = desc.Name;

    return AddRectangle(rectangle);
}

Actor2D World2D::GetActor(ActorId id)
{
    Actor2D actor;
    if (detail::Resolve(m_State, id, "GetActor") != nullptr)
    {
        actor.m_State = m_State;
        actor.m_Id = id;
    }
    return actor;
}

void World2D::ForEachActor(const std::function<void(Actor2D&)>& visit)
{
    if (m_State == nullptr || !visit)
    {
        return;
    }

    for (size_t index = 0; index < m_State->Actors.size(); ++index)
    {
        const ActorRecord2& record = m_State->Actors[index];
        if (!record.Alive)
        {
            continue;
        }

        Actor2D actor;
        actor.m_State = m_State;
        actor.m_Id = ActorId{ static_cast<uint32_t>(index), record.Generation };
        visit(actor);
    }
}

// ---------------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------------

void World2D::ApplyPendingChanges()
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

        detail::JointRecord2& record = m_State->Joints[id.Index];
        if (!record.Alive || record.Generation != id.Generation)
        {
            continue;
        }

        if (b2Joint_IsValid(record.Joint))
        {
            b2DestroyJoint(record.Joint);
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

    if (m_State->Commands.empty())
    {
        return;
    }

    bool destroyedAny = false;

    for (size_t i = 0; i < m_State->Commands.size(); ++i)
    {
        const detail::Command command = m_State->Commands[i];
        ActorRecord2* record = detail::Resolve(m_State, command.Id, "A queued world change");
        if (record == nullptr)
        {
            continue;
        }

        switch (command.Type)
        {
            case detail::CommandType::Destroy:
            {
                if (b2Body_IsValid(record->Body))
                {
                    b2DestroyBody(record->Body);
                }

                record->Alive = false;
                record->Name.clear();
                record->IsCharacter = false;

                // Releasing the handlers here means a lambda that captured
                // other actors stops holding them the moment this one dies.
                record->OnCollided = {};
                record->OnEntered = {};
                record->OnExited = {};

                ++record->Generation;
                if (record->Generation == 0)
                {
                    record->Generation = 1;
                }

                m_State->FreeIndices.push_back(command.Id.Index);
                --m_State->LiveCount;
                destroyedAny = true;
                break;
            }

            case detail::CommandType::SetBodyType:
            {
                b2Body_SetType(record->Body, detail::ToB2BodyType(command.NewBodyType));
                break;
            }

            // A 2D world has no hierarchy; nothing queues this.
            case detail::CommandType::SetParent:
                break;
        }
    }

    m_State->Commands.clear();

    // Box2D destroys a body's joints along with it, so their records are
    // reclaimed here, after the bodies have gone.
    if (destroyedAny && m_State->LiveJoints > 0)
    {
        for (size_t index = 0; index < m_State->Joints.size(); ++index)
        {
            detail::JointRecord2& record = m_State->Joints[index];
            if (record.Alive && !b2Joint_IsValid(record.Joint))
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

void World2D::StepPhysics(float timeStep)
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

    for (ActorRecord2& record : m_State->Actors)
    {
        if (record.Alive)
        {
            record.Previous = record.Current;
        }
    }

    b2World_Step(m_State->World, timeStep, m_State->Config.SubStepCount);

    for (ActorRecord2& record : m_State->Actors)
    {
        if (record.Alive)
        {
            record.Current = ReadTransform(record.Body, record.Scale);
        }
    }

    ++m_State->StepCount;

    // Events run between two sync points: the first so a handler sees a
    // settled world, the second so a handler that destroys an actor has its
    // request applied immediately afterwards rather than a frame later.
    ApplyPendingChanges();

    DispatchCollisions(*m_State, *this);
    DispatchSensors(*m_State, *this);

    ApplyPendingChanges();
}

void World2D::Update(float deltaSeconds)
{
    if (m_State == nullptr)
    {
        return;
    }

    // Physics first, then sound, so a sound on a moving actor is where the
    // actor is.
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

    UpdateAudio(*this, *m_State, detail::IsFinite(deltaSeconds) ? std::max(0.0f, deltaSeconds) : 0.0f);
}

void World2D::StartPhysics()
{
    if (m_State != nullptr)
    {
        m_State->PhysicsRunning = true;
        m_State->Accumulator = 0.0f;
    }
}

void World2D::StopPhysics()
{
    if (m_State != nullptr)
    {
        m_State->PhysicsRunning = false;
    }
}

bool World2D::IsPhysicsRunning() const
{
    return m_State != nullptr && m_State->PhysicsRunning;
}

void World2D::SetPhysicsMode(PhysicsMode mode)
{
    if (m_State != nullptr)
    {
        m_State->Mode = mode;
        m_State->Accumulator = 0.0f;
    }
}

PhysicsMode World2D::GetPhysicsMode() const
{
    return m_State != nullptr ? m_State->Mode : PhysicsMode::Automatic;
}

void World2D::SetGravity(Vec2 gravity)
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
    b2World_SetGravity(m_State->World, detail::ToB2(gravity));
}

Vec2 World2D::GetGravity() const
{
    return m_State != nullptr ? m_State->Config.Gravity : Vec2{};
}

void World2D::SetFixedTimeStep(float seconds)
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

float World2D::GetFixedTimeStep() const
{
    return m_State != nullptr ? m_State->Config.FixedTimeStep : 0.0f;
}

void World2D::SetSubStepCount(int count)
{
    if (m_State != nullptr)
    {
        m_State->Config.SubStepCount = std::max(1, count);
    }
}

int World2D::GetSubStepCount() const
{
    return m_State != nullptr ? m_State->Config.SubStepCount : 0;
}

void World2D::SetInterpolationAlpha(float alpha)
{
    if (m_State != nullptr && detail::IsFinite(alpha))
    {
        m_State->Alpha = std::clamp(alpha, 0.0f, 1.0f);
    }
}

float World2D::GetInterpolationAlpha() const
{
    return m_State != nullptr ? m_State->Alpha : 0.0f;
}

size_t World2D::GetActorCount() const
{
    return m_State != nullptr ? m_State->LiveCount : 0;
}

uint64_t World2D::GetStepCount() const
{
    return m_State != nullptr ? m_State->StepCount : 0;
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

void World2D::WhenActorCollided(CollisionHandler2D handler)
{
    if (m_State != nullptr)
    {
        m_State->OnAnyCollision = std::move(handler);
    }
}

void World2D::SetCollisionThreshold(float metersPerSecond)
{
    if (m_State != nullptr)
    {
        m_State->CollisionThreshold = std::max(0.0f, metersPerSecond);
    }
}

void World2D::WhenActorEnteredTrigger(TriggerHandler2D handler)
{
    if (m_State != nullptr)
    {
        m_State->OnAnyEnter = std::move(handler);
    }
}

void World2D::WhenActorLeftTrigger(TriggerHandler2D handler)
{
    if (m_State != nullptr)
    {
        m_State->OnAnyExit = std::move(handler);
    }
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

RayHit2D World2D::CastRay(Vec2 origin, Vec2 direction, float maxDistance)
{
    RayHit2D result;

    if (m_State == nullptr)
    {
        return result;
    }

    if (!detail::IsFinite(origin) || !detail::IsFinite(direction) ||
        !detail::IsFinite(maxDistance) || maxDistance <= 0.0f)
    {
        LogMessage(LogLevel::Error, "query", "CastRay was given a non-finite ray; no hit is reported.");
        return result;
    }

    const float length = std::sqrt(direction.X * direction.X + direction.Y * direction.Y);
    if (length <= 0.0f)
    {
        LogMessage(LogLevel::Error, "query", "CastRay was given a zero-length direction.");
        return result;
    }

    const Vec2 translation{ direction.X / length * maxDistance, direction.Y / length * maxDistance };

    ClosestSolidHit hit;
    b2World_CastRay(m_State->World, detail::ToB2(origin), detail::ToB2(translation),
                    b2DefaultQueryFilter(), ClosestSolidCallback, &hit);

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
    result.Point = detail::FromB2(hit.Point);
    result.Normal = detail::FromB2(hit.Normal);
    result.Fraction = hit.Fraction;
    result.Distance = hit.Fraction * maxDistance;
    return result;
}

Vec2 World2D::ViewToWorld(float normalizedX, float normalizedY) const
{
    if (m_State == nullptr)
    {
        return Vec2{};
    }

    // Without a render size there is no aspect ratio to undo, so a square view
    // is assumed. Calling this on a world that never renders is reported as a
    // mistake.
    float aspect = 1.0f;
    if (m_State->RenderWidth > 0 && m_State->RenderHeight > 0)
    {
        aspect = static_cast<float>(m_State->RenderWidth) / static_cast<float>(m_State->RenderHeight);
    }
    else
    {
        LogMessage(LogLevel::Warning, "query",
                   "ViewToWorld was called before SetRenderSize, so a square view is assumed.");
    }

    const Camera2D& camera = m_State->Camera;

    // Y is flipped because view coordinates count down from the top while the
    // world counts up.
    return Vec2{ camera.Center.X + (normalizedX - 0.5f) * camera.Height * aspect,
                 camera.Center.Y - (normalizedY - 0.5f) * camera.Height };
}

Vec2 World2D::WorldToView(Vec2 world) const
{
    if (m_State == nullptr || m_State->Camera.Height <= 0.0f)
    {
        return Vec2{};
    }

    float aspect = 1.0f;
    if (m_State->RenderWidth > 0 && m_State->RenderHeight > 0)
    {
        aspect = static_cast<float>(m_State->RenderWidth) / static_cast<float>(m_State->RenderHeight);
    }
    else
    {
        LogMessage(LogLevel::Warning, "query",
                   "WorldToView was called before SetRenderSize, so a square view is assumed.");
    }

    const Camera2D& camera = m_State->Camera;
    return Vec2{ 0.5f + (world.X - camera.Center.X) / (camera.Height * aspect),
                 0.5f - (world.Y - camera.Center.Y) / camera.Height };
}

namespace
{

struct PointQuery
{
    b2Vec2 Point;
    b2ShapeId Shape;
    bool Found = false;
};

bool PointQueryCallback(b2ShapeId shapeId, void* context)
{
    auto* query = static_cast<PointQuery*>(context);

    // Sensors are skipped, as they are by ray casts, so clicking an object
    // inside a trigger volume picks the object.
    if (b2Shape_IsSensor(shapeId) || !b2Shape_TestPoint(shapeId, query->Point))
    {
        return true; // keep looking
    }

    query->Shape = shapeId;
    query->Found = true;
    return false; // stop at the first shape actually under the point
}

} // namespace

RayHit2D World2D::PickFromView(float normalizedX, float normalizedY)
{
    RayHit2D result;

    if (m_State == nullptr)
    {
        return result;
    }

    if (!detail::IsFinite(normalizedX) || !detail::IsFinite(normalizedY))
    {
        LogMessage(LogLevel::Error, "query", "PickFromView was given a non-finite view coordinate.");
        return result;
    }

    const Vec2 world = ViewToWorld(normalizedX, normalizedY);
    result.Point = world;

    // A point has no area, so the query box is given a hair of width. Anything
    // it catches is then tested exactly.
    PointQuery query;
    query.Point = detail::ToB2(world);

    constexpr float epsilon = 0.001f;
    const b2AABB box{ b2Vec2{ query.Point.x - epsilon, query.Point.y - epsilon },
                      b2Vec2{ query.Point.x + epsilon, query.Point.y + epsilon } };

    b2World_OverlapAABB(m_State->World, box, b2DefaultQueryFilter(), PointQueryCallback, &query);

    if (!query.Found)
    {
        return result;
    }

    const ActorId id = detail::ActorIdForShape(*m_State, query.Shape);
    if (!id.IsValid())
    {
        return result;
    }

    result.Hit = true;
    result.Actor = GetActor(id);
    return result;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void World2D::SetRenderSize(int width, int height)
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

void World2D::Render()
{
    if (m_State == nullptr || m_State->Renderer == nullptr)
    {
        return;
    }

    m_State->Renderer->SetSize(m_State->RenderWidth, m_State->RenderHeight, m_State->Render);
    m_State->Renderer->Render2D(*m_State);
}

void World2D::Run()
{
    if (m_State == nullptr)
    {
        return;
    }
    detail::RunWorld(detail::MakeHostHooks(this), "ludifex");
}

void* World2D::GetRenderTarget() const
{
    if (m_State == nullptr || m_State->Renderer == nullptr)
    {
        return nullptr;
    }
    return m_State->Renderer->GetResolvedTexture();
}

Camera2D& World2D::GetCamera()
{
    static Camera2D fallback;
    return m_State != nullptr ? m_State->Camera : fallback;
}

void World2D::SetCamera(const Camera2D& camera)
{
    if (m_State != nullptr)
    {
        m_State->Camera = camera;
    }
}

RenderSettings& World2D::GetRenderSettings()
{
    static RenderSettings fallback;
    return m_State != nullptr ? m_State->Render : fallback;
}

void World2D::SetRenderSettings(const RenderSettings& settings)
{
    if (m_State != nullptr)
    {
        m_State->Render = settings;
    }
}

void World2D::SetGraphicsQuality(GraphicsQuality quality)
{
    if (m_State != nullptr)
    {
        ApplyGraphicsQuality(m_State->Render, quality);
    }
}

void World2D::SetAntiAliasing(AntiAliasing mode)
{
    if (m_State != nullptr)
    {
        m_State->Render.Mode = mode;
    }
}

void World2D::SetBackground(TextureId texture)
{
    if (m_State != nullptr)
    {
        m_State->Extras.Background = texture;
    }
}

void World2D::SetBackground(const std::string& path)
{
    if (m_State != nullptr)
    {
        m_State->Extras.Background = path.empty() ? TextureId{} : LoadTexture(path);
    }
}

void World2D::AddPostProcess(MaterialId material, PassPoint point)
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

void World2D::RemovePostProcess(MaterialId material)
{
    if (m_State != nullptr)
    {
        detail::RemovePostProcessEntries(m_State->Extras, material);
    }
}

void World2D::ClearPostProcess()
{
    if (m_State != nullptr)
    {
        m_State->Extras.PostProcess.clear();
    }
}

void World2D::DrawLine(Vec2 from, Vec2 to, Color color)
{
    if (m_State != nullptr && detail::IsFinite(from) && detail::IsFinite(to))
    {
        // Slightly in front of the plane, so a line over a shape is not hidden
        // by the shape's own front face.
        detail::PushDebugLine(m_State->Extras, Vec3{ from.X, from.Y, 1.0f }, Vec3{ to.X, to.Y, 1.0f }, color);
    }
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

VoiceId World2D::PlaySound(SoundId sound, const SoundSettings& settings)
{
    detail::WorldAudio* audio = m_State != nullptr ? EnsureAudio(*m_State) : nullptr;
    return audio != nullptr ? audio->Play(sound, settings, detail::VoicePlacement::None, Vec3{}, ActorId{}) : VoiceId{};
}

VoiceId World2D::PlaySoundAt(SoundId sound, Vec2 position, const SoundSettings& settings)
{
    if (!detail::IsFinite(position))
    {
        LogMessage(LogLevel::Error, "audio", "PlaySoundAt was given a non-finite position; nothing played.");
        return VoiceId{};
    }
    detail::WorldAudio* audio = m_State != nullptr ? EnsureAudio(*m_State) : nullptr;
    return audio != nullptr ? audio->Play(sound, settings, detail::VoicePlacement::Point,
                                          Vec3{ position.X, position.Y, 0.0f }, ActorId{})
                            : VoiceId{};
}

VoiceId World2D::PlaySoundAt(SoundId sound, Actor2D emitter, const SoundSettings& settings)
{
    ActorRecord2* record = detail::Resolve(m_State, emitter.GetId(), "PlaySoundAt");
    if (record == nullptr)
    {
        return VoiceId{};
    }
    detail::WorldAudio* audio = EnsureAudio(*m_State);
    const Vec3 position{ record->Current.Position.X, record->Current.Position.Y, 0.0f };
    return audio != nullptr ? audio->Play(sound, settings, detail::VoicePlacement::Actor, position, emitter.GetId())
                            : VoiceId{};
}

void World2D::StopSound(VoiceId voice, float fadeSeconds)
{
    if (m_State != nullptr && m_State->Audio != nullptr)
    {
        m_State->Audio->Stop(voice, fadeSeconds);
    }
}

void World2D::StopAllSounds(float fadeSeconds)
{
    if (m_State != nullptr && m_State->Audio != nullptr)
    {
        m_State->Audio->StopAll(fadeSeconds);
    }
}

bool World2D::IsAudible(VoiceId voice) const
{
    return m_State != nullptr && m_State->Audio != nullptr && m_State->Audio->IsAudible(voice);
}

bool World2D::IsOccluded(VoiceId voice) const
{
    return m_State != nullptr && m_State->Audio != nullptr && m_State->Audio->IsOccluded(voice);
}

bool World2D::IsPlaying(VoiceId voice) const
{
    return m_State != nullptr && m_State->Audio != nullptr && m_State->Audio->IsPlaying(voice);
}

void World2D::SetVoiceVolume(VoiceId voice, float volume)
{
    if (m_State != nullptr && m_State->Audio != nullptr)
    {
        m_State->Audio->SetVolume(voice, volume);
    }
}

void World2D::SetVoicePosition(VoiceId voice, Vec2 position)
{
    if (m_State != nullptr && m_State->Audio != nullptr)
    {
        m_State->Audio->SetPosition(voice, Vec3{ position.X, position.Y, 0.0f });
    }
}

void World2D::PlayMusic(const std::string& path, float volume, float fadeSeconds)
{
    detail::WorldAudio* audio = m_State != nullptr ? EnsureAudio(*m_State) : nullptr;
    if (audio != nullptr)
    {
        audio->PlayMusic(path, volume, fadeSeconds);
    }
}

void World2D::StopMusic(float fadeSeconds)
{
    if (m_State != nullptr && m_State->Audio != nullptr)
    {
        m_State->Audio->StopMusic(fadeSeconds);
    }
}

void World2D::SetListener(Vec2 position)
{
    detail::WorldAudio* audio = m_State != nullptr ? EnsureAudio(*m_State) : nullptr;
    if (audio != nullptr && detail::IsFinite(position))
    {
        audio->SetListener(Vec3{ position.X, position.Y, 0.0f }, Vec3{ 0.0f, 0.0f, -1.0f }, Vec3{ 0.0f, 1.0f, 0.0f });
    }
}

AudioSettings& World2D::GetAudioSettings()
{
    static AudioSettings fallback;
    detail::WorldAudio* audio = m_State != nullptr ? EnsureAudio(*m_State) : nullptr;
    return audio != nullptr ? audio->Settings : fallback;
}

void World2D::SetAudioSettings(const AudioSettings& settings)
{
    detail::WorldAudio* audio = m_State != nullptr ? EnsureAudio(*m_State) : nullptr;
    if (audio != nullptr)
    {
        audio->Settings = settings;
    }
}

AudioStats World2D::GetAudioStats() const
{
    if (m_State == nullptr || m_State->Audio == nullptr)
    {
        return AudioStats{};
    }
    return m_State->Audio->GetStats();
}

uint32_t World2D::GetLastDrawCallCount() const
{
    if (m_State == nullptr || m_State->Renderer == nullptr)
    {
        return 0;
    }
    return m_State->Renderer->GetDrawCallCount();
}

// ---------------------------------------------------------------------------
// Sprites
// ---------------------------------------------------------------------------

Actor2D World2D::AddSprite(const SpriteDesc& desc)
{
    Actor2D actor;
    if (m_State == nullptr)
    {
        return actor;
    }

    if (!detail::IsFinite(desc.Position) || !detail::IsFinite(desc.Rotation) || !detail::IsFinite(desc.Size))
    {
        LogMessage(LogLevel::Error, "actor", "AddSprite was given a non-finite transform; no actor was created.");
        return actor;
    }

    // A path that cannot be loaded still gives a texture: the checkerboard.
    const TextureId texture = !desc.Path.empty() ? LoadTexture(desc.Path) : desc.Texture;
    const Vec2 image = GetTextureSize(texture);
    const bool hasImage = image.X > 0.0f && image.Y > 0.0f;

    // The size in world units: as given, or from the image, keeping its shape
    // when only one axis was given.
    const float pixelsPerUnit = desc.PixelsPerUnit > 0.0f ? desc.PixelsPerUnit : 100.0f;
    Vec2 size = desc.Size;
    if (size.X <= 0.0f && size.Y <= 0.0f)
    {
        size = hasImage ? Vec2{ image.X / pixelsPerUnit, image.Y / pixelsPerUnit } : Vec2{ 1.0f, 1.0f };
    }
    else if (size.X <= 0.0f)
    {
        size.X = hasImage ? size.Y * image.X / image.Y : size.Y;
    }
    else if (size.Y <= 0.0f)
    {
        size.Y = hasImage ? size.X * image.Y / image.X : size.X;
    }

    if (!detail::ValidateExtent(size.X, "Sprite width") || !detail::ValidateExtent(size.Y, "Sprite height"))
    {
        return actor;
    }

    const b2BodyId body = CreateBody(*m_State, desc.Position, desc.Rotation, desc.Type, false);
    if (desc.FixedRotation)
    {
        b2Body_SetFixedRotation(body, true);
    }

    b2ShapeDef shapeDef = MakeShapeDef(desc.Density, desc.Friction, desc.Restitution, desc.IsSensor);
    ShapeKind2 shape = ShapeKind2::None;

    switch (desc.Collider)
    {
        case SpriteCollider::Box:
        {
            const b2Polygon polygon = b2MakeBox(size.X * 0.5f, size.Y * 0.5f);
            b2CreatePolygonShape(body, &shapeDef, &polygon);
            shape = ShapeKind2::Rectangle;
            break;
        }
        case SpriteCollider::Circle:
        {
            b2Circle circle{};
            circle.radius = std::min(size.X, size.Y) * 0.5f;
            b2CreateCircleShape(body, &shapeDef, &circle);
            shape = ShapeKind2::Circle;
            break;
        }
        case SpriteCollider::None:
            break;
    }

    actor.m_State = m_State;
    actor.m_Id = RegisterActor(*m_State, body, size, desc.Name, shape, desc.IsSensor);

    if (ActorRecord2* record = detail::Resolve(m_State, actor.m_Id, "AddSprite"))
    {
        // Drawn as authored: the image's own colours, unlit, and shown even
        // when it is a sensor, because a sprite is there to be seen.
        record->IsSprite = true;
        record->Look.Tint = Color{ 1.0f, 1.0f, 1.0f, 1.0f };
        record->Look.Texture = texture;
        record->Look.Unlit = true;
        record->Look.Layer = desc.Layer;
        record->Look.Visible = true;
    }

    return actor;
}

Actor2D World2D::AddSprite(const std::string& path)
{
    SpriteDesc desc;
    desc.Path = path;
    return AddSprite(desc);
}

// ---------------------------------------------------------------------------
// Actor2D
// ---------------------------------------------------------------------------

Color Actor2D::GetColor() const
{
    const ActorRecord2* record = detail::Resolve(m_State, m_Id, "GetColor");
    return record != nullptr ? record->Look.Tint : Color{};
}

void Actor2D::SetColor(Color color)
{
    if (ActorRecord2* record = detail::Resolve(m_State, m_Id, "SetColor"))
    {
        record->Look.Tint = color;
    }
}

void Actor2D::SetMaterial(MaterialId material)
{
    if (ActorRecord2* record = detail::Resolve(m_State, m_Id, "SetMaterial"))
    {
        if (record->Look.Material != material)
        {
            record->Look.ParamMask = 0;
        }
        record->Look.Material = material;
    }
}

MaterialId Actor2D::GetMaterial() const
{
    const ActorRecord2* record = detail::Resolve(m_State, m_Id, "GetMaterial");
    return record != nullptr ? record->Look.Material : MaterialId{};
}

void Actor2D::SetUniform(const std::string& name, float x, float y, float z, float w)
{
    if (ActorRecord2* record = detail::Resolve(m_State, m_Id, "SetUniform"))
    {
        detail::SetActorUniform(record->Look, name, Color{ x, y, z, w }, false);
    }
}

void Actor2D::SetUniform(const std::string& name, Color color)
{
    if (ActorRecord2* record = detail::Resolve(m_State, m_Id, "SetUniform"))
    {
        detail::SetActorUniform(record->Look, name, color, true);
    }
}

void Actor2D::ClearUniforms()
{
    if (ActorRecord2* record = detail::Resolve(m_State, m_Id, "ClearUniforms"))
    {
        record->Look.ParamMask = 0;
    }
}

void Actor2D::SetTexture(TextureId texture)
{
    if (ActorRecord2* record = detail::Resolve(m_State, m_Id, "SetTexture"))
    {
        record->Look.Texture = texture;
    }
}

TextureId Actor2D::GetTexture() const
{
    const ActorRecord2* record = detail::Resolve(m_State, m_Id, "GetTexture");
    return record != nullptr ? record->Look.Texture : TextureId{};
}

void Actor2D::SetTextureTiling(Vec2 repeat, Vec2 offset)
{
    if (ActorRecord2* record = detail::Resolve(m_State, m_Id, "SetTextureTiling"))
    {
        if (detail::IsFinite(repeat) && detail::IsFinite(offset))
        {
            record->Look.UVScale = repeat;
            record->Look.UVOffset = offset;
        }
    }
}

void Actor2D::SetFrame(int frame, int columns, int rows)
{
    ActorRecord2* record = detail::Resolve(m_State, m_Id, "SetFrame");
    if (record == nullptr)
    {
        return;
    }
    if (columns <= 0 || rows <= 0)
    {
        LogMessage(LogLevel::Warning, "actor", "SetFrame needs at least one column and one row.");
        return;
    }

    // Frames beyond the sheet wrap around, so a counter can drive an
    // animation without being reset.
    const int count = columns * rows;
    const int index = ((frame % count) + count) % count;

    record->Look.UVScale = Vec2{ 1.0f / static_cast<float>(columns), 1.0f / static_cast<float>(rows) };
    record->Look.UVOffset = Vec2{ static_cast<float>(index % columns) / static_cast<float>(columns),
                                  static_cast<float>(index / columns) / static_cast<float>(rows) };
}

void Actor2D::SetLayer(int layer)
{
    if (ActorRecord2* record = detail::Resolve(m_State, m_Id, "SetLayer"))
    {
        record->Look.Layer = layer;
    }
}

int Actor2D::GetLayer() const
{
    const ActorRecord2* record = detail::Resolve(m_State, m_Id, "GetLayer");
    return record != nullptr ? record->Look.Layer : 0;
}

void Actor2D::SetFlipX(bool flipped)
{
    if (ActorRecord2* record = detail::Resolve(m_State, m_Id, "SetFlipX"))
    {
        record->Look.FlipX = flipped;
    }
}

bool Actor2D::GetFlipX() const
{
    const ActorRecord2* record = detail::Resolve(m_State, m_Id, "GetFlipX");
    return record != nullptr && record->Look.FlipX;
}

void Actor2D::SetFixedRotation(bool fixed)
{
    if (ActorRecord2* record = detail::Resolve(m_State, m_Id, "SetFixedRotation"))
    {
        b2Body_SetFixedRotation(record->Body, fixed);
    }
}

bool Actor2D::IsVisible() const
{
    const ActorRecord2* record = detail::Resolve(m_State, m_Id, "IsVisible");
    return record != nullptr && record->Look.Visible;
}

void Actor2D::SetVisible(bool visible)
{
    if (ActorRecord2* record = detail::Resolve(m_State, m_Id, "SetVisible"))
    {
        record->Look.Visible = visible;
    }
}

bool Actor2D::IsValid() const
{
    if (m_State == nullptr || !m_Id.IsValid() || m_Id.Index >= m_State->Actors.size())
    {
        return false;
    }

    const ActorRecord2& record = m_State->Actors[m_Id.Index];
    return record.Alive && record.Generation == m_Id.Generation;
}

const std::string& Actor2D::GetName() const
{
    const ActorRecord2* record = detail::Resolve(m_State, m_Id, "GetName");
    return record != nullptr ? record->Name : g_EmptyName;
}

void Actor2D::SetName(const std::string& name)
{
    if (ActorRecord2* record = detail::Resolve(m_State, m_Id, "SetName"))
    {
        record->Name = name;
    }
}

Vec2 Actor2D::GetPosition() const
{
    const ActorRecord2* record = detail::Resolve(m_State, m_Id, "GetPosition");
    return record != nullptr ? detail::FromB2(b2Body_GetPosition(record->Body)) : Vec2{};
}

void Actor2D::SetPosition(Vec2 position)
{
    ActorRecord2* record = detail::Resolve(m_State, m_Id, "SetPosition");
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

    b2Body_SetTransform(record->Body, detail::ToB2(position), b2Body_GetRotation(record->Body));

    record->Current = ReadTransform(record->Body, record->Scale);
    record->Previous = record->Current;
}

float Actor2D::GetRotation() const
{
    const ActorRecord2* record = detail::Resolve(m_State, m_Id, "GetRotation");
    return record != nullptr ? b2Rot_GetAngle(b2Body_GetRotation(record->Body)) : 0.0f;
}

void Actor2D::SetRotation(float radians)
{
    ActorRecord2* record = detail::Resolve(m_State, m_Id, "SetRotation");
    if (record == nullptr)
    {
        return;
    }

    if (!detail::IsFinite(radians))
    {
        LogMessage(LogLevel::Error, "actor",
                   "SetRotation on \"%s\" was given a non-finite value and was rejected.",
                   record->Name.empty() ? "<unnamed>" : record->Name.c_str());
        return;
    }

    b2Body_SetTransform(record->Body, b2Body_GetPosition(record->Body), b2MakeRot(radians));

    record->Current = ReadTransform(record->Body, record->Scale);
    record->Previous = record->Current;
}

Vec2 Actor2D::GetLinearVelocity() const
{
    const ActorRecord2* record = detail::Resolve(m_State, m_Id, "GetLinearVelocity");
    return record != nullptr ? detail::FromB2(b2Body_GetLinearVelocity(record->Body)) : Vec2{};
}

void Actor2D::SetLinearVelocity(Vec2 velocity)
{
    ActorRecord2* record = detail::Resolve(m_State, m_Id, "SetLinearVelocity");
    if (record == nullptr || !detail::IsFinite(velocity))
    {
        return;
    }
    b2Body_SetLinearVelocity(record->Body, detail::ToB2(velocity));
}

float Actor2D::GetAngularVelocity() const
{
    const ActorRecord2* record = detail::Resolve(m_State, m_Id, "GetAngularVelocity");
    return record != nullptr ? b2Body_GetAngularVelocity(record->Body) : 0.0f;
}

void Actor2D::SetAngularVelocity(float radiansPerSecond)
{
    ActorRecord2* record = detail::Resolve(m_State, m_Id, "SetAngularVelocity");
    if (record == nullptr || !detail::IsFinite(radiansPerSecond))
    {
        return;
    }
    b2Body_SetAngularVelocity(record->Body, radiansPerSecond);
}

void Actor2D::ApplyForce(Vec2 force)
{
    ActorRecord2* record = detail::Resolve(m_State, m_Id, "ApplyForce");
    if (record == nullptr || !detail::IsFinite(force))
    {
        return;
    }
    b2Body_ApplyForceToCenter(record->Body, detail::ToB2(force), true);
}

void Actor2D::ApplyImpulse(Vec2 impulse)
{
    ActorRecord2* record = detail::Resolve(m_State, m_Id, "ApplyImpulse");
    if (record == nullptr || !detail::IsFinite(impulse))
    {
        return;
    }
    b2Body_ApplyLinearImpulseToCenter(record->Body, detail::ToB2(impulse), true);
}

float Actor2D::GetMass() const
{
    const ActorRecord2* record = detail::Resolve(m_State, m_Id, "GetMass");
    return record != nullptr ? b2Body_GetMass(record->Body) : 0.0f;
}

bool Actor2D::IsAwake() const
{
    const ActorRecord2* record = detail::Resolve(m_State, m_Id, "IsAwake");
    return record != nullptr && b2Body_IsAwake(record->Body);
}

void Actor2D::SetAwake(bool awake)
{
    if (ActorRecord2* record = detail::Resolve(m_State, m_Id, "SetAwake"))
    {
        b2Body_SetAwake(record->Body, awake);
    }
}

Transform2 Actor2D::GetInterpolatedTransform() const
{
    const ActorRecord2* record = detail::Resolve(m_State, m_Id, "GetInterpolatedTransform");
    if (record == nullptr)
    {
        return Transform2{};
    }
    return detail::LerpTransform(record->Previous, record->Current, m_State->Alpha);
}

void Actor2D::WhenCollided(CollisionHandler2D handler)
{
    if (ActorRecord2* record = detail::Resolve(m_State, m_Id, "WhenCollided"))
    {
        record->OnCollided = std::move(handler);
    }
}

void Actor2D::WhenEntered(TriggerHandler2D handler)
{
    if (ActorRecord2* record = detail::Resolve(m_State, m_Id, "WhenEntered"))
    {
        record->OnEntered = std::move(handler);
    }
}

void Actor2D::WhenExited(TriggerHandler2D handler)
{
    if (ActorRecord2* record = detail::Resolve(m_State, m_Id, "WhenExited"))
    {
        record->OnExited = std::move(handler);
    }
}

bool Actor2D::IsSensor() const
{
    const ActorRecord2* record = detail::Resolve(m_State, m_Id, "IsSensor");
    return record != nullptr && record->IsSensor;
}

void Actor2D::SetCollisionFilter(uint64_t category, uint64_t mask)
{
    ActorRecord2* record = detail::Resolve(m_State, m_Id, "SetCollisionFilter");
    if (record == nullptr)
    {
        return;
    }

    const int shapeCount = b2Body_GetShapeCount(record->Body);
    if (shapeCount <= 0)
    {
        return;
    }

    std::vector<b2ShapeId> shapes(static_cast<size_t>(shapeCount));
    b2Body_GetShapes(record->Body, shapes.data(), shapeCount);

    for (b2ShapeId shape : shapes)
    {
        b2Filter filter = b2Shape_GetFilter(shape);
        filter.categoryBits = category;
        filter.maskBits = mask;
        b2Shape_SetFilter(shape, filter);
    }
}

void Actor2D::SetBodyType(BodyType type)
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

void Actor2D::Destroy()
{
    if (detail::Resolve(m_State, m_Id, "Destroy") == nullptr)
    {
        return;
    }

    detail::Command command;
    command.Type = detail::CommandType::Destroy;
    command.Id = m_Id;
    m_State->Commands.push_back(command);
}

} // namespace ludifex
