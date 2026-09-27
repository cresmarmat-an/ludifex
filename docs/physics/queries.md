# Queries

Queries ask a world what lies along a line, under the pointer, along the path
of a moving shape, or inside a shape. They read the world as of the last
physics step.

## Ray casts

A ray cast returns the first actor it hits:

```cpp
ludifex::RayHit hit = world.CastRay(origin, direction, 100.0f);

if (hit.Hit)
{
    hit.Actor;      // what was hit
    hit.Point;      // where
    hit.Normal;     // the surface normal there
    hit.Distance;   // metres from the origin
    hit.Fraction;   // 0 at the origin, 1 at the ray's end
}
```

`direction` does not need to be normalized. Sensors are ignored.

## Picking with the pointer

To find what is under the pointer, cast through the view. The coordinates run
0..1 across the view, with (0, 0) at the top left, which is what an opane
`Viewport` passes to its event callback:

```cpp
ludifex::RayHit hit = world.PickFromView(normalizedX, normalizedY);
if (hit.Hit)
{
    ludifex::Actor3D picked = hit.Actor;
    picked.ApplyImpulse({ 0.0f, 5.0f, 0.0f });
}
```

`PickFromView` uses the camera and the view's proportions, so the world must
have been given a render size (drawing it does that).

## Shape casts

A ray tells you whether anything is on a line. A shape cast tells you whether
something of a given size can move along a path:

```cpp
ludifex::RayHit hit = world.CastSphere(from, 0.4f, direction, 20.0f);
world.CastBox(from, { 1.0f, 1.0f, 1.0f }, direction, 20.0f);   // full size, axis-aligned
world.CastCapsule(from, 0.35f, 1.8f, direction, 20.0f);        // standing along Y
```

The result is a `RayHit`: what was hit, where, the surface normal, and how far
the shape travelled first. Sensors are ignored.

## Overlaps

An overlap finds every actor a shape touches where it stands now:

```cpp
for (ludifex::Actor3D& actor : world.OverlapSphere(blast, 6.0f))
{
    actor.ApplyImpulse(/* ... */);
}

world.OverlapBox(doorway, { 1.2f, 2.2f, 0.4f });
world.OverlapCapsule(spawnPoint, 0.35f, 1.8f);
```

Each actor is reported once, however many shapes it has. Sensors are left out
unless you pass `true` as the last argument, so `OverlapSphere(point, radius,
true)` finds the trigger volumes a point is inside.

2D worlds have the same queries plus `OverlapPoint`; see
[2D worlds](../worlds/2d-worlds.md#shape-queries-in-2d).

## Limitations

- Casts return only the nearest hit. There is no call that returns every hit
  along a ray.
- Box casts and box overlaps are axis-aligned, and capsules stand along Y; a
  rotated query shape is not supported.
- Queries cannot be restricted to certain collision layers; filter the results
  yourself.
- Queries see the physics state at the last step, not the interpolated
  position drawn on screen.
