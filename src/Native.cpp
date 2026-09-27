#include "Internal.h"

#include <ludifex/native.h>

namespace ludifex::native
{

// Befriended by World3D, Actor3D, World2D, and Actor2D. It exists so the
// public header can promise native access without including box2d or box3d.
struct Accessor
{
    static b3WorldId World3(const World3D& world)
    {
        return world.m_State != nullptr ? world.m_State->World : b3WorldId{};
    }

    static b3BodyId Body3(const Actor3D& actor)
    {
        const detail::ActorRecord3* record =
            detail::Resolve(actor.m_State, actor.m_Id, "GetNativeBody");
        return record != nullptr ? record->Body : b3BodyId{};
    }

    static b2WorldId World2(const World2D& world)
    {
        return world.m_State != nullptr ? world.m_State->World : b2WorldId{};
    }

    static b2BodyId Body2(const Actor2D& actor)
    {
        const detail::ActorRecord2* record =
            detail::Resolve(actor.m_State, actor.m_Id, "GetNativeBody");
        return record != nullptr ? record->Body : b2BodyId{};
    }
};

b3WorldId GetNativeWorld(const World3D& world)
{
    return Accessor::World3(world);
}

b3BodyId GetNativeBody(const Actor3D& actor)
{
    return Accessor::Body3(actor);
}

b2WorldId GetNativeWorld(const World2D& world)
{
    return Accessor::World2(world);
}

b2BodyId GetNativeBody(const Actor2D& actor)
{
    return Accessor::Body2(actor);
}

} // namespace ludifex::native
