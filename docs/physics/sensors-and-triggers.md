# Sensors and triggers

A sensor is an actor that detects overlaps but never pushes anything. Use one
for a checkpoint, a pickup radius, a damage zone, or a test such as "the player
is near the door".

```cpp
ludifex::Actor3D checkpoint = world.AddBox({
    .Scale = { 4.0f, 3.0f, 4.0f },
    .Position = { 12.0f, 1.5f, 0.0f },
    .Type = ludifex::BodyType::Static,
    .IsSensor = true,
});

checkpoint.WhenEntered([&](const ludifex::TriggerInfo& info) {
    if (info.Other == player)
    {
        Save(player.GetPosition());
    }
});

checkpoint.WhenExited([&](const ludifex::TriggerInfo& info) { /* ... */ });

checkpoint.IsSensor();   // true
```

`IsSensor` can be set on boxes, spheres, and capsules (and in 2D, on
rectangles, circles, capsules, and sprites).

`TriggerInfo` has two actors:

| Field | Meaning |
|---|---|
| `Sensor` | The actor created with `IsSensor`. |
| `Other` | What entered or left it. |

Handles compare equal when they refer to the same actor, so
`info.Other == player` tells you who set off the trigger.

You can also listen for every sensor in the world. For each overlap, the
sensor's own handler runs first, then the world's:

```cpp
world.WhenActorEnteredTrigger([&](const ludifex::TriggerInfo& info) { /* ... */ });
world.WhenActorLeftTrigger([&](const ludifex::TriggerInfo& info) { /* ... */ });
```

## What a sensor notices

A sensor notices static, kinematic, and dynamic actors, including other
sensors, but never itself. Nothing has to be marked in advance to be
detected. To make a sensor notice only some actors, give it a collision
filter:

```cpp
checkpoint.SetCollisionFilter(Trigger, Player);   // only the player sets it off
```

A sensor that is already overlapping something when it is created reports it
as entering.

## Visibility and movement

Sensors start hidden, since trigger volumes are usually not meant to be seen.
`SetVisible(true)` shows one, which helps while tuning.

A sensor still has a body with a `BodyType`. `Static` is usual for a fixed
area; a `Kinematic` sensor is one you move yourself, such as a moving danger
zone; a `Dynamic` sensor falls, which is rarely wanted.

## When events arrive

Trigger events are delivered after the physics step, like collision events, so
a handler can create and destroy actors. If the other actor was destroyed
before the step ended, its leaving is not reported; you never receive a handle
to an actor that no longer exists.

## Limitations

- Only enter and leave are reported. There is no event while something stays
  inside; keep your own list from the enter and leave events, or use an
  [overlap query](queries.md).
- Models cannot be sensors. Use a primitive shape around the model instead.
- One handler for entering and one for leaving per sensor; a new handler
  replaces the old one.
