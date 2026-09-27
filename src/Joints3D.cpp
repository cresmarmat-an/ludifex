// Joints for World3D.
//
// Box3D describes a joint with a local frame on each body: an attachment point
// and an orientation whose axes the solver measures against. A hinge turns
// about its frame's z-axis and a slider travels along its frame's x-axis.
//
// Describing that directly is awkward, so this file takes a world-space pivot
// and axis and does the conversion. The frames are built from the bodies'
// current transforms, which is why a joint can be created from placed bodies
// without them jumping when it starts.

#include "Render3D.h"

#include <algorithm>
#include <cmath>

namespace ludifex
{
namespace
{

using detail::ActorRecord3;
using detail::JointRecord3;
using detail::World3DState;

// Local frame on one body: the anchor in that body's space, and the joint's
// orientation expressed relative to the body's own rotation.
b3Transform MakeLocalFrame(b3BodyId body, const Vec3& worldAnchor, const Quat& worldRotation)
{
    b3Transform frame;
    frame.p = b3Body_GetLocalPoint(body, detail::ToB3Pos(worldAnchor));

    const Quat bodyRotation = detail::FromB3(b3Body_GetRotation(body));
    const Quat local = detail::QuatMultiply(detail::QuatConjugate(bodyRotation), worldRotation);

    frame.q = detail::ToB3(local);
    return frame;
}

// Validates the two actors and fills in the shared part of a joint definition.
bool PrepareJoint(World3DState& state, const Actor3D& actorA, const Actor3D& actorB,
                  bool collideConnected, b3JointDef& outDef, b3BodyId& outBodyA, b3BodyId& outBodyB)
{
    const ActorRecord3* recordA = detail::Resolve(&state, actorA.GetId(), "Creating a joint");
    const ActorRecord3* recordB = detail::Resolve(&state, actorB.GetId(), "Creating a joint");

    if (recordA == nullptr || recordB == nullptr)
    {
        LogMessage(LogLevel::Error, "joint", "A joint needs two live actors; none was created.");
        return false;
    }

    if (recordA == recordB)
    {
        LogMessage(LogLevel::Error, "joint", "A joint cannot connect an actor to itself.");
        return false;
    }

    outBodyA = recordA->Body;
    outBodyB = recordB->Body;

    outDef.bodyIdA = outBodyA;
    outDef.bodyIdB = outBodyB;
    outDef.collideConnected = collideConnected;

    return true;
}

// Returns the handle's id rather than the handle: only World3D may fill in a
// Joint3D's fields, so each Add function does that itself.
JointId RegisterJoint(World3DState& state, b3JointId joint, JointKind kind)
{
    if (!b3Joint_IsValid(joint))
    {
        LogMessage(LogLevel::Error, "joint", "The solver refused to create the joint.");
        return JointId{};
    }

    uint32_t index;
    if (!state.FreeJoints.empty())
    {
        index = state.FreeJoints.back();
        state.FreeJoints.pop_back();
    }
    else
    {
        index = static_cast<uint32_t>(state.Joints.size());
        state.Joints.emplace_back();
    }

    JointRecord3& record = state.Joints[index];
    ++record.Generation;
    if (record.Generation == 0)
    {
        record.Generation = 1;
    }

    record.Alive = true;
    record.Joint = joint;
    record.Kind = kind;

    ++state.LiveJoints;

    return JointId{ index, record.Generation };
}

bool ValidAxis(const Vec3& axis, const char* what)
{
    if (!detail::IsFinite(axis))
    {
        LogMessage(LogLevel::Error, "joint", "%s is not finite.", what);
        return false;
    }

    if (axis.X * axis.X + axis.Y * axis.Y + axis.Z * axis.Z < 1e-12f)
    {
        LogMessage(LogLevel::Error, "joint", "%s has no length, so it names no direction.", what);
        return false;
    }

    return true;
}

} // namespace

Joint3D World3D::AddHinge(const HingeDesc& desc)
{
    Joint3D joint;
    if (m_State == nullptr || !detail::IsFinite(desc.Anchor) || !ValidAxis(desc.Axis, "Hinge axis"))
    {
        return joint;
    }

    b3RevoluteJointDef def = b3DefaultRevoluteJointDef();
    b3BodyId bodyA{};
    b3BodyId bodyB{};

    if (!PrepareJoint(*m_State, desc.BodyA, desc.BodyB, desc.CollideConnected, def.base, bodyA, bodyB))
    {
        return joint;
    }

    // A hinge turns about its frame's z-axis, so the frame is oriented to put
    // z along the axis the caller asked for. Both bodies get the same world
    // orientation, so the starting angle is zero.
    const Quat frame = detail::QuatFromTo(Vec3{ 0.0f, 0.0f, 1.0f }, desc.Axis);

    def.base.localFrameA = MakeLocalFrame(bodyA, desc.Anchor, frame);
    def.base.localFrameB = MakeLocalFrame(bodyB, desc.Anchor, frame);

    def.enableLimit = desc.EnableLimit;
    def.lowerAngle = desc.LowerAngle;
    def.upperAngle = desc.UpperAngle;
    def.enableMotor = desc.EnableMotor;
    def.motorSpeed = desc.MotorSpeed;
    def.maxMotorTorque = desc.MaxMotorTorque;
    def.enableSpring = desc.EnableSpring;
    def.hertz = std::max(0.0f, desc.Hertz);
    def.dampingRatio = std::max(0.0f, desc.DampingRatio);

    joint.m_State = m_State;
    joint.m_Id = RegisterJoint(*m_State, b3CreateRevoluteJoint(m_State->World, &def), JointKind::Hinge);
    return joint;
}

Joint3D World3D::AddConeJoint(const ConeJointDesc& desc)
{
    Joint3D joint;
    if (m_State == nullptr || !detail::IsFinite(desc.Anchor) || !ValidAxis(desc.Axis, "Cone axis"))
    {
        return joint;
    }

    b3SphericalJointDef def = b3DefaultSphericalJointDef();
    b3BodyId bodyA{};
    b3BodyId bodyB{};

    if (!PrepareJoint(*m_State, desc.BodyA, desc.BodyB, desc.CollideConnected, def.base, bodyA, bodyB))
    {
        return joint;
    }

    // The cone is centred on the frame's z-axis and the twist is measured
    // about it, so the frame puts z along the axis asked for. Both bodies get
    // the same world orientation, which makes the pose at creation the one
    // with no swing and no twist.
    const Quat frame = detail::QuatFromTo(Vec3{ 0.0f, 0.0f, 1.0f }, desc.Axis);
    def.base.localFrameA = MakeLocalFrame(bodyA, desc.Anchor, frame);
    def.base.localFrameB = MakeLocalFrame(bodyB, desc.Anchor, frame);

    def.enableConeLimit = desc.EnableConeLimit;
    def.coneAngle = std::clamp(desc.ConeAngle, 0.0f, 0.5f * 3.14159265f);
    def.enableTwistLimit = desc.EnableTwistLimit;
    def.lowerTwistAngle = std::clamp(std::min(desc.LowerTwist, desc.UpperTwist), -0.99f * 3.14159265f, 0.0f);
    def.upperTwistAngle = std::clamp(std::max(desc.LowerTwist, desc.UpperTwist), 0.0f, 0.99f * 3.14159265f);
    def.enableSpring = desc.EnableSpring;
    def.hertz = std::max(0.0f, desc.Hertz);
    def.dampingRatio = std::max(0.0f, desc.DampingRatio);
    def.enableMotor = desc.EnableMotor;
    def.motorVelocity = detail::IsFinite(desc.MotorVelocity) ? detail::ToB3(desc.MotorVelocity) : b3Vec3{};
    def.maxMotorTorque = std::max(0.0f, desc.MaxMotorTorque);

    joint.m_State = m_State;
    joint.m_Id = RegisterJoint(*m_State, b3CreateSphericalJoint(m_State->World, &def), JointKind::Cone);
    return joint;
}

Ragdoll World3D::AddRagdoll(const RagdollDesc& desc)
{
    Ragdoll ragdoll;
    if (m_State == nullptr || !detail::IsFinite(desc.Position) || !(desc.Height > 0.2f) ||
        !detail::IsFinite(desc.Height))
    {
        LogMessage(LogLevel::Error, "joint", "AddRagdoll needs a finite position and a height above 0.2 m.");
        return ragdoll;
    }

    // Proportions of a standing adult, scaled to the height asked for. Every
    // limb hangs straight down, so every capsule is upright as made.
    const float s = desc.Height / 1.8f;
    const Vec3 at = desc.Position;
    auto P = [&](float x, float y, float z) { return Vec3{ at.X + x * s, at.Y + y * s, at.Z + z * s }; };

    auto Tinted = [&](Actor3D actor) {
        actor.SetColor(desc.Tint);
        return actor;
    };

    auto Capsule = [&](float radius, float height, Vec3 position, const char* part) {
        return Tinted(AddCapsule({ .Radius = radius * s, .Height = height * s, .Position = position,
                                   .Density = desc.Density, .Friction = 0.6f, .Name = desc.Name + "." + part }));
    };

    ragdoll.Pelvis = Tinted(AddBox({ .Scale = { 0.32f * s, 0.20f * s, 0.20f * s }, .Position = P(0.0f, 0.96f, 0.0f),
                                     .Density = desc.Density, .Friction = 0.6f, .Name = desc.Name + ".Pelvis" }));
    ragdoll.Chest = Tinted(AddBox({ .Scale = { 0.38f * s, 0.40f * s, 0.22f * s }, .Position = P(0.0f, 1.29f, 0.0f),
                                    .Density = desc.Density, .Friction = 0.6f, .Name = desc.Name + ".Chest" }));
    ragdoll.Head = Tinted(AddSphere({ .Radius = 0.11f * s, .Position = P(0.0f, 1.63f, 0.0f), .Density = desc.Density,
                                      .Friction = 0.6f, .Name = desc.Name + ".Head" }));

    ragdoll.UpperArmLeft = Capsule(0.055f, 0.32f, P(-0.26f, 1.31f, 0.0f), "UpperArmLeft");
    ragdoll.LowerArmLeft = Capsule(0.05f, 0.30f, P(-0.26f, 0.99f, 0.0f), "LowerArmLeft");
    ragdoll.UpperArmRight = Capsule(0.055f, 0.32f, P(0.26f, 1.31f, 0.0f), "UpperArmRight");
    ragdoll.LowerArmRight = Capsule(0.05f, 0.30f, P(0.26f, 0.99f, 0.0f), "LowerArmRight");

    ragdoll.UpperLegLeft = Capsule(0.075f, 0.44f, P(-0.1f, 0.64f, 0.0f), "UpperLegLeft");
    ragdoll.LowerLegLeft = Capsule(0.065f, 0.42f, P(-0.1f, 0.22f, 0.0f), "LowerLegLeft");
    ragdoll.UpperLegRight = Capsule(0.075f, 0.44f, P(0.1f, 0.64f, 0.0f), "UpperLegRight");
    ragdoll.LowerLegRight = Capsule(0.065f, 0.42f, P(0.1f, 0.22f, 0.0f), "LowerLegRight");

    auto Cone = [&](Actor3D a, Actor3D b, Vec3 anchor, Vec3 axis, float cone, float twist) {
        return AddConeJoint({ .BodyA = a, .BodyB = b, .Anchor = anchor, .Axis = axis, .ConeAngle = cone,
                              .LowerTwist = -twist, .UpperTwist = twist, .EnableSpring = desc.Stiff,
                              .Hertz = 1.5f, .DampingRatio = 0.9f });
    };

    // A hinge about +X: a positive angle swings the lower part backward (-Z),
    // so an elbow bends through negative angles and a knee through positive.
    auto Hinge = [&](Actor3D a, Actor3D b, Vec3 anchor, float lower, float upper) {
        return AddHinge({ .BodyA = a, .BodyB = b, .Anchor = anchor, .Axis = { 1.0f, 0.0f, 0.0f },
                          .EnableLimit = true, .LowerAngle = lower, .UpperAngle = upper });
    };

    const Vec3 up{ 0.0f, 1.0f, 0.0f };
    const Vec3 down{ 0.0f, -1.0f, 0.0f };

    ragdoll.Spine = Cone(ragdoll.Pelvis, ragdoll.Chest, P(0.0f, 1.08f, 0.0f), up, 0.45f, 0.35f);
    ragdoll.Neck = Cone(ragdoll.Chest, ragdoll.Head, P(0.0f, 1.51f, 0.0f), up, 0.6f, 0.8f);
    ragdoll.ShoulderLeft = Cone(ragdoll.Chest, ragdoll.UpperArmLeft, P(-0.26f, 1.46f, 0.0f), down, 1.4f, 0.8f);
    ragdoll.ShoulderRight = Cone(ragdoll.Chest, ragdoll.UpperArmRight, P(0.26f, 1.46f, 0.0f), down, 1.4f, 0.8f);
    ragdoll.HipLeft = Cone(ragdoll.Pelvis, ragdoll.UpperLegLeft, P(-0.1f, 0.86f, 0.0f), down, 1.2f, 0.5f);
    ragdoll.HipRight = Cone(ragdoll.Pelvis, ragdoll.UpperLegRight, P(0.1f, 0.86f, 0.0f), down, 1.2f, 0.5f);

    ragdoll.ElbowLeft = Hinge(ragdoll.UpperArmLeft, ragdoll.LowerArmLeft, P(-0.26f, 1.15f, 0.0f), -2.4f, 0.0f);
    ragdoll.ElbowRight = Hinge(ragdoll.UpperArmRight, ragdoll.LowerArmRight, P(0.26f, 1.15f, 0.0f), -2.4f, 0.0f);
    ragdoll.KneeLeft = Hinge(ragdoll.UpperLegLeft, ragdoll.LowerLegLeft, P(-0.1f, 0.43f, 0.0f), 0.0f, 2.4f);
    ragdoll.KneeRight = Hinge(ragdoll.UpperLegRight, ragdoll.LowerLegRight, P(0.1f, 0.43f, 0.0f), 0.0f, 2.4f);

    return ragdoll;
}

Joint3D World3D::AddSlider(const SliderDesc& desc)
{
    Joint3D joint;
    if (m_State == nullptr || !detail::IsFinite(desc.Anchor) || !ValidAxis(desc.Axis, "Slider axis"))
    {
        return joint;
    }

    b3PrismaticJointDef def = b3DefaultPrismaticJointDef();
    b3BodyId bodyA{};
    b3BodyId bodyB{};

    if (!PrepareJoint(*m_State, desc.BodyA, desc.BodyB, desc.CollideConnected, def.base, bodyA, bodyB))
    {
        return joint;
    }

    // A slider travels along its frame's x-axis.
    const Quat frame = detail::QuatFromTo(Vec3{ 1.0f, 0.0f, 0.0f }, desc.Axis);

    def.base.localFrameA = MakeLocalFrame(bodyA, desc.Anchor, frame);
    def.base.localFrameB = MakeLocalFrame(bodyB, desc.Anchor, frame);

    def.enableLimit = desc.EnableLimit;
    def.lowerTranslation = desc.LowerTranslation;
    def.upperTranslation = desc.UpperTranslation;
    def.enableMotor = desc.EnableMotor;
    def.motorSpeed = desc.MotorSpeed;
    def.maxMotorForce = desc.MaxMotorForce;
    def.enableSpring = desc.EnableSpring;
    def.hertz = std::max(0.0f, desc.Hertz);
    def.dampingRatio = std::max(0.0f, desc.DampingRatio);

    joint.m_State = m_State;
    joint.m_Id = RegisterJoint(*m_State, b3CreatePrismaticJoint(m_State->World, &def), JointKind::Slider);
    return joint;
}

Joint3D World3D::AddDistanceJoint(const DistanceJointDesc& desc)
{
    Joint3D joint;
    if (m_State == nullptr || !detail::IsFinite(desc.AnchorA) || !detail::IsFinite(desc.AnchorB))
    {
        return joint;
    }

    b3DistanceJointDef def = b3DefaultDistanceJointDef();
    b3BodyId bodyA{};
    b3BodyId bodyB{};

    if (!PrepareJoint(*m_State, desc.BodyA, desc.BodyB, desc.CollideConnected, def.base, bodyA, bodyB))
    {
        return joint;
    }

    // Only the anchor points matter here, so the frames keep each body's own
    // orientation and contribute nothing.
    def.base.localFrameA = MakeLocalFrame(bodyA, desc.AnchorA, detail::FromB3(b3Body_GetRotation(bodyA)));
    def.base.localFrameB = MakeLocalFrame(bodyB, desc.AnchorB, detail::FromB3(b3Body_GetRotation(bodyB)));

    float length = desc.Length;
    if (!(length > 0.0f))
    {
        // Whatever the anchors are apart right now, so building from placed
        // bodies does not yank them together.
        const float dx = desc.AnchorB.X - desc.AnchorA.X;
        const float dy = desc.AnchorB.Y - desc.AnchorA.Y;
        const float dz = desc.AnchorB.Z - desc.AnchorA.Z;
        length = std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    def.length = std::max(0.005f, length);
    def.enableSpring = desc.EnableSpring;
    def.hertz = desc.Hertz;
    def.dampingRatio = desc.DampingRatio;
    def.enableLimit = desc.EnableLimit;
    def.minLength = desc.MinLength;
    def.maxLength = desc.EnableLimit ? std::max(desc.MinLength, desc.MaxLength) : def.length;

    joint.m_State = m_State;
    joint.m_Id = RegisterJoint(*m_State, b3CreateDistanceJoint(m_State->World, &def), JointKind::Distance);
    return joint;
}

Joint3D World3D::AddWeld(const WeldDesc& desc)
{
    Joint3D joint;
    if (m_State == nullptr || !detail::IsFinite(desc.Anchor))
    {
        return joint;
    }

    b3WeldJointDef def = b3DefaultWeldJointDef();
    b3BodyId bodyA{};
    b3BodyId bodyB{};

    if (!PrepareJoint(*m_State, desc.BodyA, desc.BodyB, desc.CollideConnected, def.base, bodyA, bodyB))
    {
        return joint;
    }

    // Each frame keeps its own body's rotation, so the weld preserves however
    // the two are oriented now instead of snapping them into alignment.
    def.base.localFrameA = MakeLocalFrame(bodyA, desc.Anchor, detail::FromB3(b3Body_GetRotation(bodyA)));
    def.base.localFrameB = MakeLocalFrame(bodyB, desc.Anchor, detail::FromB3(b3Body_GetRotation(bodyB)));

    def.linearHertz = desc.LinearHertz;
    def.angularHertz = desc.AngularHertz;
    def.linearDampingRatio = desc.LinearDampingRatio;
    def.angularDampingRatio = desc.AngularDampingRatio;

    joint.m_State = m_State;
    joint.m_Id = RegisterJoint(*m_State, b3CreateWeldJoint(m_State->World, &def), JointKind::Weld);
    return joint;
}

size_t World3D::GetJointCount() const
{
    return m_State != nullptr ? m_State->LiveJoints : 0;
}

// ---------------------------------------------------------------------------
// Joint3D
// ---------------------------------------------------------------------------

bool Joint3D::IsValid() const
{
    if (m_State == nullptr || !m_Id.IsValid() || m_Id.Index >= m_State->Joints.size())
    {
        return false;
    }

    const JointRecord3& record = m_State->Joints[m_Id.Index];

    // Box3D destroys a joint when either body goes, so the backend is asked
    // as well as the generation.
    return record.Alive && record.Generation == m_Id.Generation && b3Joint_IsValid(record.Joint);
}

JointKind Joint3D::GetKind() const
{
    const JointRecord3* record = detail::Resolve(m_State, m_Id, "GetKind");
    return record != nullptr ? record->Kind : JointKind::Hinge;
}

float Joint3D::GetAngle() const
{
    const JointRecord3* record = detail::Resolve(m_State, m_Id, "GetAngle");
    if (record == nullptr)
    {
        return 0.0f;
    }

    if (record->Kind == JointKind::Cone)
    {
        return b3SphericalJoint_GetConeAngle(record->Joint);
    }
    if (record->Kind != JointKind::Hinge)
    {
        LogMessage(LogLevel::Warning, "joint", "GetAngle is only meaningful on a hinge or a cone joint.");
        return 0.0f;
    }

    return b3RevoluteJoint_GetAngle(record->Joint);
}

float Joint3D::GetTranslation() const
{
    const JointRecord3* record = detail::Resolve(m_State, m_Id, "GetTranslation");
    if (record == nullptr)
    {
        return 0.0f;
    }

    switch (record->Kind)
    {
        case JointKind::Slider:   return b3PrismaticJoint_GetTranslation(record->Joint);
        case JointKind::Distance: return b3DistanceJoint_GetCurrentLength(record->Joint);
        default: break;
    }

    LogMessage(LogLevel::Warning, "joint",
               "GetTranslation is only meaningful on a slider or a distance joint.");
    return 0.0f;
}

void Joint3D::EnableLimit(bool enabled)
{
    JointRecord3* record = detail::Resolve(m_State, m_Id, "EnableLimit");
    if (record == nullptr)
    {
        return;
    }

    switch (record->Kind)
    {
        case JointKind::Hinge:    b3RevoluteJoint_EnableLimit(record->Joint, enabled); break;
        case JointKind::Slider:   b3PrismaticJoint_EnableLimit(record->Joint, enabled); break;
        case JointKind::Distance: b3DistanceJoint_EnableLimit(record->Joint, enabled); break;
        case JointKind::Cone:
            b3SphericalJoint_EnableConeLimit(record->Joint, enabled);
            b3SphericalJoint_EnableTwistLimit(record->Joint, enabled);
            break;
        case JointKind::Weld:
        case JointKind::Wheel:
            LogMessage(LogLevel::Warning, "joint", "A weld joint has no limit to enable.");
            break;
    }

    // Changing a joint does nothing to bodies that are asleep, so they are
    // woken to feel it.
    b3Joint_WakeBodies(record->Joint);
}

void Joint3D::SetLimits(float lower, float upper)
{
    JointRecord3* record = detail::Resolve(m_State, m_Id, "SetLimits");
    if (record == nullptr || !detail::IsFinite(lower) || !detail::IsFinite(upper))
    {
        return;
    }

    if (upper < lower)
    {
        std::swap(lower, upper);
    }

    switch (record->Kind)
    {
        case JointKind::Hinge:    b3RevoluteJoint_SetLimits(record->Joint, lower, upper); break;
        case JointKind::Slider:   b3PrismaticJoint_SetLimits(record->Joint, lower, upper); break;
        case JointKind::Distance: b3DistanceJoint_SetLengthRange(record->Joint, lower, upper); break;
        case JointKind::Cone:
            b3SphericalJoint_SetTwistLimits(record->Joint, std::clamp(lower, -0.99f * 3.14159265f, 0.0f),
                                            std::clamp(upper, 0.0f, 0.99f * 3.14159265f));
            break;
        case JointKind::Weld:
        case JointKind::Wheel:
            LogMessage(LogLevel::Warning, "joint", "A weld joint has no limits to set.");
            break;
    }
    b3Joint_WakeBodies(record->Joint);
}

void Joint3D::EnableMotor(bool enabled)
{
    JointRecord3* record = detail::Resolve(m_State, m_Id, "EnableMotor");
    if (record == nullptr)
    {
        return;
    }

    switch (record->Kind)
    {
        case JointKind::Hinge:    b3RevoluteJoint_EnableMotor(record->Joint, enabled); break;
        case JointKind::Slider:   b3PrismaticJoint_EnableMotor(record->Joint, enabled); break;
        case JointKind::Distance: b3DistanceJoint_EnableMotor(record->Joint, enabled); break;
        case JointKind::Cone:     b3SphericalJoint_EnableMotor(record->Joint, enabled); break;
        case JointKind::Weld:
        case JointKind::Wheel:
            LogMessage(LogLevel::Warning, "joint", "A weld joint has no motor.");
            break;
    }
    b3Joint_WakeBodies(record->Joint);
}

void Joint3D::SetMotorSpeed(float speed)
{
    JointRecord3* record = detail::Resolve(m_State, m_Id, "SetMotorSpeed");
    if (record == nullptr || !detail::IsFinite(speed))
    {
        return;
    }

    switch (record->Kind)
    {
        case JointKind::Hinge:    b3RevoluteJoint_SetMotorSpeed(record->Joint, speed); break;
        case JointKind::Slider:   b3PrismaticJoint_SetMotorSpeed(record->Joint, speed); break;
        case JointKind::Distance: b3DistanceJoint_SetMotorSpeed(record->Joint, speed); break;
        case JointKind::Cone:
            LogMessage(LogLevel::Warning, "joint", "A cone joint's motor takes a velocity: use SetMotorVelocity.");
            break;
        case JointKind::Weld:
        case JointKind::Wheel: break;
    }
    b3Joint_WakeBodies(record->Joint);
}

void Joint3D::SetMaxMotorEffort(float effort)
{
    JointRecord3* record = detail::Resolve(m_State, m_Id, "SetMaxMotorEffort");
    if (record == nullptr || !detail::IsFinite(effort))
    {
        return;
    }

    switch (record->Kind)
    {
        case JointKind::Hinge:    b3RevoluteJoint_SetMaxMotorTorque(record->Joint, effort); break;
        case JointKind::Slider:   b3PrismaticJoint_SetMaxMotorForce(record->Joint, effort); break;
        case JointKind::Distance: b3DistanceJoint_SetMaxMotorForce(record->Joint, effort); break;
        case JointKind::Cone:     b3SphericalJoint_SetMaxMotorTorque(record->Joint, std::max(0.0f, effort)); break;
        case JointKind::Weld:
        case JointKind::Wheel: break;
    }
    b3Joint_WakeBodies(record->Joint);
}

float Joint3D::GetMotorEffort() const
{
    const JointRecord3* record = detail::Resolve(m_State, m_Id, "GetMotorEffort");
    if (record == nullptr)
    {
        return 0.0f;
    }

    switch (record->Kind)
    {
        case JointKind::Hinge:    return b3RevoluteJoint_GetMotorTorque(record->Joint);
        case JointKind::Slider:   return b3PrismaticJoint_GetMotorForce(record->Joint);
        case JointKind::Distance: return b3DistanceJoint_GetMotorForce(record->Joint);
        case JointKind::Cone:
        {
            const b3Vec3 torque = b3SphericalJoint_GetMotorTorque(record->Joint);
            return std::sqrt(torque.x * torque.x + torque.y * torque.y + torque.z * torque.z);
        }

        // A weld has no motor, so there is no effort to report.
        case JointKind::Weld:
        case JointKind::Wheel: break;
    }

    return 0.0f;
}

void Joint3D::SetLength(float length)
{
    JointRecord3* record = detail::Resolve(m_State, m_Id, "SetLength");
    if (record == nullptr || !detail::IsFinite(length))
    {
        return;
    }

    if (record->Kind != JointKind::Distance)
    {
        LogMessage(LogLevel::Warning, "joint", "SetLength is only meaningful on a distance joint.");
        return;
    }

    b3DistanceJoint_SetLength(record->Joint, std::max(0.005f, length));
    b3Joint_WakeBodies(record->Joint);
}

void Joint3D::EnableSpring(bool enabled)
{
    JointRecord3* record = detail::Resolve(m_State, m_Id, "EnableSpring");
    if (record == nullptr)
    {
        return;
    }

    switch (record->Kind)
    {
        case JointKind::Hinge:    b3RevoluteJoint_EnableSpring(record->Joint, enabled); break;
        case JointKind::Slider:   b3PrismaticJoint_EnableSpring(record->Joint, enabled); break;
        case JointKind::Distance: b3DistanceJoint_EnableSpring(record->Joint, enabled); break;
        case JointKind::Cone:     b3SphericalJoint_EnableSpring(record->Joint, enabled); break;
        case JointKind::Weld:
        case JointKind::Wheel:
            LogMessage(LogLevel::Warning, "joint",
                       "A weld's springiness is set by its hertz values: use SetSpring.");
            return;
    }
    b3Joint_WakeBodies(record->Joint);
}

void Joint3D::SetSpring(float hertz, float dampingRatio)
{
    JointRecord3* record = detail::Resolve(m_State, m_Id, "SetSpring");
    if (record == nullptr || !detail::IsFinite(hertz) || !detail::IsFinite(dampingRatio))
    {
        return;
    }

    hertz = std::max(0.0f, hertz);
    dampingRatio = std::max(0.0f, dampingRatio);

    switch (record->Kind)
    {
        case JointKind::Hinge:
            b3RevoluteJoint_SetSpringHertz(record->Joint, hertz);
            b3RevoluteJoint_SetSpringDampingRatio(record->Joint, dampingRatio);
            break;
        case JointKind::Slider:
            b3PrismaticJoint_SetSpringHertz(record->Joint, hertz);
            b3PrismaticJoint_SetSpringDampingRatio(record->Joint, dampingRatio);
            break;
        case JointKind::Distance:
            b3DistanceJoint_SetSpringHertz(record->Joint, hertz);
            b3DistanceJoint_SetSpringDampingRatio(record->Joint, dampingRatio);
            break;
        case JointKind::Cone:
            b3SphericalJoint_SetSpringHertz(record->Joint, hertz);
            b3SphericalJoint_SetSpringDampingRatio(record->Joint, dampingRatio);
            break;
        case JointKind::Weld:
            b3WeldJoint_SetLinearHertz(record->Joint, hertz);
            b3WeldJoint_SetAngularHertz(record->Joint, hertz);
            b3WeldJoint_SetLinearDampingRatio(record->Joint, dampingRatio);
            b3WeldJoint_SetAngularDampingRatio(record->Joint, dampingRatio);
            break;
        case JointKind::Wheel:
            return;
    }
    b3Joint_WakeBodies(record->Joint);
}

void Joint3D::SetConeAngle(float radians)
{
    JointRecord3* record = detail::Resolve(m_State, m_Id, "SetConeAngle");
    if (record == nullptr || !detail::IsFinite(radians))
    {
        return;
    }
    if (record->Kind != JointKind::Cone)
    {
        LogMessage(LogLevel::Warning, "joint", "SetConeAngle is only meaningful on a cone joint.");
        return;
    }
    b3SphericalJoint_SetConeLimit(record->Joint, std::clamp(radians, 0.0f, 0.5f * 3.14159265f));
    b3Joint_WakeBodies(record->Joint);
}

float Joint3D::GetConeAngle() const
{
    const JointRecord3* record = detail::Resolve(m_State, m_Id, "GetConeAngle");
    if (record == nullptr || record->Kind != JointKind::Cone)
    {
        return 0.0f;
    }
    return b3SphericalJoint_GetConeAngle(record->Joint);
}

float Joint3D::GetTwistAngle() const
{
    const JointRecord3* record = detail::Resolve(m_State, m_Id, "GetTwistAngle");
    if (record == nullptr || record->Kind != JointKind::Cone)
    {
        return 0.0f;
    }
    return b3SphericalJoint_GetTwistAngle(record->Joint);
}

void Joint3D::SetMotorVelocity(Vec3 velocity)
{
    JointRecord3* record = detail::Resolve(m_State, m_Id, "SetMotorVelocity");
    if (record == nullptr || !detail::IsFinite(velocity))
    {
        return;
    }
    if (record->Kind != JointKind::Cone)
    {
        LogMessage(LogLevel::Warning, "joint", "SetMotorVelocity is only meaningful on a cone joint.");
        return;
    }
    b3SphericalJoint_SetMotorVelocity(record->Joint, detail::ToB3(velocity));
    b3Joint_WakeBodies(record->Joint);
}

void Joint3D::Destroy()
{
    if (detail::Resolve(m_State, m_Id, "Destroy") == nullptr)
    {
        return;
    }

    // Deferred, like destroying an actor, so this is safe from inside a
    // collision handler.
    m_State->PendingJointDestroys.push_back(m_Id);
}

} // namespace ludifex
