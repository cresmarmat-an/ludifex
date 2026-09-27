// Shape queries and the character mover, for World2D.
//
// The same two halves as Character3D.cpp, in the plane. A shape cast asks
// whether something this wide can get there; an overlap asks what is here.
// Both are built on Box2D's shape proxy, a few points with a radius, which is
// how circles, rectangles, and capsules share one code path.
//
// The character is a capsule that is moved rather than simulated: pushed out
// of what it overlaps, slid along what it meets, lifted over what is low
// enough, and asked what it stands on. Box2D's own mover cast has no way to
// leave a body out, and the character's capsule is a body in the world, so the
// casts here go through the general shape cast with a callback that skips it.

#include "Internal.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <vector>

namespace ludifex
{
namespace
{

using detail::ActorRecord2;
using detail::World2DState;

constexpr float DegreesToRadians = 3.14159265358979323846f / 180.0f;

// Enough to round a corner: move, hit, slide, move again.
constexpr int SlideIterations = 4;

// The mover is kept this far off what it touches, so the next move does not
// start already inside it.
constexpr float SkinWidth = 0.01f;

// How far down the ground is looked for.
constexpr float GroundProbe = 0.05f;

// Beyond this many touching surfaces a character is in a crack, and the
// nearest few decide the answer.
constexpr int MaxPlanes = 8;

float Length(const Vec2& value)
{
    return std::sqrt(value.X * value.X + value.Y * value.Y);
}

// --- proxies -----------------------------------------------------------------

b2ShapeProxy CircleProxy(const Vec2& center, float radius)
{
    const b2Vec2 point = detail::ToB2(center);
    return b2MakeProxy(&point, 1, std::max(radius, 1e-3f));
}

b2ShapeProxy RectangleProxy(const Vec2& center, const Vec2& size)
{
    const float x = std::max(size.X, 1e-3f) * 0.5f;
    const float y = std::max(size.Y, 1e-3f) * 0.5f;
    const b2Vec2 points[4] = {
        { center.X - x, center.Y - y },
        { center.X + x, center.Y - y },
        { center.X + x, center.Y + y },
        { center.X - x, center.Y + y },
    };
    return b2MakeProxy(points, 4, 0.0f);
}

b2ShapeProxy CapsuleProxy(const Vec2& center, float radius, float height)
{
    const float safeRadius = std::max(radius, 1e-3f);
    const float half = std::max(0.0f, height * 0.5f - safeRadius);
    const b2Vec2 points[2] = {
        { center.X, center.Y - half },
        { center.X, center.Y + half },
    };
    return b2MakeProxy(points, half > 0.0f ? 2 : 1, safeRadius);
}

// --- callbacks ---------------------------------------------------------------

// The closest solid hit along a cast, leaving out sensors and, for a
// character, its own body. A shape the cast starts inside is left out too:
// what is already overlapped is the push-out's business, not the cast's, and
// counting it would pin the mover where it stands.
struct ClosestCast
{
    b2BodyId Ignore{};
    bool HasIgnore = false;

    b2ShapeId Shape{};
    b2Vec2 Point{};
    b2Vec2 Normal{};
    float Fraction = 1.0f;
    bool Found = false;
};

float ClosestCastCallback(b2ShapeId shapeId, b2Vec2 point, b2Vec2 normal, float fraction, void* context)
{
    auto* cast = static_cast<ClosestCast*>(context);

    if (b2Shape_IsSensor(shapeId) || fraction <= 0.0f)
    {
        return -1.0f;
    }

    if (cast->HasIgnore)
    {
        const b2BodyId body = b2Shape_GetBody(shapeId);
        if (B2_ID_EQUALS(body, cast->Ignore))
        {
            return -1.0f;
        }
    }

    cast->Shape = shapeId;
    cast->Point = point;
    cast->Normal = normal;
    cast->Fraction = fraction;
    cast->Found = true;
    return fraction;
}

struct OverlapCollector
{
    World2DState* State = nullptr;
    bool IncludeSensors = false;
    std::vector<ActorId> Found;
};

bool OverlapCallback(b2ShapeId shapeId, void* context)
{
    auto* collector = static_cast<OverlapCollector*>(context);

    if (!collector->IncludeSensors && b2Shape_IsSensor(shapeId))
    {
        return true;
    }

    const ActorId id = detail::ActorIdForShape(*collector->State, shapeId);
    if (id.IsValid() && std::find(collector->Found.begin(), collector->Found.end(), id) == collector->Found.end())
    {
        collector->Found.push_back(id);
    }
    return true;
}

struct PointCollector
{
    OverlapCollector Overlaps;
    b2Vec2 Point{};
};

bool PointCallback(b2ShapeId shapeId, void* context)
{
    auto* collector = static_cast<PointCollector*>(context);
    if (!b2Shape_TestPoint(shapeId, collector->Point))
    {
        return true;
    }
    return OverlapCallback(shapeId, &collector->Overlaps);
}

struct PlaneCollector
{
    b2BodyId Self{};
    b2CollisionPlane Planes[MaxPlanes]{};
    int PlaneCount = 0;
};

bool PlaneCallback(b2ShapeId shapeId, const b2PlaneResult* plane, void* context)
{
    auto* collector = static_cast<PlaneCollector*>(context);

    if (b2Shape_IsSensor(shapeId) || !plane->hit)
    {
        return true;
    }

    const b2BodyId body = b2Shape_GetBody(shapeId);
    if (B2_ID_EQUALS(body, collector->Self))
    {
        return true;
    }

    b2CollisionPlane& collision = collector->Planes[collector->PlaneCount++];
    collision.plane = plane->plane;
    collision.pushLimit = FLT_MAX;
    collision.push = 0.0f;
    collision.clipVelocity = true;

    return collector->PlaneCount < MaxPlanes;
}

// --- the mover -----------------------------------------------------------------

b2Capsule MoverCapsule(const detail::CharacterState2& character, const Vec2& position, float inflate = 0.0f)
{
    const float half = std::max(0.0f, character.Height * 0.5f - character.Radius);

    b2Capsule capsule;
    capsule.center1 = b2Vec2{ position.X, position.Y - half };
    capsule.center2 = b2Vec2{ position.X, position.Y + half };
    capsule.radius = character.Radius + inflate;
    return capsule;
}

// The surfaces around the character where it stands. The capsule is fattened
// a little, because a character that has just stopped against a wall rests a
// hair off it: with the true radius it would find no wall and push into it
// again instead of sliding along it.
void CollectPlanes(World2DState& state, const ActorRecord2& record, const Vec2& position, PlaneCollector& planes)
{
    planes.PlaneCount = 0;
    planes.Self = record.Body;

    const b2Capsule capsule = MoverCapsule(record.Character, position, SkinWidth * 3.0f);
    b2World_CollideMover(state.World, &capsule, b2DefaultQueryFilter(), PlaneCallback, &planes);
}

// How far along a translation the character's capsule gets before touching
// anything but itself: 1 when nothing is in the way.
float CastCharacter(World2DState& state, const ActorRecord2& record, const Vec2& position, const Vec2& translation)
{
    const detail::CharacterState2& character = record.Character;
    const b2ShapeProxy proxy = CapsuleProxy(position, character.Radius, character.Height);

    ClosestCast cast;
    cast.Ignore = record.Body;
    cast.HasIgnore = true;
    b2World_CastShape(state.World, &proxy, detail::ToB2(translation), b2DefaultQueryFilter(), ClosestCastCallback,
                      &cast);
    return cast.Found ? cast.Fraction : 1.0f;
}

void SyncCharacterBody(ActorRecord2& record, const Vec2& position)
{
    b2Body_SetTransform(record.Body, detail::ToB2(position), b2Rot_identity);

    // Moved on the caller's frame, not the physics step, so there is nothing
    // to interpolate between.
    record.Previous.Position = position;
    record.Current.Position = position;
}

RayHit2D CastProxy(World2DState* state, const b2ShapeProxy& proxy, const Vec2& direction, float maxDistance,
                   const char* name, ActorId& outActor)
{
    RayHit2D result;
    if (state == nullptr)
    {
        return result;
    }

    if (!detail::IsFinite(direction) || !detail::IsFinite(maxDistance) || maxDistance <= 0.0f)
    {
        LogMessage(LogLevel::Error, "world", "%s was given non-finite arguments; it found nothing.", name);
        return result;
    }

    const float length = Length(direction);
    if (length <= 1e-6f)
    {
        LogMessage(LogLevel::Error, "world", "%s was given a zero-length direction.", name);
        return result;
    }

    const Vec2 translation{ direction.X / length * maxDistance, direction.Y / length * maxDistance };

    ClosestCast cast;
    b2World_CastShape(state->World, &proxy, detail::ToB2(translation), b2DefaultQueryFilter(), ClosestCastCallback,
                      &cast);
    if (!cast.Found)
    {
        return result;
    }

    outActor = detail::ActorIdForShape(*state, cast.Shape);
    if (!outActor.IsValid())
    {
        return result;
    }

    result.Hit = true;
    result.Point = detail::FromB2(cast.Point);
    result.Normal = detail::FromB2(cast.Normal);
    result.Fraction = cast.Fraction;
    result.Distance = cast.Fraction * maxDistance;
    return result;
}

std::vector<Actor2D> OverlapProxy(World2DState* state, const b2ShapeProxy& proxy, bool includeSensors)
{
    std::vector<Actor2D> actors;
    if (state == nullptr)
    {
        return actors;
    }

    OverlapCollector collector;
    collector.State = state;
    collector.IncludeSensors = includeSensors;
    b2World_OverlapShape(state->World, &proxy, b2DefaultQueryFilter(), OverlapCallback, &collector);

    actors.reserve(collector.Found.size());
    for (const ActorId& id : collector.Found)
    {
        actors.push_back(detail::MakeActor(state, id));
    }
    return actors;
}

bool ValidQuery(const Vec2& point, const char* name)
{
    if (!detail::IsFinite(point))
    {
        LogMessage(LogLevel::Error, "world", "%s was given a non-finite position; it found nothing.", name);
        return false;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Shape queries
// ---------------------------------------------------------------------------

RayHit2D World2D::CastCircle(Vec2 from, float radius, Vec2 direction, float maxDistance)
{
    ActorId id;
    if (!ValidQuery(from, "CastCircle"))
    {
        return {};
    }
    RayHit2D hit = CastProxy(m_State, CircleProxy(from, radius), direction, maxDistance, "CastCircle", id);
    if (hit.Hit)
    {
        hit.Actor = GetActor(id);
    }
    return hit;
}

RayHit2D World2D::CastRectangle(Vec2 from, Vec2 size, Vec2 direction, float maxDistance)
{
    ActorId id;
    if (!ValidQuery(from, "CastRectangle"))
    {
        return {};
    }
    RayHit2D hit = CastProxy(m_State, RectangleProxy(from, size), direction, maxDistance, "CastRectangle", id);
    if (hit.Hit)
    {
        hit.Actor = GetActor(id);
    }
    return hit;
}

RayHit2D World2D::CastCapsule(Vec2 from, float radius, float height, Vec2 direction, float maxDistance)
{
    ActorId id;
    if (!ValidQuery(from, "CastCapsule"))
    {
        return {};
    }
    RayHit2D hit =
        CastProxy(m_State, CapsuleProxy(from, radius, height), direction, maxDistance, "CastCapsule", id);
    if (hit.Hit)
    {
        hit.Actor = GetActor(id);
    }
    return hit;
}

std::vector<Actor2D> World2D::OverlapCircle(Vec2 center, float radius, bool includeSensors)
{
    if (!ValidQuery(center, "OverlapCircle"))
    {
        return {};
    }
    return OverlapProxy(m_State, CircleProxy(center, radius), includeSensors);
}

std::vector<Actor2D> World2D::OverlapRectangle(Vec2 center, Vec2 size, bool includeSensors)
{
    if (!ValidQuery(center, "OverlapRectangle"))
    {
        return {};
    }
    return OverlapProxy(m_State, RectangleProxy(center, size), includeSensors);
}

std::vector<Actor2D> World2D::OverlapCapsule(Vec2 center, float radius, float height, bool includeSensors)
{
    if (!ValidQuery(center, "OverlapCapsule"))
    {
        return {};
    }
    return OverlapProxy(m_State, CapsuleProxy(center, radius, height), includeSensors);
}

std::vector<Actor2D> World2D::OverlapPoint(Vec2 point, bool includeSensors)
{
    std::vector<Actor2D> actors;
    if (m_State == nullptr || !ValidQuery(point, "OverlapPoint"))
    {
        return actors;
    }

    PointCollector collector;
    collector.Overlaps.State = m_State;
    collector.Overlaps.IncludeSensors = includeSensors;
    collector.Point = detail::ToB2(point);

    const b2AABB box{ { point.X - 1e-3f, point.Y - 1e-3f }, { point.X + 1e-3f, point.Y + 1e-3f } };
    b2World_OverlapAABB(m_State->World, box, b2DefaultQueryFilter(), PointCallback, &collector);

    for (const ActorId& id : collector.Overlaps.Found)
    {
        actors.push_back(GetActor(id));
    }
    return actors;
}

// ---------------------------------------------------------------------------
// Character
// ---------------------------------------------------------------------------

Character2D World2D::AddCharacter(const Character2DDesc& desc)
{
    Character2D character;
    if (m_State == nullptr)
    {
        return character;
    }

    const float radius = std::max(0.05f, detail::IsFinite(desc.Radius) ? desc.Radius : 0.3f);
    const float height = std::max(radius * 2.0f + 0.01f, detail::IsFinite(desc.Height) ? desc.Height : 1.2f);

    Capsule2DDesc capsule;
    capsule.Radius = radius;
    capsule.Height = height;
    capsule.Position = desc.Position;
    capsule.Type = BodyType::Kinematic;
    capsule.Name = desc.Name.empty() ? std::string("Character") : desc.Name;

    const Actor2D actor = AddCapsule(capsule);
    ActorRecord2* record = detail::Resolve(m_State, actor.GetId(), "AddCharacter");
    if (record == nullptr)
    {
        return character;
    }

    record->IsCharacter = true;
    record->Character.Radius = radius;
    record->Character.Height = height;
    record->Character.StepHeight = std::max(0.0f, detail::IsFinite(desc.StepHeight) ? desc.StepHeight : 0.0f);
    record->Character.SlopeLimit =
        std::cos(std::clamp(detail::IsFinite(desc.SlopeLimitDegrees) ? desc.SlopeLimitDegrees : 55.0f, 0.0f, 89.0f) *
                 DegreesToRadians);
    record->Character.Position = desc.Position;
    record->Character.OnGround = false;
    record->Character.GroundNormal = Vec2{ 0.0f, 1.0f };

    // A character is not spun by what it walks into.
    b2Body_SetFixedRotation(record->Body, true);

    character.m_Id = actor.GetId();
    character.m_State = m_State;
    return character;
}

bool Character2D::IsValid() const
{
    if (m_State == nullptr || !m_Id.IsValid() || m_Id.Index >= m_State->Actors.size())
    {
        return false;
    }

    const ActorRecord2& record = m_State->Actors[m_Id.Index];
    return record.Alive && record.Generation == m_Id.Generation && record.IsCharacter;
}

Actor2D Character2D::GetActor() const
{
    return detail::MakeActor(m_State, m_Id);
}

Vec2 Character2D::GetPosition() const
{
    const ActorRecord2* record = detail::Resolve(m_State, m_Id, "GetPosition");
    return record != nullptr ? record->Character.Position : Vec2{};
}

void Character2D::SetPosition(Vec2 position)
{
    ActorRecord2* record = detail::Resolve(m_State, m_Id, "SetPosition");
    if (record == nullptr || !detail::IsFinite(position))
    {
        return;
    }

    record->Character.Position = position;
    SyncCharacterBody(*record, position);
}

bool Character2D::IsOnGround() const
{
    return IsValid() && m_State->Actors[m_Id.Index].Character.OnGround;
}

Vec2 Character2D::GetGroundNormal() const
{
    return IsValid() ? m_State->Actors[m_Id.Index].Character.GroundNormal : Vec2{ 0.0f, 1.0f };
}

void Character2D::Destroy()
{
    Actor2D actor = GetActor();
    actor.Destroy();
    m_Id = ActorId{};
}

Vec2 Character2D::Move(Vec2 delta)
{
    ActorRecord2* record = detail::Resolve(m_State, m_Id, "Move");
    if (record == nullptr)
    {
        return Vec2{};
    }

    if (!detail::IsFinite(delta))
    {
        LogMessage(LogLevel::Error, "world", "Character2D::Move was given a non-finite delta.");
        return Vec2{};
    }

    World2DState& state = *m_State;
    const Vec2 start = record->Character.Position;
    Vec2 position = start;
    Vec2 remaining = delta;

    PlaneCollector planes;

    for (int iteration = 0; iteration < SlideIterations; ++iteration)
    {
        // Out of anything it is already inside first, then along what is left.
        CollectPlanes(state, *record, position, planes);

        if (planes.PlaneCount > 0)
        {
            // Only genuine overlap is pushed out of; the fattened probe finds
            // surfaces, it does not shove the character off them.
            const b2PlaneSolverResult push = b2SolvePlanes(b2Vec2{ 0.0f, 0.0f }, planes.Planes, planes.PlaneCount);
            const float pushed =
                std::sqrt(push.translation.x * push.translation.x + push.translation.y * push.translation.y);
            if (pushed > SkinWidth * 3.0f)
            {
                const float keep = (pushed - SkinWidth * 3.0f) / pushed;
                position.X += push.translation.x * keep;
                position.Y += push.translation.y * keep;
            }

            // Whatever is in the way takes its share out of the movement, so
            // running into a wall becomes sliding along it.
            remaining = detail::FromB2(b2ClipVector(detail::ToB2(remaining), planes.Planes, planes.PlaneCount));
        }

        const float length = Length(remaining);
        if (length <= 1e-5f)
        {
            break;
        }

        const float fraction = CastCharacter(state, *record, position, remaining);

        // Stopping a hair short keeps the next move out of the surface it
        // just touched.
        const float travelled = std::max(0.0f, fraction - SkinWidth / std::max(length, 1e-5f));
        position.X += remaining.X * travelled;
        position.Y += remaining.Y * travelled;

        if (fraction >= 1.0f)
        {
            break;
        }

        remaining.X *= (1.0f - travelled);
        remaining.Y *= (1.0f - travelled);
    }

    // A ledge no taller than StepHeight is stepped up rather than walked into:
    // lift, try the rest of the sideways movement, and settle back down.
    const float wantedSide = delta.X;
    const float movedSide = position.X - start.X;

    if (record->Character.StepHeight > 0.0f && std::abs(wantedSide) > 1e-4f &&
        std::abs(movedSide) < std::abs(wantedSide) * 0.9f)
    {
        const float lift = record->Character.StepHeight;
        const Vec2 up{ 0.0f, lift };
        const float rise = CastCharacter(state, *record, position, up);

        Vec2 stepped{ position.X, position.Y + lift * std::max(0.0f, rise - SkinWidth / lift) };
        const Vec2 leftover{ wantedSide - movedSide, 0.0f };

        const float across = CastCharacter(state, *record, stepped, leftover);
        if (across > 0.05f)
        {
            stepped.X += leftover.X * std::max(0.0f, across - SkinWidth / std::abs(leftover.X));

            // Back down onto whatever is under the step.
            const Vec2 down{ 0.0f, -(lift + SkinWidth * 2.0f) };
            const float drop = CastCharacter(state, *record, stepped, down);
            stepped.Y += down.Y * std::max(0.0f, drop - SkinWidth / std::abs(down.Y));

            // Only worth it if it gained ground and landed on something.
            if (drop < 1.0f && std::abs(stepped.X - start.X) > std::abs(movedSide) + 1e-4f)
            {
                position = stepped;
            }
        }
    }

    // What is underfoot, and whether it is flat enough to stand on.
    record->Character.OnGround = false;
    record->Character.GroundNormal = Vec2{ 0.0f, 1.0f };

    CollectPlanes(state, *record, Vec2{ position.X, position.Y - GroundProbe }, planes);
    float bestUp = -1.0f;
    for (int index = 0; index < planes.PlaneCount; ++index)
    {
        const b2Vec2 normal = planes.Planes[index].plane.normal;
        if (normal.y >= record->Character.SlopeLimit && normal.y > bestUp)
        {
            bestUp = normal.y;
            record->Character.OnGround = true;
            record->Character.GroundNormal = detail::FromB2(normal);
        }
    }

    record->Character.Position = position;
    SyncCharacterBody(*record, position);

    return Vec2{ position.X - start.X, position.Y - start.Y };
}

} // namespace ludifex
