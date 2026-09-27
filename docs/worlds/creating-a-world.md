# Creating a world

One call creates a 3D world with every setting at its default:

```cpp
ludifex::World3D world = ludifex::CreateWorld3D();
```

Every setting can be changed when the world is created:

```cpp
ludifex::World3D world = ludifex::CreateWorld3D({
    .Gravity = { 0.0f, -10.0f, 0.0f },
    .FixedTimeStep = 1.0f / 60.0f,   // physics runs in steps of this length
    .SubStepCount = 4,               // solver passes per step
    .MaxStepsPerFrame = 8,           // the most steps one Update may run
    .PhysicsEnabled = true,          // false: no physics until StartPhysics
    .EnableSleep = true,             // bodies at rest stop being simulated
    .EnableContinuous = true,        // fast bodies do not pass through thin ones
    .Deterministic = false,          // the same inputs give the same results
    .WorkerCount = 1,                // threads that step this world; 0 is one per core
});
if (!world.IsValid())
{
    // the reason is in the log
}
```

`FixedTimeStep`, `SubStepCount`, `MaxStepsPerFrame`, `Deterministic`, and
`WorkerCount` are explained in [Stepping the simulation](stepping.md) and
[The stability contract](stability-contract.md).

A 2D world works the same way with `CreateWorld2D`; see
[2D worlds](2d-worlds.md).

## Changing settings later

```cpp
world.SetGravity({ 0.0f, -3.0f, 0.0f });
world.SetFixedTimeStep(1.0f / 120.0f);
world.SetSubStepCount(8);

world.GetGravity();
world.GetFixedTimeStep();
world.GetSubStepCount();
world.GetWorkerCount();
```

## Owning a world

A world owns everything in it: actors, joints, lights, voices, and its GPU
resources. `World3D` can be moved but not copied, and destroying it destroys
everything it owns. Several worlds can exist at the same time, each with its
own camera and settings. Textures, materials, and sounds are shared by all
worlds in the process.

A world needs no window or GPU to simulate. It creates rendering resources
only when it is first drawn; see [Rendering a world](../rendering/overview.md).

## Limitations

- A world is not thread-safe. Call its functions from one thread at a time,
  normally the main thread. The physics step itself can use several threads.
- Gravity is the same for every actor in a world. There is no per-actor
  gravity scale.
