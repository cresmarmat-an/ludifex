# Working with actors

An `Actor3D` is a small handle that can be copied and stored freely. It
refers to an actor by index and generation, so a handle kept after the actor
is destroyed does not dangle: `IsValid()` returns false, and calls through it
do nothing and log a message.

```cpp
actor.IsValid();
actor.GetId();
actor.GetName();   actor.SetName("Player");
```

Two handles compare equal when they refer to the same actor, which is useful
in collision handlers.

## Position and motion

```cpp
actor.GetPosition();         actor.SetPosition({ 0.0f, 5.0f, 0.0f });
actor.GetRotation();         actor.SetRotation(rotation);
actor.GetScale();

actor.GetLinearVelocity();   actor.SetLinearVelocity({ 3.0f, 0.0f, 0.0f });
actor.GetAngularVelocity();  actor.SetAngularVelocity({ 0.0f, 1.0f, 0.0f });   // radians per second

actor.ApplyForce({ 0.0f, 100.0f, 0.0f });    // newtons, over the next step
actor.ApplyImpulse({ 0.0f, 5.0f, 0.0f });    // an instant change in momentum
actor.ApplyTorque({ 0.0f, 2.0f, 0.0f });

actor.GetMass();
actor.IsAwake();             actor.SetAwake(true);
```

`SetPosition` and `SetRotation` teleport the actor: the rendered position
moves at once instead of sliding from the old place. Forces and impulses are
applied at the centre of mass.

For drawing, `GetInterpolatedTransform()` returns the transform blended
between the last two physics steps, which is what the renderer uses.

## Appearance

Colour, textures, materials, and visibility are covered in
[Surfaces and textures](../rendering/surfaces-and-textures.md) and
[Custom shaders](../rendering/custom-shaders.md). They affect drawing only,
never the simulation.

## Changing the body type and destroying

```cpp
actor.SetBodyType(ludifex::BodyType::Kinematic);   // applied at the next sync point
actor.Destroy();                                    // applied at the next sync point
```

Destroying an actor also destroys everything
[parented](transform-hierarchy.md) to it and every joint attached to it. See
[The mutability contract](mutability-contract.md) for when changes apply.

## Finding actors

```cpp
world.ForEachActor([&](ludifex::Actor3D& actor) {
    // creating and destroying actors in here is safe
});

world.GetActorCount();
world.GetStepCount();
ludifex::Actor3D found = world.GetActor(someId);
```

## Limitations

- There is no linear or angular damping, gravity scale, or mass override on
  actors, and no way to lock individual axes of motion in 3D. Use
  [native access](../tools/native-access.md) to the Box3D body for these.
- Forces are applied at the centre of mass only; there is no call to push at a
  point.
- There is no lookup by name. Keep the handles `Add` returns, or search with
  `ForEachActor`.
