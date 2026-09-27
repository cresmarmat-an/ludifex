# Collision events

A collision event tells you that two actors hit each other, where, and how
hard. Events are delivered after the physics step has finished, never while
the solver is running, so a handler can create and destroy actors freely.

```cpp
ludifex::Actor3D ball = world.AddSphere({ .Position = { 0.0f, 10.0f, 0.0f } });

ball.WhenCollided([&](const ludifex::CollisionInfo& info) {
    // info.Self is the ball; info.Other is what it hit.
    const float strength = std::clamp(info.ImpactSpeed / 12.0f, 0.1f, 1.0f);
    world.PlaySoundAt(impact, info.Point, { .Volume = strength });
});
```

Or once for the whole world, after each actor's own handler:

```cpp
world.WhenActorCollided([&](const ludifex::CollisionInfo& info) { /* ... */ });
```

`CollisionInfo` has:

| Field | Meaning |
|---|---|
| `Self`, `Other` | The two actors, from `Self`'s point of view. |
| `Point` | The contact point, in world space. |
| `Normal` | The contact normal, pointing from `Other` toward `Self`. |
| `ImpactSpeed` | How fast the two were moving toward each other, in metres per second. |

Use `ImpactSpeed` to scale a sound or an effect, so a gentle touch and a hard
crash do not look or sound the same.

Contacts that are too gentle are not reported, so objects resting on each
other do not fire events constantly:

```cpp
world.SetCollisionThreshold(1.5f);   // metres per second; the default is 1
```

Each actor has one handler; setting a new one replaces the old, and
`WhenCollided({})` removes it. A handler is released when its actor is
destroyed, along with anything its lambda captured.

## Filtering

Every actor belongs to one or more collision layers and has a mask of layers it
collides with. Two actors collide only when each one's layers are in the other
one's mask, so filtering is always symmetric.

```cpp
constexpr uint64_t Player  = 1ull << 0;
constexpr uint64_t Enemy   = 1ull << 1;
constexpr uint64_t Pickup  = 1ull << 2;
constexpr uint64_t Scenery = 1ull << 3;

player.SetCollisionFilter(Player, ~Pickup);              // walks through pickups
coin.SetCollisionFilter(Pickup, Scenery);                // falls onto scenery, ignores actors
bullet.SetCollisionFilter(Enemy, Enemy | Scenery);       // never hits the player who fired it
```

The filter applies to all of an actor's shapes and takes effect from the next
step. Filters also decide what a [sensor](sensors-and-triggers.md) notices.

## Limitations

- Only the start of a contact is reported. There are no events for contacts
  that continue or end; use [sensors](sensors-and-triggers.md) to know when
  something leaves an area.
- Each event reports one contact point, even when two shapes touch along an
  edge or a face.
- One handler per actor. To notify several parts of your program, call them
  from that handler.
- There are 64 collision layers, one per bit of a `uint64_t`.
