# The mutability contract

You can change a world from anywhere, including from inside a callback that is
walking through it or handling one of its events. Which changes happen at once
and which are queued follows a fixed rule.

## Immediate changes

These take effect at once and are visible to the next read:

- position and rotation;
- linear and angular velocity;
- forces, impulses, and torque;
- the sleep state;
- names, appearance, collision filters, and local transforms;
- adding actors, joints, characters, and lights.

A new actor takes part in the simulation from the next physics step.

## Queued changes

These are recorded and applied at the next sync point:

- destroying an actor, joint, or character;
- changing a body type;
- parenting and unparenting.

## Sync points

Queued changes are applied before each physics step, after each step, and
whenever you call `world.ApplyPendingChanges()`. They are applied in the order
they were made, which keeps results deterministic.

```cpp
world.ForEachActor([&](ludifex::Actor3D& actor) {
    if (ShouldRemove(actor))
    {
        actor.Destroy();          // queued; the actor stays usable until the sync point
    }
});

world.ApplyPendingChanges();      // now it is gone
```

Collision and trigger events are delivered after the solver finishes, never
while it runs, so handlers can create and destroy actors freely.

## Stale handles

After an actor is destroyed, every handle to it is stale. Using one does
nothing and logs a message; it never crashes or corrupts memory:

```cpp
if (actor.IsValid())
{
    actor.SetPosition(target);
}
```

The same rule applies to joints, lights, characters, textures, materials,
sounds, and voices.

## Limitations

- None of this makes a world safe to use from several threads at once. Call a
  world's functions from one thread.
