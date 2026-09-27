# The stability contract

These are the rules ludifex follows to keep a simulation stable and
predictable.

## Fixed steps with a limit

Physics runs in fixed steps. `Update` runs at most `MaxStepsPerFrame` of them,
so a slow frame cannot ask for more steps and make the next frame slower
still. See [Stepping the simulation](stepping.md).

## Sub-steps

Each step is solved in `SubStepCount` sub-steps; four at 60 Hz means
constraints are solved 240 times a second. When stacks jitter or joints
stretch, raise `SubStepCount` first.

## Validation

Bad values are rejected where they enter: a position, rotation, or velocity
that is not a finite number, or a shape smaller than the minimum size, is
refused with a message naming the actor. Invalid data never reaches the
solver, where it would make the whole scene disappear. See
[Diagnostics and validation](../tools/diagnostics-and-validation.md).

## Sleeping and fast bodies

Bodies at rest go to sleep and cost nothing until something wakes them.
Continuous collision stops fast bodies passing through thin static ones; set
`IsBullet` on small, fast dynamic bodies so they do not pass through other
dynamic bodies either.

## Determinism

```cpp
ludifex::World3D world = ludifex::CreateWorld3D({ .Deterministic = true });
```

A deterministic world uses one thread and a fixed step, so the same inputs
give bit-identical results from run to run. That is what replays, lockstep
networking, and reproducible bug reports need. The
[checks example](https://github.com/cresmarmat-an/ludifex-examples/tree/main/01-checks)
verifies this for 2D and 3D worlds.

## Threads

Both physics engines split their work into tasks. Asking for more than one
worker starts a thread pool shared by every world, so several worlds never
compete for the CPU with separate pools:

```cpp
ludifex::World3D world = ludifex::CreateWorld3D({ .WorkerCount = 4 });
ludifex::World3D automatic = ludifex::CreateWorld3D({ .WorkerCount = 0 });   // one per core

world.GetWorkerCount();   // the number actually used
```

`WorkerCount` includes the calling thread, so 4 starts three more. A
deterministic world always uses one. More threads change the order in which
work is done, not the physical result: a pile of bodies settles the same way on
one thread or four. See [Measuring a frame](../tools/measuring-a-frame.md) to
check how busy the workers are.

## Limitations

- Determinism holds for the same build of your program on the same machine. It
  has not been verified across different CPUs, compilers, or operating
  systems.
- A deterministic world is single-threaded.
