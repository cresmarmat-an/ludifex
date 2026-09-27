# Backgrounds and post-processing

A world can draw an image behind everything it renders, and run full-screen
materials over the frame it has finished.

## Backgrounds

```cpp
world.SetBackground("mountains.png");   // or a TextureId
world.SetBackground(ludifex::TextureId{});   // back to the sky or the plain sky colour
```

The image fills the view behind everything, cropped to keep its proportions.
It stays fixed to the view and does not move with the camera. It covers the
[sky](sky.md) if the sky is on, but the sky still lights the world.

## Post-processing

A post-process material is a full-screen shader that runs over the frame at
one of two points:

```cpp
ludifex::MaterialId vignette = ludifex::CreateMaterial({
    .ShaderPath = "vignette.hlsl",
    .Uniforms = { { "Strength", 0.55f } },
});

world.AddPostProcess(vignette, ludifex::PassPoint::AfterToneMap);   // the default point
world.AddPostProcess(grade, ludifex::PassPoint::BeforeToneMap);
world.RemovePostProcess(vignette);
world.ClearPostProcess();
```

| `PassPoint` | Sees | Good for |
|---|---|---|
| `BeforeToneMap` | The scene in linear light, which can be brighter than 1. | Colour grading, effects that should interact with bloom, outlines that read depth. |
| `AfterToneMap` | The final image, after tone mapping and anti-aliasing. | Vignettes, film grain, screen overlays. |

Several materials at the same point run in the order they were added, each
reading the previous one's output. Writing them is covered in
[Custom shaders](custom-shaders.md#post-process-shaders).

## Limitations

- A background is one flat image fixed to the view. There is no parallax,
  sky box, or environment lighting from it.
- Post-process materials are fragment shaders only, with the same limits as
  other [custom shaders](custom-shaders.md).
