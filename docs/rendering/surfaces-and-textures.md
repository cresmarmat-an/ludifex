# Surfaces and textures

An actor's surface is its colour, its roughness, and how metallic it is, with
any textures and maps on top.

## Colour, roughness, and metalness

```cpp
actor.SetColor(ludifex::Color::FromBytes(226, 162, 78));
actor.SetRoughness(0.25f);   // 0 is a mirror-sharp highlight, 1 none; default 0.6
actor.SetMetallic(1.0f);     // 0 for plastic, wood, and stone; 1 for bare metal
actor.SetVisible(false);

actor.GetColor();
actor.GetRoughness();
actor.GetMetallic();
actor.IsVisible();
```

Static bodies start a cool grey and dynamic bodies a warm orange, so a scene
is readable before anything has been styled.

## Translucency

A colour with alpha below 1 makes an actor translucent. It is drawn after
everything opaque, sorted from back to front, and blended, which suits glass:

```cpp
glass.SetColor(ludifex::Color::FromBytes(150, 210, 255, 80));
```

## Textures

```cpp
ludifex::TextureId bricks = ludifex::LoadTexture("bricks.png");

crate.SetTexture(bricks);                  // multiplied by the actor's colour
floor.SetTexture(ludifex::LoadTexture("tiles.png"));
floor.SetTextureTiling({ 20.0f, 20.0f });  // repeat it 20 times each way
floor.SetTextureTiling({ 20.0f, 20.0f }, { 0.5f, 0.0f });   // and shift it

crate.SetTexture({});                      // back to the model's own texture, or none
```

`LoadTexture` reads PNG, JPEG, BMP, TGA, GIF (the first frame), PSD, and HDR
files through the [asset roots](../getting-started/finding-files.md). Loading
the same name again returns the same texture. Every texture gets a full mip
chain, built in linear light and weighted by alpha, and is sampled with
trilinear 8x anisotropic filtering, so a distant floor does not shimmer.
Textures are shared by every world in the process.

A file that cannot be found or decoded gives a magenta-and-black checkerboard,
and the log lists every path that was tried. `CreateTexture(width, height,
rgbaPixels, usage)` makes a texture from pixels you already have (four bytes
per pixel, straight alpha, rows from top to bottom). `GetTextureSize` and
`DestroyTexture` do what their names say.

## Texture usage

A texture is loaded for what it holds, because colours and numbers are stored
differently:

```cpp
ludifex::TextureId albedo = ludifex::LoadTexture("brick_color.png");   // colour, sRGB
ludifex::TextureId bumps = ludifex::LoadTexture("brick_normal.png", ludifex::TextureUsage::Normal);
ludifex::TextureId mask = ludifex::LoadTexture("brick_mask.png", ludifex::TextureUsage::Data);
```

| `TextureUsage` | For | Stored and filtered |
|---|---|---|
| `Color` (default) | Base colour and emission | Decoded from sRGB and averaged in linear light. |
| `Data` | Roughness, metalness, occlusion, masks | Exactly as stored. |
| `Normal` | Tangent-space normal maps | As stored, with each smaller mip level renormalized so distant bumps do not go dim. |

The same image loaded with two usages becomes two textures.

## Normal maps and emission

```cpp
wall.SetNormalMap(bumps);                  // strength 1
wall.SetNormalMap(bumps, 0.5f);            // half as deep; 0 turns it off
lamp.SetEmission(ludifex::Color::FromBytes(255, 200, 120), 4.0f);   // glows brighter than white
```

A normal map bends the surface's normal so small bumps and grooves are lit as
if they were geometry. It needs no tangents in the mesh (the frame it is read
in is worked out per pixel), so it works on every shape ludifex draws. Maps are
read the glTF way, with green pointing up the image.

Emission is light the surface gives off regardless of the scene's lighting. It
is not affected by shadow, and a strength above 1 makes it brighter than white,
which the [bloom](bloom-exposure-grading.md) effect picks up.

## Maps from model files

A glTF model brings its materials with it. Each material's base colour texture
and factor, metallic-roughness map (roughness in green, metalness in blue),
normal map and scale, occlusion map and strength, and emissive map and factor
are used as the file describes, including
`KHR_materials_emissive_strength`. Materials from other formats are converted
when the model loads; see [Model formats](../assets/model-formats.md).
`SetTexture`, `SetNormalMap`, and `SetEmission` on the actor replace the
file's.

## Limitations

- On actors, only the colour texture, normal map, and emission can be set.
  Metallic-roughness and occlusion maps come only from model files.
- `SetTexture` and the other setters apply to the whole actor; a model's parts
  cannot be given different textures one by one.
- Translucent actors are sorted per object, not per triangle, so intersecting
  translucent objects can draw in the wrong order. There is no refraction.
- There are no compressed texture formats (such as BC, ASTC, or KTX2), and HDR
  files are converted to 8 bits per channel.
- Emissive surfaces do not light their surroundings, except in the
  [path tracer](path-tracing.md).
- There is no clear coat, sheen, subsurface scattering, or other extended
  material model.
