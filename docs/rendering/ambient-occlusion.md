# Ambient occlusion

Ambient occlusion darkens creases, corners, and the ground beneath objects,
where light from the sky would be partly blocked. It gives a scene contact and
depth. It is one of a world's render settings:

```cpp
ludifex::AmbientOcclusionSettings& occlusion = world.GetRenderSettings().AmbientOcclusion;
occlusion.Enabled = true;   // on from the Medium preset up
occlusion.Radius = 0.5f;    // how far a surface looks for what blocks it, in metres
occlusion.Intensity = 1.0f;
occlusion.Samples = 12;     // 4 to 32; more is smoother and costs more
```

## How it works

It is computed each frame from the depth buffer (screen-space ambient
occlusion). For each pixel, points are scattered through the hemisphere above
its surface, and every point the depth buffer shows to be buried counts
against the light reaching it. A blur that stops at edges smooths the result.
It is computed at half the render resolution in each direction, a quarter of
the pixels, and scaled up with filtering; since it is soft anyway, the result
looks the same.

Only ambient light is darkened. Sunlight and point lights have their own
shadows. An object much closer to the camera than the surface behind it does
not cast a dark halo onto that surface.

## Traced ambient occlusion

[Ray tracing](ray-tracing.md) offers ambient occlusion traced against the
world instead of the screen. It replaces the screen-space kind when it is on.

## Limitations

- Screen-space occlusion only knows what is on screen. Something off screen or
  hidden behind another object does not darken what it should, and occlusion
  can change as the camera turns. Traced occlusion does not have this problem.
- Translucent objects neither receive nor cast screen-space occlusion.
- It does not apply to 2D worlds.
