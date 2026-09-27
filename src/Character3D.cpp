// Shape queries and the character mover.
//
// A shape cast finds whether something of a given size can move somewhere, and
// an overlap finds what is already in a place. Both use the same proxy (a
// small point cloud with a radius), so one code path serves spheres, boxes,
// and capsules.
//
// The character is not a dynamic body. It is a capsule that is pushed out of
// what it overlaps, slides along what it meets, steps over what is low enough,
// and reports what it is standing on. It carries a kinematic actor with it so
// the rest of the world can collide with it.

#include "Internal.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <vector>

namespace ludifex
{
namespace
{

using detail::ActorRecord3;
using detail::World3DState;

constexpr float DegreesToRadians = 3.14159265358979323846f / 180.0f;

// Enough to round a corner: move, hit, slide, move again.
constexpr int SlideIterations = 4;

// The mover is kept this far off what it touches, so the next frame's overlap
// test does not start already inside something.
constexpr float SkinWidth = 0.015f;

// How far down the ground is looked for.
constexpr float GroundProbe = 0.08f;

// The most planes worth solving against at once. Beyond this a character is in
// a crack, and the nearest few decide the answer.
constexpr int MaxPlanes = 8;

b3Vec3 ToB3(const Vec3& value)
{
    return b3Vec3{ value.X, value.Y, value.Z };
}

Vec3 FromB3(const b3Vec3& value)
{
    return Vec3{ value.x, value.y, value.z };
}

float Length(const Vec3& value)
{
    return std::sqrt(value.X * value.X + value.Y * value.Y + value.Z * value.Z);
}

// --- proxies ---------------------------------------------------------------

// A sphere is one point with a radius; a capsule is two; a box is its eight
// corners with no radius. The same cast and overlap code then serves all three.
struct Proxy
{
    b3Vec3 Points[8]{};
    b3ShapeProxy Shape{};
};

Proxy SphereProxy(float radius)
{
    Proxy proxy;
    proxy.Points[0] = b3Vec3{ 0.0f, 0.0f, 0.0f };
    proxy.Shape.points = proxy.Points;
    proxy.Shape.count = 1;
    proxy.Shape.radius = std::max(radius, 1e-3f);
    return proxy;
}

Proxy CapsuleProxy(float radius, float height)
{
    const float safeRadius = std::max(radius, 1e-3f);
    const float half = std::max(0.0f, height * 0.5f - safeRadius);

    Proxy proxy;
    proxy.Points[0] = b3Vec3{ 0.0f, -half, 0.0f };
    proxy.Points[1] = b3Vec3{ 0.0f, half, 0.0f };
    proxy.Shape.points = proxy.Points;
    proxy.Shape.count = 2;
    proxy.Shape.radius = safeRadius;
    return proxy;
}

Proxy BoxProxy(const Vec3& scale)
{
    const float x = std::max(scale.X, 1e-3f) * 0.5f;
    const float y = std::max(scale.Y, 1e-3f) * 0.5f;
    const float z = std::max(scale.Z, 1e-3f) * 0.5f;

    Proxy proxy;
    int index = 0;
    for (int corner = 0; corner < 8; ++corner)
    {
        proxy.Points[index++] = b3Vec3{ (corner & 1) ? x : -x, (corner & 2) ? y : -y,
                                        (corner & 4) ? z : -z };
    }
    proxy.Shape.points = proxy.Points;
    proxy.Shape.count = 8;
    proxy.Shape.radius = 0.0f;
    return proxy;
}

// --- callbacks -------------------------------------------------------------

struct ClosestShapeHit
{
    b3ShapeId Shape{};
    b3Pos Point{};
    b3Vec3 Normal{};
    float Fraction = 1.0f;
    bool Found = false;
};

float ClosestShapeCallback(b3ShapeId shapeId, b3Pos point, b3Vec3 normal, float fraction,
                           uint64_t userMaterialId, int triangleIndex, int childIndex, void* context)
{
    (void)userMaterialId;
    (void)triangleIndex;
    (void)childIndex;

    if (b3Shape_IsSensor(shapeId))
    {
        return -1.0f; // a trigger volume does not stop anything
    }

    auto* hit = static_cast<ClosestShapeHit*>(context);
    hit->Shape = shapeId;
    hit->Point = point;
    hit->Normal = normal;
    hit->Fraction = fraction;
    hit->Found = true;

    return fraction;
}

struct OverlapCollector
{
    World3DState* State = nullptr;
    bool IncludeSensors = false;
    std::vector<ActorId> Found;
};

bool OverlapCallback(b3ShapeId shapeId, void* context)
{
    auto* collector = static_cast<OverlapCollector*>(context);

    if (!collector->IncludeSensors && b3Shape_IsSensor(shapeId))
    {
        return true;
    }

    const ActorId id = detail::ActorIdForShape(*collector->State, shapeId);
    if (id.IsValid() &&
        std::find(collector->Found.begin(), collector->Found.end(), id) == collector->Found.end())
    {
        // One actor can own several shapes; it is reported once.
        collector->Found.push_back(id);
    }
    return true;
}

// The character's own capsule is in the world like anything else, so every
// query it makes has to leave itself out.
struct MoverContext
{
    World3DState* State = nullptr;
    b3BodyId Self{};

    b3CollisionPlane Planes[MaxPlanes]{};
    int PlaneCount = 0;
};

bool IgnoreSelf(b3ShapeId shapeId, const MoverContext& context)
{
    if (b3Shape_IsSensor(shapeId))
    {
        return true;
    }
    const b3BodyId body = b3Shape_GetBody(shapeId);
    return B3_ID_EQUALS(body, context.Self);
}

bool MoverFilterCallback(b3ShapeId shapeId, void* context)
{
    return !IgnoreSelf(shapeId, *static_cast<MoverContext*>(context));
}

bool PlaneCallback(b3ShapeId shapeId, const b3PlaneResult* plane, int planeCount, void* context)
{
    auto* mover = static_cast<MoverContext*>(context);
    if (IgnoreSelf(shapeId, *mover))
    {
        return true;
    }

    for (int index = 0; index < planeCount && mover->PlaneCount < MaxPlanes; ++index)
    {
        b3CollisionPlane& collision = mover->Planes[mover->PlaneCount++];
        collision.plane = plane[index].plane;
        collision.pushLimit = FLT_MAX;
        collision.push = 0.0f;
        collision.clipVelocity = true;
    }

    return mover->PlaneCount < MaxPlanes;
}

// --- the mover -------------------------------------------------------------

b3Capsule MoverCapsule(const ActorRecord3& record, float inflate = 0.0f)
{
    const float radius = record.Character.Radius;
    const float half = std::max(0.0f, record.Character.Height * 0.5f - radius);

    b3Capsule capsule;
    capsule.center1 = b3Vec3{ 0.0f, -half, 0.0f };
    capsule.center2 = b3Vec3{ 0.0f, half, 0.0f };
    capsule.radius = radius + inflate;
    return capsule;
}

// Gathers the planes around the character where it stands. The capsule is
// slightly fattened for this, because a character that has just stopped
// against a wall is resting a hair off it: with the true radius it would touch
// nothing, find no wall, and push straight into it again instead of sliding
// along it.
void CollectPlanes(World3DState& state, const ActorRecord3& record, const Vec3& position,
                   MoverContext& mover)
{
    mover.PlaneCount = 0;

    const b3Capsule capsule = MoverCapsule(record, SkinWidth * 3.0f);
    b3World_CollideMover(state.World, detail::ToB3Pos(position), &capsule, b3DefaultQueryFilter(),
                         PlaneCallback, &mover);
}

} // namespace

namespace detail
{

// Steps the character's own actor to where the mover ended up, so what is drawn
// and what other bodies collide with follow the character rather than lag a
// frame behind it.
void SyncCharacterBody(ActorRecord3& record, const Vec3& position)
{
    b3Body_SetTransform(record.Body, ToB3Pos(position), b3Quat_identity);

    record.Previous.Position = position;
    record.Current.Position = position;
}

} // namespace detail

// ---------------------------------------------------------------------------
// Shape queries
// ---------------------------------------------------------------------------

namespace
{

RayHit CastProxy(World3DState* state, const Proxy& proxy, const Vec3& from, const Vec3& direction,
                 float maxDistance, const char* name, ActorId& outActor)
{
    RayHit result;

    if (state == nullptr)
    {
        return result;
    }

    if (!detail::IsFinite(from) || !detail::IsFinite(direction) || !detail::IsFinite(maxDistance) ||
        maxDistance <= 0.0f)
    {
        LogMessage(LogLevel::Error, "world", "%s was given non-finite arguments; it found nothing.",
                   name);
        return result;
    }

    const float length = Length(direction);
    if (length <= 1e-6f)
    {
        LogMessage(LogLevel::Error, "world", "%s was given a zero-length direction.", name);
        return result;
    }

    const Vec3 translation{ direction.X / length * maxDistance, direction.Y / length * maxDistance,
                            direction.Z / length * maxDistance };

    ClosestShapeHit hit;
    b3World_CastShape(state->World, detail::ToB3Pos(from), &proxy.Shape, ToB3(translation),
                      b3DefaultQueryFilter(), ClosestShapeCallback, &hit);

    if (!hit.Found)
    {
        return result;
    }

    outActor = detail::ActorIdForShape(*state, hit.Shape);
    if (!outActor.IsValid())
    {
        return result;
    }

    result.Hit = true;
    result.Point = detail::FromB3Pos(hit.Point);
    result.Normal = FromB3(hit.Normal);
    result.Fraction = hit.Fraction;
    result.Distance = hit.Fraction * maxDistance;
    return result;
}

std::vector<ActorId> OverlapProxy(World3DState* state, const Proxy& proxy, const Vec3& center,
                                  bool includeSensors)
{
    OverlapCollector collector;
    collector.State = state;
    collector.IncludeSensors = includeSensors;

    if (state == nullptr || !detail::IsFinite(center))
    {
        return collector.Found;
    }

    b3World_OverlapShape(state->World, detail::ToB3Pos(center), &proxy.Shape, b3DefaultQueryFilter(),
                         OverlapCallback, &collector);
    return collector.Found;
}

} // namespace

RayHit World3D::CastSphere(Vec3 from, float radius, Vec3 direction, float maxDistance)
{
    ActorId id;
    RayHit hit =
        CastProxy(m_State, SphereProxy(radius), from, direction, maxDistance, "CastSphere", id);
    if (hit.Hit)
    {
        hit.Actor = GetActor(id);
    }
    return hit;
}

RayHit World3D::CastBox(Vec3 from, Vec3 scale, Vec3 direction, float maxDistance)
{
    ActorId id;
    RayHit hit = CastProxy(m_State, BoxProxy(scale), from, direction, maxDistance, "CastBox", id);
    if (hit.Hit)
    {
        hit.Actor = GetActor(id);
    }
    return hit;
}

RayHit World3D::CastCapsule(Vec3 from, float radius, float height, Vec3 direction, float maxDistance)
{
    ActorId id;
    RayHit hit = CastProxy(m_State, CapsuleProxy(radius, height), from, direction, maxDistance,
                           "CastCapsule", id);
    if (hit.Hit)
    {
        hit.Actor = GetActor(id);
    }
    return hit;
}

std::vector<Actor3D> World3D::OverlapSphere(Vec3 center, float radius, bool includeSensors)
{
    std::vector<Actor3D> actors;
    for (const ActorId& id : OverlapProxy(m_State, SphereProxy(radius), center, includeSensors))
    {
        actors.push_back(GetActor(id));
    }
    return actors;
}

std::vector<Actor3D> World3D::OverlapBox(Vec3 center, Vec3 scale, bool includeSensors)
{
    std::vector<Actor3D> actors;
    for (const ActorId& id : OverlapProxy(m_State, BoxProxy(scale), center, includeSensors))
    {
        actors.push_back(GetActor(id));
    }
    return actors;
}

std::vector<Actor3D> World3D::OverlapCapsule(Vec3 center, float radius, float height,
                                             bool includeSensors)
{
    std::vector<Actor3D> actors;
    for (const ActorId& id :
         OverlapProxy(m_State, CapsuleProxy(radius, height), center, includeSensors))
    {
        actors.push_back(GetActor(id));
    }
    return actors;
}

// ---------------------------------------------------------------------------
// Character
// ---------------------------------------------------------------------------

Character3D World3D::AddCharacter(const CharacterDesc& desc)
{
    Character3D character;

    if (m_State == nullptr)
    {
        return character;
    }

    const float radius = std::max(0.05f, desc.Radius);
    const float height = std::max(radius * 2.0f + 0.01f, desc.Height);

    CapsuleDesc capsule;
    capsule.Radius = radius;
    capsule.Height = height;
    capsule.Position = desc.Position;
    capsule.Type = BodyType::Kinematic;
    capsule.Name = desc.Name.empty() ? std::string("Character") : desc.Name;

    Actor3D actor = AddCapsule(capsule);
    if (!actor.IsValid())
    {
        return character;
    }

    ActorRecord3* record = detail::Resolve(m_State, actor.GetId(), "AddCharacter");
    if (record == nullptr)
    {
        return character;
    }

    record->IsCharacter = true;
    record->Character.Radius = radius;
    record->Character.Height = height;
    record->Character.StepHeight = std::max(0.0f, desc.StepHeight);
    record->Character.SlopeLimit = std::cos(std::clamp(desc.SlopeLimitDegrees, 0.0f, 89.0f) *
                                            DegreesToRadians);
    record->Character.Position = desc.Position;
    record->Character.OnGround = false;
    record->Character.GroundNormal = Vec3{ 0.0f, 1.0f, 0.0f };

    character.m_Id = actor.GetId();
    character.m_State = m_State;
    return character;
}

bool Character3D::IsValid() const
{
    if (m_State == nullptr || !m_Id.IsValid() || m_Id.Index >= m_State->Actors.size())
    {
        return false;
    }

    const ActorRecord3& record = m_State->Actors[m_Id.Index];
    return record.Alive && record.Generation == m_Id.Generation;
}

Actor3D Character3D::GetActor() const
{
    return detail::MakeActor(m_State, m_Id);
}

Vec3 Character3D::GetPosition() const
{
    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "GetPosition");
    return record != nullptr ? record->Character.Position : Vec3{};
}

void Character3D::SetPosition(Vec3 position)
{
    ActorRecord3* record = detail::Resolve(m_State, m_Id, "SetPosition");
    if (record == nullptr || !detail::IsFinite(position))
    {
        return;
    }

    record->Character.Position = position;
    detail::SyncCharacterBody(*record, position);
}

bool Character3D::IsOnGround() const
{
    return IsValid() && m_State->Actors[m_Id.Index].Character.OnGround;
}

Vec3 Character3D::GetGroundNormal() const
{
    return IsValid() ? m_State->Actors[m_Id.Index].Character.GroundNormal
                     : Vec3{ 0.0f, 1.0f, 0.0f };
}

void Character3D::Destroy()
{
    Actor3D actor = GetActor();
    actor.Destroy();
    m_Id = ActorId{};
}

Vec3 Character3D::Move(Vec3 delta)
{
    ActorRecord3* record = detail::Resolve(m_State, m_Id, "Move");
    if (record == nullptr)
    {
        return Vec3{};
    }

    if (!detail::IsFinite(delta))
    {
        LogMessage(LogLevel::Error, "world", "Character3D::Move was given a non-finite delta.");
        return Vec3{};
    }

    const Vec3 start = record->Character.Position;

    MoverContext mover;
    mover.State = m_State;
    mover.Self = record->Body;

    const b3Capsule capsule = MoverCapsule(*record);
    Vec3 position = start;
    Vec3 remaining = delta;

    for (int iteration = 0; iteration < SlideIterations; ++iteration)
    {
        // Out of anything it is already inside first, then along what is left.
        CollectPlanes(*m_State, *record, position, mover);

        if (mover.PlaneCount > 0)
        {
            // Only genuine overlap is pushed out of; the fattened probe is for
            // finding surfaces, not for shoving the character off them.
            const b3PlaneSolverResult push =
                b3SolvePlanes(b3Vec3_zero, mover.Planes, mover.PlaneCount);
            const float pushed = std::sqrt(push.delta.x * push.delta.x + push.delta.y * push.delta.y +
                                           push.delta.z * push.delta.z);
            if (pushed > SkinWidth * 3.0f)
            {
                const float keep = (pushed - SkinWidth * 3.0f) / pushed;
                position.X += push.delta.x * keep;
                position.Y += push.delta.y * keep;
                position.Z += push.delta.z * keep;
            }

            // Whatever is in the way takes its share out of the movement, which
            // is what turns walking into a wall into sliding along it.
            const b3Vec3 clipped = b3ClipVector(ToB3(remaining), mover.Planes, mover.PlaneCount);
            remaining = FromB3(clipped);
        }

        const float length = Length(remaining);
        if (length <= 1e-5f)
        {
            break;
        }

        const float fraction = b3World_CastMover(m_State->World, detail::ToB3Pos(position), &capsule,
                                                 ToB3(remaining), b3DefaultQueryFilter(),
                                                 MoverFilterCallback, &mover);

        // Stopping a hair short keeps the next frame's overlap test out of the
        // surface it just touched.
        const float travelled = std::max(0.0f, fraction - SkinWidth / std::max(length, 1e-5f));
        position.X += remaining.X * travelled;
        position.Y += remaining.Y * travelled;
        position.Z += remaining.Z * travelled;

        if (fraction >= 1.0f)
        {
            break;
        }

        remaining.X *= (1.0f - travelled);
        remaining.Y *= (1.0f - travelled);
        remaining.Z *= (1.0f - travelled);
    }

    // A ledge no taller than StepHeight is stepped over rather than walked
    // into: lift, try the rest of the movement again, and settle back down.
    const Vec3 moved{ position.X - start.X, position.Y - start.Y, position.Z - start.Z };
    const Vec3 wantedFlat{ delta.X, 0.0f, delta.Z };
    const Vec3 movedFlat{ moved.X, 0.0f, moved.Z };

    if (record->Character.StepHeight > 0.0f && Length(wantedFlat) > 1e-4f &&
        Length(movedFlat) < Length(wantedFlat) * 0.9f)
    {
        const float lift = record->Character.StepHeight;

        Vec3 stepped{ position.X, position.Y + lift, position.Z };
        Vec3 leftover{ wantedFlat.X - movedFlat.X, 0.0f, wantedFlat.Z - movedFlat.Z };

        const float length = Length(leftover);
        if (length > 1e-5f)
        {
            const float fraction =
                b3World_CastMover(m_State->World, detail::ToB3Pos(stepped), &capsule, ToB3(leftover),
                                  b3DefaultQueryFilter(), MoverFilterCallback, &mover);
            if (fraction > 0.05f)
            {
                stepped.X += leftover.X * fraction;
                stepped.Z += leftover.Z * fraction;

                // Back down onto whatever is under the step.
                const Vec3 down{ 0.0f, -(lift + SkinWidth), 0.0f };
                const float drop =
                    b3World_CastMover(m_State->World, detail::ToB3Pos(stepped), &capsule, ToB3(down),
                                      b3DefaultQueryFilter(), MoverFilterCallback, &mover);
                stepped.Y += down.Y * drop;

                // Only worth it if the step actually gained ground.
                const Vec3 steppedFlat{ stepped.X - start.X, 0.0f, stepped.Z - start.Z };
                if (Length(steppedFlat) > Length(movedFlat) + 1e-4f)
                {
                    position = stepped;
                }
            }
        }
    }

    // What is underfoot, and whether it is flat enough to stand on.
    record->Character.OnGround = false;
    record->Character.GroundNormal = Vec3{ 0.0f, 1.0f, 0.0f };

    CollectPlanes(*m_State, *record, Vec3{ position.X, position.Y - GroundProbe, position.Z }, mover);
    for (int index = 0; index < mover.PlaneCount; ++index)
    {
        const b3Vec3 normal = mover.Planes[index].plane.normal;
        if (normal.y >= record->Character.SlopeLimit)
        {
            record->Character.OnGround = true;
            record->Character.GroundNormal = FromB3(normal);
            break;
        }
    }

    record->Character.Position = position;
    detail::SyncCharacterBody(*record, position);

    return Vec3{ position.X - start.X, position.Y - start.Y, position.Z - start.Z };
}

} // namespace ludifex
