# Measuring a frame

ludifex can tell you where the time in a frame went and how busy its worker
threads were. Both are off until you ask for them and cost nothing while off.

## Where the time went

```cpp
ludifex::SetProfilingEnabled(true);

world.Update(1.0f / 60.0f);

for (const ludifex::ProfileSection& section : ludifex::GetProfile())
{
    std::printf("%-16s %6.2f ms  %d calls\n",
                section.Name, section.Milliseconds, section.Calls);
}
```

`GetProfile` returns the last complete frame, in the order the sections first
ran, so the list reads from top to bottom like the frame:

| Section | What it covers |
| --- | --- |
| `world update` | All of `Update`, including the sections below it |
| `assets arriving` | Taking up finished background loads |
| `physics` | The solver, including the work spread across worker threads |
| `hierarchy` | Moving children with their parents |
| `events` | Collision and sensor callbacks |
| `animation` | Advancing clips and building poses |
| `sound` | Voices, the listener, and occlusion tests |
| `render` | All of a 3D world's `Render`: culling, gathering what to draw, and `render frame` |
| `render frame` | Recording the frame's GPU commands, for 3D and 2D worlds |
| `light clusters` | Assigning point lights to parts of the view |

Sections nest, so they do not add up to the frame time. Compare a section
with the one containing it rather than with the total.

`Milliseconds` is the total for every call that frame, not an average, and
`Calls` is how many times the section ran. Two calls to `physics` in one
update mean the fixed step ran twice to catch up.

Turning profiling off clears the profile, so `GetProfile` returns an empty
list rather than old numbers:

```cpp
ludifex::SetProfilingEnabled(false);
assert(ludifex::GetProfile().empty());
```

Timing has a small cost of its own, a clock read at each end of each section.
Leave it off in release builds and turn it on when a frame is slow.
`IsProfilingEnabled` tells you the current state.

## How busy the workers were

```cpp
ludifex::GetWorkerUtilisation();      // starts a measuring window

for (int frame = 0; frame < 240; ++frame)
{
    world.StepPhysics(1.0f / 60.0f);
}

const std::vector<float> busy = ludifex::GetWorkerUtilisation();
```

Each entry is the fraction of time, from 0 to 1, that one worker thread spent
running jobs. Reading the values starts a new window, so two calls in a row
describe the time between them. The first call just starts the measurement.

The first entry is the thread that started the scheduler, usually yours.
While it waits for jobs to finish, it runs pending jobs itself instead of
sleeping, so it appears in the list like the other workers. It also does the
frame's single-threaded work, so its number is normally lower than the pool
threads'.

Look at how evenly the work is spread rather than at the exact values. When
the pool threads are all similarly busy, the work is being divided well. When
one is busy and the rest are nearly idle, the work is not dividing. That
usually means the world is one large group of touching bodies that the solver
cannot split, or that `WorkerCount` is 1. The values also depend on whatever
else the machine is doing at the time, so compare runs on the same machine
under the same conditions.

An empty list means the scheduler is not running.

## Allocation while stepping

A world that is not being changed (no actors added or destroyed, no models
loaded, no materials created) makes no heap allocations while it steps
physics. Everything the solver and the scheduler need is allocated when the
world is built and reused afterwards, including the bookkeeping for each job.

You can check this yourself by replacing the global `operator new`, counting
calls, and stepping. The
[benchmark example](https://github.com/cresmarmat-an/ludifex-examples/tree/main/03-benchmark)
does this over two measuring windows in a row. Something that grows once shows
up in the first window only, while something allocated every step shows up in
both.

Changing the world does allocate: adding an actor, loading a model, or
creating a material all need memory.

## Limitations

- The profile measures time on the CPU only. There is no GPU timing, so a
  frame limited by the graphics card looks fast in the profile.
- The sections cover ludifex's own work. Your code is not timed unless you
  time it yourself.
- Most sections come from 3D worlds. For a 2D world, only `render frame` and
  `sound` appear; its physics step is not timed.
- Only the last complete frame is kept. There is no history, averaging, or
  export; collect the values yourself if you need them over time.
- The no-allocation behaviour covers stepping physics. Rendering, sound, and
  animation may allocate when what is on screen or playing changes.
