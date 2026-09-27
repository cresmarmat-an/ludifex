// Joints for World2D.
//
// Box2D describes a joint with an anchor on each body in that body's own
// space, and for the joints that slide, an axis on the first body. As in 3D,
// this file takes a world-space pivot and axis and converts them from where
// the bodies stand now, which is why a joint can be made between placed bodies
// without either jumping when it starts. The angle the two bodies have
// between them at that moment is the joint's zero.

#include "Internal.h"

#include <algorithm>
#include <cmath>

namespace ludifex
{
namespace
{

using detail::ActorRecord2;
using detail::JointRecord2;
using detail::World2DState;

// Box2D refuses hinge limits beyond a little under half a turn.
constexpr float MaxHingeAngle = 0.99f * 3.14159265f;

bool PrepareJoint(World2DState& state, const Actor2D& actorA, const Actor2D& actorB, b2BodyId& outBodyA,
                  b2BodyId& outBodyB)
{
    const ActorRecord2* recordA = detail::Resolve(&state, actorA.GetId(), "Creating a joint");
    const ActorRecord2* recordB = detail::Resolve(&state, actorB.GetId(), "Creating a joint");

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
    return true;
}

JointId RegisterJoint(World2DState& state, b2JointId joint, JointKind kind)
{
    if (!b2Joint_IsValid(joint))
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

    JointRecord2& record = state.Joints[index];
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

// The unit direction of an axis, or false with a diagnostic when it names none.
bool UnitAxis(const Vec2& axis, const char* what, b2Vec2& out)
{
    if (!detail::IsFinite(axis))
    {
        LogMessage(LogLevel::Error, "joint", "%s is not finite.", what);
        return false;
    }

    const float length = std::sqrt(axis.X * axis.X + axis.Y * axis.Y);
    if (length < 1e-6f)
    {
        LogMessage(LogLevel::Error, "joint", "%s has no length, so it names no direction.", what);
        return false;
    }

    out = b2Vec2{ axis.X / length, axis.Y / length };
    return true;
}

float ReferenceAngle(b2BodyId bodyA, b2BodyId bodyB)
{
    return b2RelativeAngle(b2Body_GetRotation(bodyB), b2Body_GetRotation(bodyA));
}

// Orders a range and keeps it where the solver accepts it.
void OrderRange(float& lower, float& upper)
{
    if (upper < lower)
    {
        std::swap(lower, upper);
    }
}

void HingeRange(float& lower, float& upper)
{
    OrderRange(lower, upper);
    lower = std::clamp(lower, -MaxHingeAngle, MaxHingeAngle);
    upper = std::clamp(upper, -MaxHingeAngle, MaxHingeAngle);
}

float NonNegative(float value)
{
    return detail::IsFinite(value) ? std::max(0.0f, value) : 0.0f;
}

} // namespace

// ---------------------------------------------------------------------------
// Creation
// ---------------------------------------------------------------------------

Joint2D World2D::AddHinge(const HingeDesc2D& desc)
{
    Joint2D joint;
    b2BodyId bodyA{};
    b2BodyId bodyB{};
    if (m_State == nullptr || !detail::IsFinite(desc.Anchor) ||
        !PrepareJoint(*m_State, desc.BodyA, desc.BodyB, bodyA, bodyB))
    {
        return joint;
    }

    b2RevoluteJointDef def = b2DefaultRevoluteJointDef();
    def.bodyIdA = bodyA;
    def.bodyIdB = bodyB;
    def.localAnchorA = b2Body_GetLocalPoint(bodyA, detail::ToB2(desc.Anchor));
    def.localAnchorB = b2Body_GetLocalPoint(bodyB, detail::ToB2(desc.Anchor));
    def.referenceAngle = ReferenceAngle(bodyA, bodyB);

    float lower = desc.LowerAngle;
    float upper = desc.UpperAngle;
    HingeRange(lower, upper);
    def.enableLimit = desc.EnableLimit;
    def.lowerAngle = lower;
    def.upperAngle = upper;

    def.enableMotor = desc.EnableMotor;
    def.motorSpeed = detail::IsFinite(desc.MotorSpeed) ? desc.MotorSpeed : 0.0f;
    def.maxMotorTorque = NonNegative(desc.MaxMotorTorque);

    def.enableSpring = desc.EnableSpring;
    def.hertz = NonNegative(desc.Hertz);
    def.dampingRatio = NonNegative(desc.DampingRatio);
    def.collideConnected = desc.CollideConnected;

    joint.m_State = m_State;
    joint.m_Id = RegisterJoint(*m_State, b2CreateRevoluteJoint(m_State->World, &def), JointKind::Hinge);
    return joint;
}

Joint2D World2D::AddSlider(const SliderDesc2D& desc)
{
    Joint2D joint;
    b2BodyId bodyA{};
    b2BodyId bodyB{};
    b2Vec2 axis{};
    if (m_State == nullptr || !detail::IsFinite(desc.Anchor) || !UnitAxis(desc.Axis, "Slider axis", axis) ||
        !PrepareJoint(*m_State, desc.BodyA, desc.BodyB, bodyA, bodyB))
    {
        return joint;
    }

    b2PrismaticJointDef def = b2DefaultPrismaticJointDef();
    def.bodyIdA = bodyA;
    def.bodyIdB = bodyB;
    def.localAnchorA = b2Body_GetLocalPoint(bodyA, detail::ToB2(desc.Anchor));
    def.localAnchorB = b2Body_GetLocalPoint(bodyB, detail::ToB2(desc.Anchor));
    def.localAxisA = b2Body_GetLocalVector(bodyA, axis);
    def.referenceAngle = ReferenceAngle(bodyA, bodyB);

    float lower = desc.LowerTranslation;
    float upper = desc.UpperTranslation;
    OrderRange(lower, upper);
    def.enableLimit = desc.EnableLimit;
    def.lowerTranslation = lower;
    def.upperTranslation = upper;

    def.enableMotor = desc.EnableMotor;
    def.motorSpeed = detail::IsFinite(desc.MotorSpeed) ? desc.MotorSpeed : 0.0f;
    def.maxMotorForce = NonNegative(desc.MaxMotorForce);

    def.enableSpring = desc.EnableSpring;
    def.hertz = NonNegative(desc.Hertz);
    def.dampingRatio = NonNegative(desc.DampingRatio);
    def.collideConnected = desc.CollideConnected;

    joint.m_State = m_State;
    joint.m_Id = RegisterJoint(*m_State, b2CreatePrismaticJoint(m_State->World, &def), JointKind::Slider);
    return joint;
}

Joint2D World2D::AddDistanceJoint(const DistanceJointDesc2D& desc)
{
    Joint2D joint;
    b2BodyId bodyA{};
    b2BodyId bodyB{};
    if (m_State == nullptr || !detail::IsFinite(desc.AnchorA) || !detail::IsFinite(desc.AnchorB) ||
        !PrepareJoint(*m_State, desc.BodyA, desc.BodyB, bodyA, bodyB))
    {
        return joint;
    }

    b2DistanceJointDef def = b2DefaultDistanceJointDef();
    def.bodyIdA = bodyA;
    def.bodyIdB = bodyB;
    def.localAnchorA = b2Body_GetLocalPoint(bodyA, detail::ToB2(desc.AnchorA));
    def.localAnchorB = b2Body_GetLocalPoint(bodyB, detail::ToB2(desc.AnchorB));

    float length = desc.Length;
    if (!(length > 0.0f) || !detail::IsFinite(length))
    {
        // Whatever the anchors are apart now, so placed bodies are not yanked.
        const float dx = desc.AnchorB.X - desc.AnchorA.X;
        const float dy = desc.AnchorB.Y - desc.AnchorA.Y;
        length = std::sqrt(dx * dx + dy * dy);
    }

    def.length = std::max(0.005f, length);
    def.enableSpring = desc.EnableSpring;
    def.hertz = NonNegative(desc.Hertz);
    def.dampingRatio = NonNegative(desc.DampingRatio);

    // Without a limit the default range, which is unbounded, is kept.
    def.enableLimit = desc.EnableLimit;
    if (desc.EnableLimit)
    {
        float lower = desc.MinLength;
        float upper = desc.MaxLength;
        OrderRange(lower, upper);
        def.minLength = std::max(0.0f, lower);
        def.maxLength = std::max(def.minLength, upper);
    }
    def.collideConnected = desc.CollideConnected;

    joint.m_State = m_State;
    joint.m_Id = RegisterJoint(*m_State, b2CreateDistanceJoint(m_State->World, &def), JointKind::Distance);
    return joint;
}

Joint2D World2D::AddWeld(const WeldDesc2D& desc)
{
    Joint2D joint;
    b2BodyId bodyA{};
    b2BodyId bodyB{};
    if (m_State == nullptr || !detail::IsFinite(desc.Anchor) ||
        !PrepareJoint(*m_State, desc.BodyA, desc.BodyB, bodyA, bodyB))
    {
        return joint;
    }

    b2WeldJointDef def = b2DefaultWeldJointDef();
    def.bodyIdA = bodyA;
    def.bodyIdB = bodyB;
    def.localAnchorA = b2Body_GetLocalPoint(bodyA, detail::ToB2(desc.Anchor));
    def.localAnchorB = b2Body_GetLocalPoint(bodyB, detail::ToB2(desc.Anchor));
    def.referenceAngle = ReferenceAngle(bodyA, bodyB);
    def.linearHertz = NonNegative(desc.LinearHertz);
    def.angularHertz = NonNegative(desc.AngularHertz);
    def.linearDampingRatio = NonNegative(desc.LinearDampingRatio);
    def.angularDampingRatio = NonNegative(desc.AngularDampingRatio);
    def.collideConnected = desc.CollideConnected;

    joint.m_State = m_State;
    joint.m_Id = RegisterJoint(*m_State, b2CreateWeldJoint(m_State->World, &def), JointKind::Weld);
    return joint;
}

Joint2D World2D::AddWheel(const WheelDesc2D& desc)
{
    Joint2D joint;
    b2BodyId bodyA{};
    b2BodyId bodyB{};
    b2Vec2 axis{};
    if (m_State == nullptr || !detail::IsFinite(desc.Anchor) || !UnitAxis(desc.Axis, "Wheel axis", axis) ||
        !PrepareJoint(*m_State, desc.BodyA, desc.BodyB, bodyA, bodyB))
    {
        return joint;
    }

    b2WheelJointDef def = b2DefaultWheelJointDef();
    def.bodyIdA = bodyA;
    def.bodyIdB = bodyB;
    def.localAnchorA = b2Body_GetLocalPoint(bodyA, detail::ToB2(desc.Anchor));
    def.localAnchorB = b2Body_GetLocalPoint(bodyB, detail::ToB2(desc.Anchor));
    def.localAxisA = b2Body_GetLocalVector(bodyA, axis);

    def.enableSpring = desc.EnableSpring;
    def.hertz = NonNegative(desc.Hertz);
    def.dampingRatio = NonNegative(desc.DampingRatio);

    float lower = desc.LowerTranslation;
    float upper = desc.UpperTranslation;
    OrderRange(lower, upper);
    def.enableLimit = desc.EnableLimit;
    def.lowerTranslation = lower;
    def.upperTranslation = upper;

    def.enableMotor = desc.EnableMotor;
    def.motorSpeed = detail::IsFinite(desc.MotorSpeed) ? desc.MotorSpeed : 0.0f;
    def.maxMotorTorque = NonNegative(desc.MaxMotorTorque);
    def.collideConnected = desc.CollideConnected;

    joint.m_State = m_State;
    joint.m_Id = RegisterJoint(*m_State, b2CreateWheelJoint(m_State->World, &def), JointKind::Wheel);
    return joint;
}

size_t World2D::GetJointCount() const
{
    return m_State != nullptr ? m_State->LiveJoints : 0;
}

// ---------------------------------------------------------------------------
// Joint2D
// ---------------------------------------------------------------------------

bool Joint2D::IsValid() const
{
    if (m_State == nullptr || !m_Id.IsValid() || m_Id.Index >= m_State->Joints.size())
    {
        return false;
    }

    const JointRecord2& record = m_State->Joints[m_Id.Index];
    return record.Alive && record.Generation == m_Id.Generation && b2Joint_IsValid(record.Joint);
}

JointKind Joint2D::GetKind() const
{
    const JointRecord2* record = detail::Resolve(m_State, m_Id, "GetKind");
    return record != nullptr ? record->Kind : JointKind::Hinge;
}

float Joint2D::GetAngle() const
{
    const JointRecord2* record = detail::Resolve(m_State, m_Id, "GetAngle");
    if (record == nullptr)
    {
        return 0.0f;
    }

    if (record->Kind != JointKind::Hinge)
    {
        LogMessage(LogLevel::Warning, "joint", "GetAngle is only meaningful on a hinge.");
        return 0.0f;
    }

    return b2RevoluteJoint_GetAngle(record->Joint);
}

float Joint2D::GetTranslation() const
{
    const JointRecord2* record = detail::Resolve(m_State, m_Id, "GetTranslation");
    if (record == nullptr)
    {
        return 0.0f;
    }

    switch (record->Kind)
    {
        case JointKind::Slider:   return b2PrismaticJoint_GetTranslation(record->Joint);
        case JointKind::Distance: return b2DistanceJoint_GetCurrentLength(record->Joint);
        default: break;
    }

    LogMessage(LogLevel::Warning, "joint", "GetTranslation is only meaningful on a slider or a distance joint.");
    return 0.0f;
}

void Joint2D::EnableLimit(bool enabled)
{
    JointRecord2* record = detail::Resolve(m_State, m_Id, "EnableLimit");
    if (record == nullptr)
    {
        return;
    }

    switch (record->Kind)
    {
        case JointKind::Hinge:    b2RevoluteJoint_EnableLimit(record->Joint, enabled); break;
        case JointKind::Slider:   b2PrismaticJoint_EnableLimit(record->Joint, enabled); break;
        case JointKind::Distance: b2DistanceJoint_EnableLimit(record->Joint, enabled); break;
        case JointKind::Wheel:    b2WheelJoint_EnableLimit(record->Joint, enabled); break;
        case JointKind::Weld:
        case JointKind::Cone:
            LogMessage(LogLevel::Warning, "joint", "This joint has no limit to enable.");
            return;
    }

    // Changing a joint does nothing to bodies that are asleep, so they are
    // woken to feel it.
    b2Joint_WakeBodies(record->Joint);
}

void Joint2D::SetLimits(float lower, float upper)
{
    JointRecord2* record = detail::Resolve(m_State, m_Id, "SetLimits");
    if (record == nullptr || !detail::IsFinite(lower) || !detail::IsFinite(upper))
    {
        return;
    }

    OrderRange(lower, upper);

    switch (record->Kind)
    {
        case JointKind::Hinge:
            HingeRange(lower, upper);
            b2RevoluteJoint_SetLimits(record->Joint, lower, upper);
            break;
        case JointKind::Slider:   b2PrismaticJoint_SetLimits(record->Joint, lower, upper); break;
        case JointKind::Distance: b2DistanceJoint_SetLengthRange(record->Joint, std::max(0.0f, lower), upper); break;
        case JointKind::Wheel:    b2WheelJoint_SetLimits(record->Joint, lower, upper); break;
        case JointKind::Weld:
        case JointKind::Cone:
            LogMessage(LogLevel::Warning, "joint", "This joint has no limits to set.");
            return;
    }

    b2Joint_WakeBodies(record->Joint);
}

void Joint2D::EnableMotor(bool enabled)
{
    JointRecord2* record = detail::Resolve(m_State, m_Id, "EnableMotor");
    if (record == nullptr)
    {
        return;
    }

    switch (record->Kind)
    {
        case JointKind::Hinge:    b2RevoluteJoint_EnableMotor(record->Joint, enabled); break;
        case JointKind::Slider:   b2PrismaticJoint_EnableMotor(record->Joint, enabled); break;
        case JointKind::Distance: b2DistanceJoint_EnableMotor(record->Joint, enabled); break;
        case JointKind::Wheel:    b2WheelJoint_EnableMotor(record->Joint, enabled); break;
        case JointKind::Weld:
        case JointKind::Cone:
            LogMessage(LogLevel::Warning, "joint", "This joint has no motor.");
            return;
    }

    b2Joint_WakeBodies(record->Joint);
}

void Joint2D::SetMotorSpeed(float speed)
{
    JointRecord2* record = detail::Resolve(m_State, m_Id, "SetMotorSpeed");
    if (record == nullptr || !detail::IsFinite(speed))
    {
        return;
    }

    switch (record->Kind)
    {
        case JointKind::Hinge:    b2RevoluteJoint_SetMotorSpeed(record->Joint, speed); break;
        case JointKind::Slider:   b2PrismaticJoint_SetMotorSpeed(record->Joint, speed); break;
        case JointKind::Distance: b2DistanceJoint_SetMotorSpeed(record->Joint, speed); break;
        case JointKind::Wheel:    b2WheelJoint_SetMotorSpeed(record->Joint, speed); break;
        case JointKind::Weld:
        case JointKind::Cone:     return;
    }

    b2Joint_WakeBodies(record->Joint);
}

void Joint2D::SetMaxMotorEffort(float effort)
{
    JointRecord2* record = detail::Resolve(m_State, m_Id, "SetMaxMotorEffort");
    if (record == nullptr || !detail::IsFinite(effort))
    {
        return;
    }

    effort = std::max(0.0f, effort);
    switch (record->Kind)
    {
        case JointKind::Hinge:    b2RevoluteJoint_SetMaxMotorTorque(record->Joint, effort); break;
        case JointKind::Slider:   b2PrismaticJoint_SetMaxMotorForce(record->Joint, effort); break;
        case JointKind::Distance: b2DistanceJoint_SetMaxMotorForce(record->Joint, effort); break;
        case JointKind::Wheel:    b2WheelJoint_SetMaxMotorTorque(record->Joint, effort); break;
        case JointKind::Weld:
        case JointKind::Cone:     return;
    }

    b2Joint_WakeBodies(record->Joint);
}

float Joint2D::GetMotorEffort() const
{
    const JointRecord2* record = detail::Resolve(m_State, m_Id, "GetMotorEffort");
    if (record == nullptr)
    {
        return 0.0f;
    }

    switch (record->Kind)
    {
        case JointKind::Hinge:    return b2RevoluteJoint_GetMotorTorque(record->Joint);
        case JointKind::Slider:   return b2PrismaticJoint_GetMotorForce(record->Joint);
        case JointKind::Distance: return b2DistanceJoint_GetMotorForce(record->Joint);
        case JointKind::Wheel:    return b2WheelJoint_GetMotorTorque(record->Joint);
        case JointKind::Weld:
        case JointKind::Cone:     break;
    }

    return 0.0f;
}

void Joint2D::EnableSpring(bool enabled)
{
    JointRecord2* record = detail::Resolve(m_State, m_Id, "EnableSpring");
    if (record == nullptr)
    {
        return;
    }

    switch (record->Kind)
    {
        case JointKind::Hinge:    b2RevoluteJoint_EnableSpring(record->Joint, enabled); break;
        case JointKind::Slider:   b2PrismaticJoint_EnableSpring(record->Joint, enabled); break;
        case JointKind::Distance: b2DistanceJoint_EnableSpring(record->Joint, enabled); break;
        case JointKind::Wheel:    b2WheelJoint_EnableSpring(record->Joint, enabled); break;
        case JointKind::Weld:
        case JointKind::Cone:
            LogMessage(LogLevel::Warning, "joint",
                       "A weld's springiness is set when it is made, through its hertz values.");
            return;
    }

    b2Joint_WakeBodies(record->Joint);
}

void Joint2D::SetSpring(float hertz, float dampingRatio)
{
    JointRecord2* record = detail::Resolve(m_State, m_Id, "SetSpring");
    if (record == nullptr || !detail::IsFinite(hertz) || !detail::IsFinite(dampingRatio))
    {
        return;
    }

    hertz = std::max(0.0f, hertz);
    dampingRatio = std::max(0.0f, dampingRatio);

    switch (record->Kind)
    {
        case JointKind::Hinge:
            b2RevoluteJoint_SetSpringHertz(record->Joint, hertz);
            b2RevoluteJoint_SetSpringDampingRatio(record->Joint, dampingRatio);
            break;
        case JointKind::Slider:
            b2PrismaticJoint_SetSpringHertz(record->Joint, hertz);
            b2PrismaticJoint_SetSpringDampingRatio(record->Joint, dampingRatio);
            break;
        case JointKind::Distance:
            b2DistanceJoint_SetSpringHertz(record->Joint, hertz);
            b2DistanceJoint_SetSpringDampingRatio(record->Joint, dampingRatio);
            break;
        case JointKind::Wheel:
            b2WheelJoint_SetSpringHertz(record->Joint, hertz);
            b2WheelJoint_SetSpringDampingRatio(record->Joint, dampingRatio);
            break;
        case JointKind::Weld:
            b2WeldJoint_SetLinearHertz(record->Joint, hertz);
            b2WeldJoint_SetAngularHertz(record->Joint, hertz);
            b2WeldJoint_SetLinearDampingRatio(record->Joint, dampingRatio);
            b2WeldJoint_SetAngularDampingRatio(record->Joint, dampingRatio);
            break;
        case JointKind::Cone: return;
    }

    b2Joint_WakeBodies(record->Joint);
}

void Joint2D::SetLength(float length)
{
    JointRecord2* record = detail::Resolve(m_State, m_Id, "SetLength");
    if (record == nullptr || !detail::IsFinite(length))
    {
        return;
    }

    if (record->Kind != JointKind::Distance)
    {
        LogMessage(LogLevel::Warning, "joint", "SetLength is only meaningful on a distance joint.");
        return;
    }

    b2DistanceJoint_SetLength(record->Joint, std::max(0.005f, length));
    b2Joint_WakeBodies(record->Joint);
}

void Joint2D::Destroy()
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
