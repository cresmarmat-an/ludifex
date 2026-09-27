# Anti-aliasing

Aliasing (jagged edges, shimmering textures, flickering highlights) is handled
at several levels. The ones that cost little are always on:

- mip-mapped, anisotropic texture filtering;
- alpha-to-coverage on cutout materials;
- specular anti-aliasing on tightly curved surfaces;
- smoothing of edges drawn inside shaders (for custom materials, through
  `AntiAlias` and `AntiAliasedStep`).

A preset chooses the rest:

| Preset | Multisampling | Final image |
|---|---|---|
| `Off` | none | none |
| `Fast` | none | FXAA |
| `Balanced` (default) | 4x | none |
| `High` | 8x | FXAA |
| `Temporal` | 2x | TAA |

```cpp
world.SetAntiAliasing(ludifex::AntiAliasing::High);
// the same as world.GetRenderSettings().Mode = ludifex::AntiAliasing::High;
```

Multisampling falls back to the highest sample count the device supports.

**Temporal** anti-aliasing moves the camera by a different sub-pixel amount
each frame and blends the frames together using motion vectors. It limits the
history to what the current frame could plausibly show, so moving objects do
not leave trails.

**Specular anti-aliasing** handles what multisampling cannot: multisampling
smooths the edges of shapes but not the shading inside them, so a sharp
highlight on a small sphere would flicker from pixel to pixel as it moves. The
renderer widens highlights where the surface curves sharply within a pixel,
which removes the flicker without blurring flat surfaces.

A render scale above 1 also anti-aliases; see
[Graphics quality](graphics-quality.md#render-scale).

## Limitations

- Temporal anti-aliasing can soften fine detail and leave faint ghosting on
  fast motion, as it does in every renderer.
- FXAA works on the final image and slightly blurs sharp detail such as small
  text drawn in the world.
- There is no SMAA and no vendor upscaler such as DLSS, FSR, or XeSS.
- Skinned limbs' own motion is not in the motion vectors (only the actor's),
  so fast-moving limbs can blur slightly under temporal anti-aliasing.
