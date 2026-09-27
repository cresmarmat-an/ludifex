# Diagnostics and validation

ludifex checks the values it is given and reports problems through a log.
By default, messages at `Info` and above are written to the console:
warnings and errors to standard error, the rest to standard output, as
`[ludifex:category] level: message`. You can send them somewhere else:

```cpp
ludifex::SetLogHandler([](ludifex::LogLevel level, const char* category, const char* message) {
    // send to your own console or log file
});

ludifex::SetMinimumLogLevel(ludifex::LogLevel::Warning);
```

The levels are `Trace`, `Info`, `Warning`, and `Error`. You can write to the
same log with `ludifex::LogMessage(level, category, format, ...)`, which takes
`printf`-style arguments.

## Categories

| Category | Messages about |
| --- | --- |
| `world` | Creating worlds, settings, stepping, saving and loading |
| `actor` | Creating actors, and calls on destroyed or empty handles |
| `physics` | Shape sizes and solver input |
| `joint` | Creating and changing joints |
| `query` | Ray casts and shape queries |
| `light` | Lights and their limits |
| `render` | Rendering setup, quality, and effects |
| `gpu` | The graphics device |
| `texture` | Loading and creating textures |
| `material` | Compiling and using custom shaders |
| `asset` | Finding, loading, and reloading files |
| `audio` | The audio device, sounds, and voices |
| `jobs` | The worker threads |

Messages name the object or file involved and, where there is one, suggest a
fix.

## What is checked

| Situation | What happens |
| --- | --- |
| A position, rotation, or velocity that is not a finite number | The call is ignored, and an error names the actor |
| A force or impulse that is not a finite number | The call is ignored |
| A shape smaller than 5 mm | The shape is rejected, with an error giving the minimum |
| A shape larger than 2 km | The shape is created, with a warning about precision |
| A call on a destroyed actor | Nothing happens, and a warning says which call was made |
| A gravity vector that is not finite | The call is ignored and gravity is unchanged |
| A fixed time step that is not a positive number | At creation, 1/60 s is used instead; `SetFixedTimeStep` ignores it |
| A file that cannot be found | The message lists every place it was looked for |
| A model that cannot be read | A placeholder cube with a checkerboard texture stands in its place |
| A shader that fails to compile | The compiler's message is logged, with file, line, and column |

The limits on shape size exist because the physics solvers work in single
precision and are tuned for objects measured in metres. Very small or very
large shapes make contacts unreliable. If your game needs a different scale,
scale the whole world instead.

## Limitations

- The handler is called on whichever thread wrote the message, which is
  usually yours but can be the loading thread or a worker thread. Make the
  handler safe to call from any thread.
- There is one handler and one minimum level for the whole process, not one
  per world or category.
- Messages are plain text, cut off after about a thousand characters. There
  are no error codes to test for in code; check return values and
  `IsValid()` instead.
- Validation catches values that would break the simulation. It does not
  catch values that are legal but unwise, such as a very heavy body resting
  on a very light one.
