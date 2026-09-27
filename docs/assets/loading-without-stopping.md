# Loading without stopping

Reading and parsing a model file takes time, and loading many of them in one
frame freezes the window until they are done. To avoid that, load them in the
background:

```cpp
world.AddModel({
    .Path = "props/crate.gltf",
    .LoadInBackground = true,
    .Position = { 4.0f, 1.0f, 0.0f },
});
```

The call returns at once and the actor is in the world immediately, drawn as a
plain box at its scale. When the file has been read, the actor takes the
model's shape. You do not have to poll or wait for anything; the world fills
in over the next few frames.

```cpp
world.GetPendingLoadCount();   // models still being read; 0 when all have arrived
world.WaitForLoads();          // blocks until they have, for a loading screen
```

## How the work is split

The file is read and parsed on a background thread. Everything that touches
the world or the GPU happens on the thread that calls `Update`, when that call
collects the finished loads. New meshes are uploaded to the GPU at most two per
frame, so many models arriving together do not cause a spike of their own.

The actor is an ordinary actor the whole time. You can move it, parent it,
change its colour, or destroy it while its model is still loading, and those
changes carry over when the model arrives. If the file cannot be read, the
actor stays a box and the error goes to the log.

A model that fails to load in the foreground also still appears: as a
one-metre cube with the missing-texture checkerboard, named `missing:`
followed by the path, so the gap is easy to spot.

## When to load in the foreground

The collider changes when the model arrives: the placeholder box is replaced
by a box fitted to the model's bounds, and the body's mass is recomputed from
it. An actor that must have its final collider from the first frame, such as
the floor of a level, should be loaded in the foreground, or you should call
`WaitForLoads()` before the simulation starts.

## Limitations

- There is one loading thread. Models load one after another, not in
  parallel.
- Only models load in the background. `LoadTexture`, `LoadSound`, and
  `CreateMaterial` run on the calling thread.
- Loading cannot be cancelled. Destroying the actor does not stop its file
  from being read.
- There is no progress value for a single file, only the number of models
  still pending.
- While a model loads, its actor is a plain box. There is no way to choose a
  different placeholder.
