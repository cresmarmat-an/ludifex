# Stepping the simulation

Physics advances in fixed steps, `FixedTimeStep` seconds each (a sixtieth of a
second by default), whatever the frame rate. Rendering blends between the last
two steps, so motion looks smooth even when the frame rate and the step rate
differ.

## Automatic stepping

This is the default. Pass `Update` the time since the last frame, and it runs
as many fixed steps as that time allows:

```cpp
world.Update(deltaSeconds);
```

Leftover time carries over to the next frame. `Update` runs at most
`MaxStepsPerFrame` steps, so one slow frame cannot ask for so many steps that
the next frame is slower still. `Update` also picks up models that finished
loading in the background, reloads changed files when
[hot reload](../assets/reloading-while-it-runs.md) is on, and advances
animation and sound by the frame time.

## Pausing

```cpp
world.StopPhysics();
world.StartPhysics();
world.IsPhysicsRunning();
```

While physics is stopped, rendering, animation, and sound keep running, and
the solver costs nothing. Time that passes while paused is discarded, so the
world does not jump when it resumes.

## Stepping by hand

```cpp
world.SetPhysicsMode(ludifex::PhysicsMode::Manual);
world.StepPhysics(1.0f / 60.0f);   // exactly one step
```

In manual mode, `Update` does not step physics but still applies queued
changes, updates animation and sound, and can be called every frame as usual.
If you step on your own schedule, tell the renderer how far the frame is
between steps so motion stays smooth:

```cpp
world.SetInterpolationAlpha(accumulator / step);   // 0 at the last step, 1 at the next
world.GetInterpolationAlpha();
```

## Sync points

Changes that alter the world's structure (creating and destroying actors,
changing body types, parenting) are queued and applied at sync points: before
and after each physics step, and when you call `ApplyPendingChanges()`. See
[The mutability contract](mutability-contract.md).

## Limitations

- The step length is fixed. There is no variable time step mode.
- `Update` does not interpolate or extrapolate physics state for queries: a
  ray cast sees the world as of the last step, not where it is drawn.
