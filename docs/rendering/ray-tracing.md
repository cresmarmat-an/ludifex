# Ray tracing

Shadows, ambient occlusion, and reflections can each be traced as rays
through the world instead of worked out from shadow maps and the screen:

```cpp
ludifex::RayTracingSettings& rays = world.GetRenderSettings().RayTracing;
rays.Shadows = true;
rays.AmbientOcclusion = true;
rays.Reflections = true;

rays.ResolutionScale = 0.5f;   // rays are traced at this fraction of the render resolution
rays.SunAngle = 1.0f;          // the sun's diameter in degrees: wider gives softer shadows
rays.OcclusionRadius = 1.5f;   // how far occlusion rays look, in metres
rays.MaxRoughness = 0.6f;      // rougher surfaces reflect the sky instead of tracing

world.IsRayTracingActive();    // whether the last frame traced rays
```

The three effects can be turned on independently and work with any
[quality preset](graphics-quality.md).

## What each effect does

The rasterizer still draws every surface. For each pixel it drew, rays are
traced for what rasterizing cannot know:

- **Shadows:** a ray toward a random point on the sun's disc. Traced sun
  shadows are accurate at any distance, with no cascades and no bias, and get
  softer the farther the shadow falls from what casts it.
- **Ambient occlusion:** a ray over the hemisphere above the surface. Corners
  darken according to what is in the world, even when the blocking object is
  off screen. It replaces the screen-space ambient occlusion.
- **Reflections:** a ray in the direction of the surface's highlight. Smooth
  surfaces reflect the world, including what is off screen, lit by the sun
  (with its own shadow ray) and the ambient light, with textures and emission.
  Rougher surfaces fade to the reflection of the sky and ambient light they
  already have.

## Smoothing over time

Each effect traces one ray per pixel, and one ray only says "blocked" or "not
blocked". The result for a pixel is built up the way a camera gathers light:
over time, by blending each frame into a history that follows the surfaces'
motion, and across the screen, by averaging neighbouring pixels on the same
surface. So a traced shadow settles over a few frames, and one on a fast-moving
object trails slightly.

## Where it runs

Ray tracing runs in compute shaders, not on ray-tracing hardware, so it works
on every device ludifex supports, on Direct3D 12, Vulkan, and Metal alike. It
is expensive; `ResolutionScale` is the main way to control its cost. Where a
device cannot run it, the rasterized effects are used and
`IsRayTracingActive()` returns false.

## The traced scene

The tracer sees every visible actor in the world, not only those in view,
through a two-level tree of bounding boxes. Each mesh gets a tree over its
triangles, built once; a tree over the placed actors is rebuilt every frame,
so moving an object only rebuilds that top tree. Skinned and morphing models
are posed on the CPU each frame and their trees adjusted to the new pose, so
an animated character casts a traced shadow of its current pose. Textures are
copied into an atlas at up to 256 by 256 texels each, and the tracer samples
them from there.

## Limitations

- Traced surfaces do not use normal maps, metallic-roughness maps, or custom
  materials; they are seen with the actor's colour, texture, roughness, and
  metalness only.
- Textures seen in reflections are at most 256 by 256 texels, so reflected
  detail is lower than on screen.
- Point lights are not included in traced reflections, and point lights never
  cast shadows, traced or not.
- Translucent surfaces do not cast traced shadows or block traced ambient
  occlusion.
- Results take a few frames to settle and can trail on fast motion.
- 2D worlds are not traced.
- Posing skinned models on the CPU for the tracer adds cost for each animated
  character in view.
