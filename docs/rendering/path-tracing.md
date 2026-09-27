# Path tracing

For a reference image, or a still image with the most accurate lighting
ludifex can produce, turn on the path tracer:

```cpp
ludifex::PathTracingSettings& path = world.GetRenderSettings().PathTracing;
path.Enabled = true;
path.MaxBounces = 4;        // how many times light may bounce on its way to the camera
path.SamplesPerFrame = 1;
path.MaxSamples = 1024;     // after this many samples the image stops changing

world.GetPathTracedSampleCount();   // rises while nothing moves; 0 when off
```

## How it works

Instead of rasterizing, the whole image is computed by following light as it
bounces. Each pixel is traced back into the world from surface to surface. At
each bounce, the path samples the sun and one randomly chosen point light
directly, then continues in a direction chosen from the surface's material:
anywhere over the hemisphere for a matte surface, toward the highlight for a
glossy one. Soft shadows, colour bleeding from one surface onto another,
reflections of reflections, and light from emissive surfaces all come from
this one method. Translucent surfaces let paths through in proportion to how
transparent they are.

## Accumulation

The image builds up over time. While the camera and the world are still, each
frame adds samples and the image becomes less noisy: grainy at first, clean
after a few hundred samples. When anything changes (the camera, an actor, a
light, a setting), it starts again. Pause animations and physics while path
tracing, or the image will keep restarting.

Fog, the sky or background, bloom, exposure, and grading apply to the
path-traced image as to any frame.

## Limitations

- It is far too slow for real-time use and meant for stills, previews, and as
  a lighting reference.
- There is no denoiser. The image stays noisy until enough samples have
  accumulated.
- It uses the same scene as [ray tracing](ray-tracing.md), with the same
  limits: no normal maps, metallic-roughness maps, or custom materials, and
  textures at reduced resolution.
- Only the sun and one point light are sampled directly at each bounce, so
  scenes lit by many small point lights converge slowly.
- There is no depth of field, motion blur, or volumetric light.
- 2D worlds cannot be path traced.
