# Graphics quality

A quality preset sets how much work the renderer does, and leaves the look of
the world alone. Use presets for an options menu, and to adapt to slower and
faster GPUs.

```cpp
world.SetGraphicsQuality(ludifex::GraphicsQuality::Ultra);

// or on settings of your own, before applying them
ludifex::ApplyGraphicsQuality(settings, ludifex::GraphicsQuality::Low);

ludifex::GetGraphicsQualityName(ludifex::GraphicsQuality::Ultra);   // "Ultra"
```

| Preset | Render scale | Anti-aliasing | Shadows (map size, maps, distance) | Ambient occlusion | Bloom | Level of detail |
|---|---|---|---|---|---|---|
| `Potato` | 50% | `Off` | off | off | off | most aggressive |
| `Low` | 75% | `Fast` | 1024, 1, 30 m | off | off | aggressive |
| `Medium` | 100% | `Balanced` | 2048, 2, 40 m | 8 samples | on | normal |
| `High` (default) | 100% | `Balanced` | 2048, 3, 60 m | 12 samples | on | normal |
| `Ultra` | 100% | `High` | 2048, 4, 90 m, softer | 16 samples | on | finer |
| `Extreme` | 150% | `Balanced` | 4096, 4, 120 m, softer | 24 samples | on | always full |

A world starts at `High`. A preset sets only the fields in this table:
`RenderScale`, `Mode`, `Shadows`, `AmbientOcclusion`, `Bloom.Enabled`, and
`Detail`. Colours, lights, fog, the sky, grading, and exposure are unchanged,
and so are ray tracing and path tracing, which can be added to any preset. To
make a preset of your own, apply one and then change any field.

A 2D world has no shadows or ambient occlusion, so its presets set only the
render scale, anti-aliasing, and bloom.

## Render scale

`RenderScale` is the resolution the world is drawn at, relative to its target,
from 0.25 to 2:

- Below 1 is faster and softer. The finished image is scaled up and lightly
  sharpened at the end.
- Above 1 draws more pixels than are shown and scales them down, which gives
  the best anti-aliasing at the highest cost. `Extreme` draws 2.25 times as
  many pixels as `High`.

The render target a host wrapped keeps its size and address either way.

## Choosing a preset

How fast each preset runs depends entirely on the GPU and the scene, so
measure on the machines you care about. The
[graphics example](https://github.com/cresmarmat-an/opane-ludifex-examples/tree/main/18-graphics)
has a `--tour` option that runs every preset and tracing mode on your own
machine and prints the frame time of each.

As a rule of thumb:

- `Extreme` costs far more than `Ultra`, mainly because of its render scale.
- Traced ambient occlusion replaces the screen-space kind instead of adding to
  it.
- Path tracing is much slower than any preset and is meant for still images.

## Limitations

- Presets do not detect the GPU or pick a level for you; choose one in your
  program, or let the user choose.
- There is no dynamic resolution that adjusts the render scale to hold a frame
  rate.
- The upscaling at render scales below 1 is a simple filter; there is no
  temporal upscaler such as DLSS, FSR, or XeSS.
