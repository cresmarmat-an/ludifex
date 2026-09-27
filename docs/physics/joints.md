# Joints

A joint connects two actors and restricts how they can move relative to each
other. Every joint is described in world space: place the two bodies where you
want them, give the joint's anchor and axis in the same coordinates, and the
joint records its local frames for you. Nothing jumps when a joint is created.

```cpp
ludifex::Actor3D hull  = world.AddBox({ .Position = { 0.0f, 4.0f, 0.0f } });
ludifex::Actor3D wheel = world.AddSphere({ .Radius = 0.5f, .Position = { 1.0f, 3.0f, 0.0f } });

ludifex::Joint3D axle = world.AddHinge({
    .BodyA = hull,
    .BodyB = wheel,
    .Anchor = { 1.0f, 3.0f, 0.0f },
    .Axis = { 0.0f, 0.0f, 1.0f },
    .EnableMotor = true,
    .MotorSpeed = 8.0f,          // radians per second
    .MaxMotorTorque = 400.0f,
});
```

| Kind | Created with | Allows |
|---|---|---|
| Hinge | `AddHinge(HingeDesc)` | Rotation about `Axis` only. |
| Slider | `AddSlider(SliderDesc)` | Movement along `Axis` only. |
| Distance | `AddDistanceJoint(DistanceJointDesc)` | Everything except changing the distance between two points. |
| Weld | `AddWeld(WeldDesc)` | Nothing, unless made springy. |
| Cone | `AddConeJoint(ConeJointDesc)` | Swinging within a cone about `Axis`, and twisting about it within limits. |

2D worlds have the same kinds (except the cone) plus a wheel joint; see
[2D worlds](../worlds/2d-worlds.md#joints-in-2d).

Connected bodies do not collide with each other. Set `CollideConnected = true`
when they should.

## Limits and motors

Hinges and sliders each have an optional limit and an optional motor, and the
two work together: a motorised hinge within a limit makes a door closer, and a
motorised slider within a limit makes a piston.

```cpp
ludifex::Joint3D lid = world.AddHinge({
    .BodyA = chest, .BodyB = door,
    .Anchor = hingeLine, .Axis = { 0.0f, 0.0f, 1.0f },
    .EnableLimit = true,
    .LowerAngle = 0.0f,
    .UpperAngle = 1.4f,          // about 80 degrees
});

lid.SetMotorSpeed(-1.0f);        // swing it shut
lid.SetMaxMotorEffort(60.0f);    // torque for a hinge, force for a slider
lid.EnableMotor(true);
```

The limit range should include zero, because zero is the pose the bodies were
in when the joint was created.

## Springs

Hinges, sliders, and cone joints can have a spring that pulls the second body
back toward the pose it had when the joint was created. `EnableSpring`,
`Hertz` (stiffness), and `DampingRatio` set it at creation;
`EnableSpring(bool)` and `SetSpring(hertz, dampingRatio)` change it later.

A distance joint is rigid by default and becomes a spring with `EnableSpring`.
A rope (slack when loose, firm when pulled tight) is a spring of zero hertz,
which leaves it slack, combined with a limit from zero to the rope's length:

```cpp
world.AddDistanceJoint({
    .BodyA = hook, .BodyB = lamp,
    .AnchorA = hookPoint, .AnchorB = lampPoint,
    .EnableSpring = true, .Hertz = 0.0f,
    .EnableLimit = true, .MinLength = 0.0f, .MaxLength = 2.0f,
});
```

A `Length` below zero (the default) keeps the distance the anchors are apart
when the joint is created. `SetLength` changes it later.

A weld is rigid unless you give it a `LinearHertz` or `AngularHertz`, which
makes it flex. `SetSpring` on a weld sets both.

Changing a limit, motor, or spring wakes the bodies the joint holds, so the
change takes effect even if they were at rest.

## Reading a joint

```cpp
const float angle  = axle.GetAngle();          // hinge: radians
const float extent = piston.GetTranslation();  // slider: metres; distance joint: its length
const float effort = axle.GetMotorEffort();    // what the motor applied in the last step
axle.GetKind();
```

To make a joint that breaks under load, check `GetMotorEffort` each frame and
destroy the joint when it is too high.

## Destroying

```cpp
axle.Destroy();          // applied at the next sync point
```

Destroying either connected actor destroys the joint too, so a `Joint3D`
handle can become stale on its own:

```cpp
wheel.Destroy();
world.ApplyPendingChanges();

axle.IsValid();          // false: the joint went with the wheel
world.GetJointCount();
```

Like destroying an actor, destroying a joint is queued, so it is safe inside a
collision or trigger handler.

## Cone joints

A cone joint is a ball joint with limits: the second body swings freely within
a cone about `Axis` and twists about that axis within a range. It suits
shoulders, hips, necks, and chains.

```cpp
ludifex::Joint3D shoulder = world.AddConeJoint({
    .BodyA = chest,
    .BodyB = upperArm,
    .Anchor = shoulderPoint,
    .Axis = { 0.0f, -1.0f, 0.0f },   // the middle of the cone, from A toward B
    .ConeAngle = 1.2f,               // radians of swing allowed, up to a quarter turn
    .LowerTwist = -0.6f,             // radians of twist allowed about the axis
    .UpperTwist = 0.6f,
});

shoulder.GetAngle();               // how far the arm has swung from the axis
shoulder.GetTwistAngle();          // how far it has turned about the axis
shoulder.SetConeAngle(0.4f);
shoulder.SetLimits(-0.2f, 0.2f);   // on a cone joint, the twist limits
shoulder.EnableLimit(false);       // turns off the cone and twist limits together
```

Its motor drives the second body's angular velocity relative to the first,
about each world axis, with at most the given torque. With zero velocity and a
little torque, it acts as joint friction:

```cpp
shoulder.SetMotorVelocity({ 0.0f, 1.5f, 0.0f });
shoulder.SetMaxMotorEffort(40.0f);
shoulder.EnableMotor(true);
```

ludifex builds Box3D with a fix to its cone limit; see
[Installation](../getting-started/installation.md#what-gets-fetched). Without
it, a long body resting against a narrow cone slowly gains energy and leaves
the cone.

## Ragdolls

`AddRagdoll` builds a whole figure of eleven parts (pelvis, chest, head, and
two parts per limb), held by cone joints at the neck, spine, shoulders, and
hips and by hinges at the elbows and knees, each with human-like limits. It is
built standing, arms at its sides, facing +Z.

```cpp
ludifex::Ragdoll guard = world.AddRagdoll({
    .Position = { 0.0f, 0.0f, 0.0f },   // where the feet stand
    .Height = 1.8f,
    .Density = 1.0f,
    .Stiff = false,                     // true adds springs so it holds its pose a little
    .Tint = ludifex::Color::FromBytes(210, 160, 80),
    .Name = "Guard",                    // parts are "Guard.Head", "Guard.Chest", ...
});

guard.Chest.SetLinearVelocity({ 0.0f, 0.0f, -3.0f });   // knocked backwards
for (ludifex::Actor3D& part : guard.Parts())
{
    part.SetColor(clothColor);
}
```

Every part and joint is a named member (`guard.Head`, `guard.KneeLeft`), and
each is an ordinary actor or joint. You can push the part that was hit, or
remove a limb by destroying its joint.

## Limitations

- These are the only joint kinds. There are no gear, pulley, or prismatic-plus-
  rotation joints in 3D, and no motor-only joint.
- Joints do not break by themselves; watch `GetMotorEffort` and destroy them
  yourself.
- A cone joint's swing is limited to a quarter turn, and a 2D hinge's limit to
  a little under half a turn either way.
- Ragdolls are built in one fixed humanoid layout. There is no way to build a
  ragdoll from a model's skeleton, and a ragdoll does not drive a skinned
  model.
- Joints are not included when a world is
  [saved](../worlds/saving-and-restoring.md).
