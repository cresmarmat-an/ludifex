# 2D worlds

`World2D` works like `World3D`, using Box2D v3. Positions are `Vec2`, and a
rotation is a single angle in radians, counter-clockwise.

```cpp
ludifex::World2D world = ludifex::CreateWorld2D({ .Gravity = { 0.0f, -10.0f } });

world.AddGround({ .Width = 40.0f });

ludifex::Actor2D player = world.AddRectangle({
    .Width = 1.0f,
    .Height = 2.0f,
    .Position = { 0.0f, 4.0f },
});

ludifex::Actor2D ball = world.AddCircle({
    .Radius = 0.25f,
    .Position = { 1.0f, 6.0f },
    .Restitution = 0.5f,
});

ludifex::Actor2D pill = world.AddCapsule({
    .Radius = 0.2f,
    .Height = 1.0f,          // total, including both rounded ends; along its own Y
    .Position = { -1.0f, 5.0f },
});

world.Update(deltaSeconds);

const ludifex::Transform2 transform = ball.GetInterpolatedTransform();
```

`World2DConfig` has the same fields as `World3DConfig`; see
[Creating a world](creating-a-world.md). Stepping, pausing, manual mode, and
queued changes also work the same; see
[Stepping the simulation](stepping.md).

## Actors in 2D

`Actor2D` has the calls `Actor3D` has, in the plane: `GetPosition`,
`SetPosition`, `GetRotation`, `SetRotation`, velocities, `ApplyForce`,
`ApplyImpulse`, `GetMass`, sleep state, colour, visibility, textures,
materials, uniforms, collision filters, events, `SetBodyType`, and `Destroy`.
Two extras:

- `SetFixedRotation(true)` keeps a body upright however it is pushed.
- `SetLayer`, `SetFrame`, and `SetFlipX` control how sprites are drawn (below).

## Drawing a 2D world

A 2D world uses the same renderer as a 3D one, with an orthographic camera
looking down the Z axis. Shapes are given a little depth so the light still
shades them.

```cpp
ludifex::Camera2D& camera = world.GetCamera();
camera.Center = { 0.0f, 3.2f };
camera.Height = 11.0f;   // world units visible vertically

world.Run();             // a window of its own, or opane's; see Rendering a world
```

`Height` is in world units, so a wider window shows more of the world instead
of stretching it. The light comes from in front of the plane, above and to one
side. Textures, backgrounds, anti-aliasing, post-processing, custom shaders,
and debug lines work as described under [Rendering](../rendering/overview.md).
`SetGraphicsQuality` sets the render scale, anti-aliasing, and bloom.

## Sprites

A sprite is an image in the world with a body behind it:

```cpp
ludifex::Actor2D player = world.AddSprite({
    .Path = "runner.png",
    .Position = { 0.0f, 1.0f },
    .Size = { 1.0f, 1.0f },                          // zero: sized from the image
    .PixelsPerUnit = 100.0f,
    .Collider = ludifex::SpriteCollider::Circle,     // Box (default), Circle, or None
    .FixedRotation = true,
    .Layer = 0,
    .Name = "player",
});

ludifex::Actor2D quick = world.AddSprite("crate.png");   // a dynamic box at the origin
```

Sprites are drawn unlit, so the image appears as authored, and blended, so
soft edges work. When `Size` is zero it comes from the image at
`PixelsPerUnit`; when only one axis is given, the other keeps the image's
proportions.

**Layers.** Sprites are drawn in layer order, and in the order they were added
within a layer. Layers below zero are drawn behind the world's shapes; zero
and above, in front.

```cpp
// Scenery: static, with no collider.
world.AddSprite({ .Path = "hills.png", .Type = ludifex::BodyType::Static,
                  .Collider = ludifex::SpriteCollider::None, .Layer = -1 });
world.AddSprite({ .Path = "grass.png", .Type = ludifex::BodyType::Static,
                  .Collider = ludifex::SpriteCollider::None, .Layer = 1 });
player.SetLayer(0);
```

**Animation.** A sprite sheet is one image with frames in a grid. Choose a
frame by number, counting left to right and top to bottom from zero:

```cpp
player.SetFrame(frame, 4, 1);          // four columns, one row
player.SetFlipX(velocity.X < 0.0f);    // face the way it is moving
```

Frame numbers past the end wrap around, so a running counter can drive an
animation. `SetTextureTiling(repeat, offset)` is the general form and works on
any 2D actor with a texture.

A sprite created as a sensor stays visible and triggers like any other sensor,
which suits pickups:

```cpp
coin.WhenEntered([&, coin](const ludifex::TriggerInfo2D& info) mutable {
    if (info.Other == player)
    {
        ++score;
        coin.Destroy();   // queued until the next sync point, so safe here
    }
});
```

## Backgrounds

```cpp
world.SetBackground("sky.png");
```

The image fills the view behind everything, cropped to keep its proportions,
and does not move with the camera.

## Converting between the view and the world

View coordinates run 0..1 across the rendered image, with (0, 0) at the top
left. Two conversions use the camera and the render size:

```cpp
// A pointer position to a world position, to spawn something where you clicked.
const ludifex::Vec2 there = world.ViewToWorld(pointer.X / windowWidth, pointer.Y / windowHeight);

// A world position to the screen, to draw a label over an actor.
const ludifex::Vec2 view = world.WorldToView(player.GetPosition());
drawList.DrawText("Player", { view.X * windowWidth, view.Y * windowHeight - 40.0f }, font, white);
```

The world must have been given a render size first (drawing it does that).
Without one, a square view is assumed and a warning is logged.

## Picking and ray casts

```cpp
const ludifex::RayHit2D pick = world.PickFromView(pointer.X / windowWidth, pointer.Y / windowHeight);
if (pick.Hit)
{
    // pick.Actor is what is under the pointer; pick.Point is where.
}

const ludifex::RayHit2D below = world.CastRay(feet, { 0.0f, -1.0f }, 0.2f);
const bool grounded = below.Hit;
```

`PickFromView` tests the exact shape under the point, so clicking just outside
a rotated box picks nothing even if the box's bounding rectangle covers the
point. `CastRay` returns the nearest hit with its point, normal, and distance.
Both skip sensors.

## Events, triggers, and filtering

These work as in a 3D world, with 2D information:

```cpp
ball.WhenCollided([&](const ludifex::CollisionInfo2D& info) {
    PlayBounce(info.ImpactSpeed);
});

ludifex::Actor2D goal = world.AddRectangle({
    .Width = 2.0f,
    .Height = 3.0f,
    .Position = { 18.0f, 1.5f },
    .Type = ludifex::BodyType::Static,
    .IsSensor = true,
});

goal.WhenEntered([&](const ludifex::TriggerInfo2D& info) {
    if (info.Other == ball)
    {
        ++score;
    }
});

coin.SetCollisionFilter(Pickup, Player);
```

`CollisionInfo2D` has `Self`, `Other`, `Point`, `Normal`, and `ImpactSpeed`;
`TriggerInfo2D` has `Sensor` and `Other`. The world-wide handlers are
`WhenActorCollided`, `WhenActorEnteredTrigger`, and `WhenActorLeftTrigger`, and
`SetCollisionThreshold` sets how hard an impact must be to be reported (1
metre per second by default). See [Collision events](../physics/collision-events.md)
and [Sensors and triggers](../physics/sensors-and-triggers.md).

A sensor also notices static actors. A sensor whose bottom edge sits exactly
on the ground counts the ground as inside it, so raise it slightly or filter
the ground out.

## Joints in 2D

The joints from [Joints](../physics/joints.md) exist in 2D too, plus a wheel.
Anchors and axes are world-space `Vec2`s, and angles are counter-clockwise
radians.

```cpp
ludifex::Joint2D flipper = world.AddHinge({
    .BodyA = table, .BodyB = paddle,
    .Anchor = pivot,
    .EnableLimit = true, .LowerAngle = -0.5f, .UpperAngle = 0.5f,
    .EnableSpring = true, .Hertz = 6.0f,        // springs back when released
});

world.AddSlider({ .BodyA = shaft, .BodyB = lift, .Anchor = base, .Axis = { 0.0f, 1.0f },
                  .EnableLimit = true, .UpperTranslation = 3.0f,
                  .EnableMotor = true, .MotorSpeed = 1.0f, .MaxMotorForce = 500.0f });

world.AddDistanceJoint({ .BodyA = hook, .BodyB = weight, .AnchorA = top, .AnchorB = bottom });
world.AddWeld({ .BodyA = wall, .BodyB = shelf, .Anchor = bracket });

// A wheel on a suspension: it turns freely and rides a spring along Axis.
ludifex::Joint2D rear = world.AddWheel({
    .BodyA = chassis, .BodyB = wheel,
    .Anchor = wheel.GetPosition(),
    .Axis = { 0.0f, 1.0f },
    .Hertz = 4.0f, .DampingRatio = 0.7f,
    .EnableMotor = true, .MotorSpeed = -12.0f, .MaxMotorTorque = 20.0f,   // negative rolls toward +X
});
```

`Joint2D` has the same calls as `Joint3D`: `GetAngle`, `GetTranslation`,
`EnableLimit`, `SetLimits`, `EnableMotor`, `SetMotorSpeed`,
`SetMaxMotorEffort`, `GetMotorEffort`, `EnableSpring`, `SetSpring`,
`SetLength`, and `Destroy`. Destroying either actor destroys the joint, and
`world.GetJointCount()` counts the joints that exist. A 2D hinge limit can be
at most a little under half a turn either way.

## Shape queries in 2D

A shape cast finds whether something of a given size can move somewhere, and
an overlap finds what is already in a place. Rectangles are axis-aligned;
capsules stand along Y.

```cpp
const ludifex::RayHit2D landing = world.CastCircle(position, 0.4f, { 0.0f, -1.0f }, 10.0f);
world.CastRectangle(position, { 1.0f, 2.0f }, direction, 5.0f);
world.CastCapsule(position, 0.3f, 1.2f, direction, 5.0f);

for (ludifex::Actor2D& actor : world.OverlapCircle(blast, 3.0f))
{
    actor.ApplyImpulse(/* ... */);
}
world.OverlapRectangle(doorway, { 1.0f, 2.0f });
world.OverlapCapsule(spawn, 0.3f, 1.2f);
world.OverlapPoint(pointerInWorld);           // the exact shapes under a point
```

Casts skip sensors; overlaps leave them out unless the last argument asks for
them. Each actor is reported once.

## Characters in 2D

`Character2D` is the [3D character](../physics/characters.md) in the plane: a
capsule you move directly, which slides along what it meets, steps up ledges
no taller than `StepHeight`, and reports what it stands on.

```cpp
ludifex::Character2D hero = world.AddCharacter({
    .Position = { 0.0f, 2.0f },
    .Radius = 0.3f,
    .Height = 1.2f,
    .StepHeight = 0.2f,
    .SlopeLimitDegrees = 55.0f,
});

// Once a frame.
velocity.Y = hero.IsOnGround() ? (jumpPressed ? JumpSpeed : -1.0f) : velocity.Y + Gravity * dt;
const ludifex::Vec2 moved = hero.Move({ runSpeed * dt, velocity.Y * dt });
if (std::abs(moved.Y) < std::abs(velocity.Y * dt) * 0.5f)
{
    velocity.Y = 0.0f;   // landed, or hit a ceiling
}
```

`IsOnGround`, `GetGroundNormal`, `GetPosition`, `SetPosition`, and `GetActor`
work as in 3D. Its capsule is a kinematic actor that does not rotate, so
dynamic bodies land on it and bounce off it, and other characters block it
like walls.

## Sound in 2D

[Sound](../sound/sound.md) works the same way, with `Vec2` in place of `Vec3`:

```cpp
world.PlaySoundAt(clang, ludifex::Vec2{ 12.0f, 3.0f });
world.PlaySoundAt(engine, cart, { .Looping = true });
world.SetListener({ playerX, playerY });
```

The listener sits at the camera's centre and faces into the screen, so a sound
to the right of the view is heard on the right. A 2D emitter's cone points
along its own +X. Everything else on the Sound page applies.

## Limitations

- **Rendering:** no shadows, ambient occlusion, ray tracing, or path tracing
  in 2D, and no point lights. Sprites are unlit and have no normal
  maps.
- **Shapes:** rectangles, circles, capsules, and sprites (as a box or circle).
  No polygons, chains, or edge shapes, and no tile maps.
- **World features:** no transform hierarchy, no saving and restoring, and no
  ragdolls in 2D.
- **Backgrounds** are a single image fixed to the view, with no parallax
  scrolling.
