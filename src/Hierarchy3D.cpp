// The transform hierarchy.
//
// A child keeps its place relative to its parent, and the parent's movement
// carries it, whatever moved the parent.
//
// The work happens at the sync points, after physics has run, so a child never
// fights the solver within a frame. Dynamic bodies are left alone: physics
// owns their transforms, and overriding them would make a body teleport and
// then explode. Two dynamic bodies that must stay together should use a weld
// joint. The hierarchy is meant for the static and kinematic parts of a
// scene.

#include "Internal.h"

#include <algorithm>
#include <cmath>

namespace ludifex
{
namespace
{

using detail::ActorRecord3;
using detail::World3DState;

Transform3 WorldTransformOf(const ActorRecord3& record)
{
    Transform3 transform;
    transform.Position = detail::FromB3Pos(b3Body_GetPosition(record.Body));
    transform.Rotation = detail::FromB3(b3Body_GetRotation(record.Body));
    return transform;
}

// The child's place expressed in the parent's frame: undo the parent's turn,
// then its position.
Transform3 LocalFromWorld(const Transform3& parent, const Transform3& child)
{
    const Quat inverse = parent.Rotation.Inverse();
    const Vec3 offset{ child.Position.X - parent.Position.X, child.Position.Y - parent.Position.Y,
                       child.Position.Z - parent.Position.Z };

    Transform3 local;
    local.Position = inverse.Rotate(offset);
    local.Rotation = child.Rotation.Then(inverse);
    return local;
}

// The other way round: where the child sits in the world, given its parent.
Transform3 WorldFromLocal(const Transform3& parent, const Transform3& local)
{
    const Vec3 turned = parent.Rotation.Rotate(local.Position);

    Transform3 world;
    world.Position = Vec3{ parent.Position.X + turned.X, parent.Position.Y + turned.Y,
                           parent.Position.Z + turned.Z };
    world.Rotation = local.Rotation.Then(parent.Rotation);
    return world;
}

void CarryChildren(World3DState& state, ActorRecord3& parent, const Transform3& parentWorld)
{
    for (const ActorId& id : parent.Children)
    {
        if (!id.IsValid() || id.Index >= state.Actors.size())
        {
            continue;
        }

        ActorRecord3& child = state.Actors[id.Index];
        if (!child.Alive || child.Generation != id.Generation)
        {
            continue;
        }

        const b3BodyType type = b3Body_GetType(child.Body);
        if (type == b3_dynamicBody)
        {
            // Physics owns this one. Said once, then left alone.
            if (!child.WarnedAboutDynamicParent)
            {
                child.WarnedAboutDynamicParent = true;
                LogMessage(LogLevel::Warning, "actor",
                           "\"%s\" is dynamic and parented to \"%s\", so physics keeps moving it "
                           "and the parent does not carry it. Make it kinematic or static, or "
                           "weld the two together with a joint.",
                           child.Name.empty() ? "<unnamed>" : child.Name.c_str(),
                           parent.Name.empty() ? "<unnamed>" : parent.Name.c_str());
            }

            // Its own children still follow it.
            CarryChildren(state, child, WorldTransformOf(child));
            continue;
        }

        const Transform3 world = WorldFromLocal(parentWorld, child.Local);
        b3Body_SetTransform(child.Body, detail::ToB3Pos(world.Position),
                            detail::ToB3(world.Rotation));

        // A carried child is where it was put, not somewhere it is heading:
        // interpolating from where it used to be would smear it across the
        // frame the parent teleported.
        child.Current.Position = world.Position;
        child.Current.Rotation = world.Rotation;
        child.Previous = child.Current;

        if (child.IsCharacter)
        {
            child.Character.Position = world.Position;
        }

        CarryChildren(state, child, world);
    }
}

// Whether an actor is somewhere below another one; used to refuse cycles.
bool IsDescendant(World3DState& state, const ActorId& candidate, const ActorId& root)
{
    if (!candidate.IsValid() || !root.IsValid())
    {
        return false;
    }
    if (candidate == root)
    {
        return true;
    }

    if (root.Index >= state.Actors.size())
    {
        return false;
    }

    const ActorRecord3& record = state.Actors[root.Index];
    if (!record.Alive || record.Generation != root.Generation)
    {
        return false;
    }

    for (const ActorId& child : record.Children)
    {
        if (IsDescendant(state, candidate, child))
        {
            return true;
        }
    }
    return false;
}

} // namespace

namespace detail
{

void PropagateHierarchy(World3DState& state)
{
    for (size_t index = 0; index < state.Actors.size(); ++index)
    {
        ActorRecord3& record = state.Actors[index];
        if (!record.Alive || record.Children.empty() || record.Parent.IsValid())
        {
            // Only roots start a walk; everything below is reached from there.
            continue;
        }

        CarryChildren(state, record, WorldTransformOf(record));
    }
}

// Attaches or detaches, at the sync point. Keeping the child where it is means
// parenting is never a move: the offset is measured now and kept from here on.
void ApplyParent(World3DState& state, const ActorId& childId, const ActorId& parentId)
{
    ActorRecord3* child = Resolve(&state, childId, "SetParent");
    if (child == nullptr)
    {
        return;
    }

    // Out of its old parent's list first, whatever happens next.
    if (child->Parent.IsValid() && child->Parent.Index < state.Actors.size())
    {
        ActorRecord3& previous = state.Actors[child->Parent.Index];
        if (previous.Alive && previous.Generation == child->Parent.Generation)
        {
            previous.Children.erase(
                std::remove(previous.Children.begin(), previous.Children.end(), childId),
                previous.Children.end());
        }
    }
    child->Parent = ActorId{};

    if (!parentId.IsValid())
    {
        return;
    }

    ActorRecord3* parent = Resolve(&state, parentId, "SetParent");
    if (parent == nullptr)
    {
        return;
    }

    if (IsDescendant(state, parentId, childId))
    {
        LogMessage(LogLevel::Error, "actor",
                   "\"%s\" cannot be parented to \"%s\", because that would make a loop. The "
                   "actor was left where it was.",
                   child->Name.empty() ? "<unnamed>" : child->Name.c_str(),
                   parent->Name.empty() ? "<unnamed>" : parent->Name.c_str());
        return;
    }

    child->Parent = parentId;
    child->Local = LocalFromWorld(WorldTransformOf(*parent), WorldTransformOf(*child));
    parent->Children.push_back(childId);
}

// Queues the whole subtree for destruction, deepest last, so a parent's
// children go with it.
void QueueSubtreeDestroy(World3DState& state, const ActorId& id, std::vector<Command>& commands)
{
    if (!id.IsValid() || id.Index >= state.Actors.size())
    {
        return;
    }

    const ActorRecord3& record = state.Actors[id.Index];
    if (!record.Alive || record.Generation != id.Generation)
    {
        return;
    }

    // Copied, because applying the commands will edit the list this walks.
    const std::vector<ActorId> children = record.Children;
    for (const ActorId& child : children)
    {
        QueueSubtreeDestroy(state, child, commands);
    }

    Command command;
    command.Type = CommandType::Destroy;
    command.Id = id;
    commands.push_back(command);
}

// Takes an actor out of the hierarchy as it dies, so no stale id is left in a
// parent's list or a child's back-pointer.
void DetachFromHierarchy(World3DState& state, const ActorId& id, ActorRecord3& record)
{
    if (record.Parent.IsValid() && record.Parent.Index < state.Actors.size())
    {
        ActorRecord3& parent = state.Actors[record.Parent.Index];
        if (parent.Alive && parent.Generation == record.Parent.Generation)
        {
            parent.Children.erase(std::remove(parent.Children.begin(), parent.Children.end(), id),
                                  parent.Children.end());
        }
    }
    record.Parent = ActorId{};

    for (const ActorId& childId : record.Children)
    {
        if (childId.Index >= state.Actors.size())
        {
            continue;
        }
        ActorRecord3& child = state.Actors[childId.Index];
        if (child.Alive && child.Generation == childId.Generation)
        {
            child.Parent = ActorId{};
        }
    }
    record.Children.clear();
}

} // namespace detail

// ---------------------------------------------------------------------------
// Actor3D
// ---------------------------------------------------------------------------

void Actor3D::SetParent(Actor3D parent)
{
    ActorRecord3* record = detail::Resolve(m_State, m_Id, "SetParent");
    if (record == nullptr)
    {
        return;
    }

    detail::Command command;
    command.Type = detail::CommandType::SetParent;
    command.Id = m_Id;
    command.Parent = parent.GetId();
    m_State->Commands.push_back(command);
}

void Actor3D::ClearParent()
{
    SetParent(Actor3D{});
}

Actor3D Actor3D::GetParent() const
{
    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "GetParent");
    if (record == nullptr || !record->Parent.IsValid())
    {
        return Actor3D{};
    }
    return detail::MakeActor(m_State, record->Parent);
}

std::vector<Actor3D> Actor3D::GetChildren() const
{
    std::vector<Actor3D> children;

    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "GetChildren");
    if (record == nullptr)
    {
        return children;
    }

    children.reserve(record->Children.size());
    for (const ActorId& id : record->Children)
    {
        Actor3D child = detail::MakeActor(m_State, id);
        if (child.IsValid())
        {
            children.push_back(child);
        }
    }
    return children;
}

Vec3 Actor3D::GetLocalPosition() const
{
    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "GetLocalPosition");
    if (record == nullptr)
    {
        return Vec3{};
    }
    return record->Parent.IsValid() ? record->Local.Position : GetPosition();
}

void Actor3D::SetLocalPosition(Vec3 position)
{
    ActorRecord3* record = detail::Resolve(m_State, m_Id, "SetLocalPosition");
    if (record == nullptr || !detail::IsFinite(position))
    {
        return;
    }

    if (!record->Parent.IsValid())
    {
        SetPosition(position);
        return;
    }

    record->Local.Position = position;
    detail::PropagateHierarchy(*m_State);
}

Quat Actor3D::GetLocalRotation() const
{
    const ActorRecord3* record = detail::Resolve(m_State, m_Id, "GetLocalRotation");
    if (record == nullptr)
    {
        return Quat{};
    }
    return record->Parent.IsValid() ? record->Local.Rotation : GetRotation();
}

void Actor3D::SetLocalRotation(Quat rotation)
{
    ActorRecord3* record = detail::Resolve(m_State, m_Id, "SetLocalRotation");
    if (record == nullptr)
    {
        return;
    }

    if (!record->Parent.IsValid())
    {
        SetRotation(rotation);
        return;
    }

    record->Local.Rotation = rotation;
    detail::PropagateHierarchy(*m_State);
}

} // namespace ludifex
