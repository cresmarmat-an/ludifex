# Light, shadow, and fog

Every world has a sun: one directional light that casts shadows. An ambient
colour lights the parts the sun does not reach, fog fades the distance, and
point lights add local light.

## The sun and the ambient light

```cpp
ludifex::RenderSettings& settings = world.GetRenderSettings();

settings.SkyColor = ludifex::Color::FromBytes(24, 28, 38);        // the background, when there is no sky or image
settings.AmbientColor = ludifex::Color::FromBytes(96, 106, 130);  // light from every direction

settings.Light.Direction = { -0.45f, -1.0f, -0.35f };   // the direction the sunlight travels
settings.Light.Tint = ludifex::Color::FromBytes(255, 250, 240);
settings.Light.Intensity = 1.0f;

settings.Exposure = 1.0f;   // scales the whole image before tone mapping
```

The ambient light is stronger from above than from below, so shapes keep their
form on the side away from the sun. With the [sky](sky.md) turned on, the sky's
colours replace `AmbientColor`.

## Shadows

```cpp
settings.Shadows.Enabled = true;      // on by default
settings.Shadows.Distance = 60.0f;    // how far from the camera shadows reach
settings.Shadows.Resolution = 2048;   // each map's size, in texels per side
settings.Shadows.Cascades = 3;        // 1 to 4 maps, nearest sharpest
settings.Shadows.Softness = 1.5f;     // the width of the soft edge, in shadow-map texels
```

The sun casts shadows into maps fitted around what the camera sees. The
distance is cut into `Cascades` slices, each with its own map. The nearest
slice is short, so shadows near the camera are sharp; the farthest is long, so
shadows still reach into the distance. Neighbouring cascades blend where they
meet, so there is no visible line where sharpness changes, and the last one
fades out at the edge. The maps move in whole texels as the camera moves, so
shadow edges do not crawl. A shorter `Distance` spends the same maps on less
ground, which makes shadows sharper.

For shadows traced exactly at any distance, see [Ray tracing](ray-tracing.md).

## Fog

```cpp
settings.Fog.Enabled = true;
settings.Fog.Tint = settings.SkyColor;
settings.Fog.Start = 30.0f;   // metres from the camera where fog begins
settings.Fog.End = 150.0f;    // where it is complete
```

Fog is linear between `Start` and `End`. Custom materials that call `Shade()`
are fogged the same way; see [Custom shaders](custom-shaders.md).

## How surfaces are lit

Lighting is computed in linear light with a standard metallic-roughness model:
a GGX specular highlight, energy-conserving diffuse, and ambient light with a
reflection that depends on roughness. Every colour you give (actor colours,
the sky, light tints) is in sRGB, as a colour picker or an image file gives
it, and is converted on its way to the GPU.

The final image is tone mapped. The default curve leaves values below 0.9 as
authored, so the sky colour you chose appears as that colour, and rolls
brighter values off smoothly so highlights soften instead of clipping.
[Grading](bloom-exposure-grading.md) offers a filmic curve too.

Lighting is forward, not deferred, which keeps multisampling affordable; the
default anti-aliasing is real multisampling.

## Point lights

```cpp
ludifex::Light3D lamp = world.AddPointLight({
    .Position = { 0.0f, 2.0f, 0.0f },
    .Tint = ludifex::Color::FromBytes(255, 190, 110),
    .Intensity = 5.0f,
    .Range = 6.0f,          // no light at all beyond this distance
});

lamp.SetPosition({ 1.0f, 2.0f, 0.0f });
lamp.SetColor(ludifex::Color::FromBytes(255, 120, 80));
lamp.SetIntensity(3.0f);
lamp.SetRange(8.0f);
lamp.AttachTo(lantern, { 0.0f, 0.6f, 0.0f });   // follows the actor from now on
lamp.Detach();
lamp.Destroy();

world.GetLightCount();
```

A point light falls off with the square of distance, shaped so it reaches
exactly zero at its range. An attached light follows its actor's drawn
position at an offset in the actor's own space, and detaches by itself if the
actor is destroyed. `Light3D` is a handle like `Actor3D`: after `Destroy` it
reports `IsValid() == false` and ignores calls.

## How many point lights

Up to **256 point lights** shade a frame. When more are in the world, the
ones contributing most to what the camera sees are used, judged by intensity,
range, and distance, not by the order they were created.

Lights are clustered: the view is divided into a grid (16 tiles across, 9
down, and 24 slices in depth), and each frame every cell is given the lights
that reach it. A pixel shades against the lights in its own cell only, so many
lights stay affordable. At most **32 lights** can affect any one cell; in a
cell with more, the rest are skipped. Nothing needs configuring, and a scene
with few lights pays almost nothing for the grid.

## Limitations

- One directional light (the sun) per world.
- Only the sun casts shadows. Point lights do not.
- There are no spot lights, area lights, or light textures (cookies).
- Fog is linear and uniform; there is no exponential, height, or volumetric
  fog.
- There is no baked lighting (lightmaps) and no real-time global illumination
  in the rasterized image. Light bouncing between surfaces is only in the
  [path tracer](path-tracing.md).
