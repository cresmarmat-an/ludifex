# Rendering a world

A world draws itself into its own textures, on its own command buffer. It
never draws into another library's render pass, so it can render with or
without an interface library.

## Running a world

The shortest program that shows a world is one call:

```cpp
ludifex::World3D world = ludifex::CreateWorld3D();
world.AddGround();
world.AddBox({ .Position = { 0.0f, 4.0f, 0.0f } });

world.Run();   // returns when the window closes
```

`Run` updates the world, draws it, and shows it every frame. On its own,
ludifex opens a window for this. After `opane::StartApp`, the same call runs
inside opane's window and frame loop, with the world beneath opane's
interface.

## The GPU device

ludifex draws with the process's one GPU device:

- **When a host application has published one** (opane does, in `StartApp`),
  ludifex finds it through SDL's shared properties the first time it needs a
  device, with nothing to call. Before the host destroys its device, it calls
  a function ludifex registered, and ludifex releases every GPU object it
  holds. So the application may shut down before its worlds are destroyed; the
  worlds then stop drawing instead of using a device that is gone.
- **When there is none**, ludifex creates its own device on first use. No
  window is needed: a world renders into an offscreen texture, so ludifex can
  draw in a program with no window, such as a thumbnail tool or a test.
- **To connect explicitly**, for example to another engine's device, pass it
  in before creating a world that renders:

```cpp
ludifex::AdoptHost({ app.GetGpuDevice(), app.GetWindow() });
ludifex::IsRenderingAvailable();
```

`Host` is a plain struct of pointers. ludifex never destroys an adopted
device; it belongs to whoever created it.

The connection uses four SDL global properties (`host.gpu_device`,
`host.window`, `host.interface` for the host's frame loop, and
`host.device_release` for the release function), so any host can offer it, not
only opane.

## Drawing it yourself

`Run` is a shortcut. From your own loop, it takes three calls:

```cpp
world.SetRenderSize(width, height);   // again whenever the view is resized
world.Update(deltaSeconds);
world.Render();

void* texture = world.GetRenderTarget();   // an SDL_GPUTexture*
```

Give that texture to anything that can sample it. With opane,
`app.SetMainWorld(world)` fills the window with it and
`viewport->SetWorld(world)` places it in a layout; see
[Hosting a world](https://cresmarmat-an.github.io/opane/application/hosting-a-world/).
By hand:

```cpp
opane::TextureId view = app.WrapExternalTexture(world.GetRenderTarget(), width, height);
drawList.DrawTexture({ 0, 0, windowSize.X, windowSize.Y }, view);
```

The texture pointer stays the same until the size changes, whatever else is
reconfigured, so wrapping it again after each resize is enough.

## The camera

```cpp
ludifex::Camera3D& camera = world.GetCamera();
camera.Position = { 0.0f, 7.0f, 14.0f };
camera.Target = { 0.0f, 1.5f, 0.0f };
camera.Up = { 0.0f, 1.0f, 0.0f };
camera.FieldOfViewDegrees = 55.0f;   // vertical
camera.NearPlane = 0.1f;
camera.FarPlane = 500.0f;

world.SetCamera(camera);   // or change the reference in place
```

## What happens in a frame

The renderer runs these passes in order, skipping the ones that are off:

1. **Shadow maps**, one per cascade.
2. **Prepass** of depth, motion, and normals, when temporal anti-aliasing,
   ambient occlusion, ray tracing, or a post-process material needs it.
3. **Ambient occlusion**.
4. **Ray tracing**: traced shadows, occlusion, and reflections, smoothed.
5. **The scene**, multisampled, in high dynamic range, at the render scale:
   the sky or background, opaque objects, translucent objects from back to
   front, then debug lines. With path tracing on, the traced image replaces
   this pass.
6. **Temporal anti-aliasing** and your `BeforeToneMap` materials.
7. **Bloom and exposure**.
8. **Tone mapping and grading**.
9. **FXAA** and your `AfterToneMap` materials.
10. **Scaling** to the target's size.

Each pass is described on its own page in this section.

## Culling and batching

Objects outside the view are rejected before they reach the GPU. An object
outside the view that can cast a shadow into it is still drawn into the shadow
map. Opaque objects are sorted by material, texture, and mesh, and each run
that shares all three is one instanced draw call. Actors sharing a material
stay in one draw call even when their per-actor uniform values differ; the
[rendering example](https://github.com/cresmarmat-an/opane-ludifex-examples/tree/main/07-rendering)
draws sixty-four spheres of different colours in a single call.

```cpp
world.GetLastDrawCallCount();     // every GPU draw: shadow, scene, and full-screen passes
world.GetDrawnInstanceCount();    // objects submitted to the scene
world.GetCulledCount();           // objects rejected because they were out of view
world.GetLargestBatchSize();      // the most objects in a single draw call
```

## Smooth motion

Physics runs at a fixed rate and frames do not, so reading `GetPosition()`
every frame makes motion stutter when several frames fall within one step.
`Render()` uses interpolated transforms, so motion on screen is smooth. When
you need the drawn position yourself, for a label that follows an object or a
renderer of your own, read it the same way:

```cpp
const ludifex::Transform3 transform = actor.GetInterpolatedTransform();

world.GetInterpolationAlpha();   // 0 at the last step, approaching 1 at the next
```

## Limitations

- One camera per world, with a perspective projection only. There is no
  orthographic 3D camera, no split screen within one world, and no stereo or
  VR rendering.
- The final image is 8 bits per channel. There is no HDR display output.
- A world renders at one size into one target. To show the same world twice at
  different sizes, draw it once and display the texture twice.
