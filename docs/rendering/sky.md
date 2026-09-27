# The sky

The sky replaces the flat `SkyColor` background with a gradient of three
colours and a sun. Turn it on in the world's render settings:

```cpp
ludifex::SkySettings& sky = world.GetRenderSettings().Sky;
sky.Enabled = true;
sky.Zenith = ludifex::Color::FromBytes(46, 98, 186);    // straight up
sky.Horizon = ludifex::Color::FromBytes(172, 200, 228);
sky.Ground = ludifex::Color::FromBytes(62, 58, 54);     // below the horizon
sky.SunSize = 1.0f;      // the sun's disc, 1 as seen from Earth; 0 hides it
sky.Brightness = 1.0f;
```

The sun is drawn where the directional light comes from, with a glow around
it. Its disc is far brighter than the light it casts, so
[bloom](bloom-exposure-grading.md) makes it flare.

## The sky lights the world

With the sky on, it also replaces `AmbientColor` as the ambient light: a floor
takes its ambient light mostly from the zenith colour, and a wall from the
horizon and ground colours. Glossy surfaces reflect the sky, more blurred the
rougher they are. When [ray tracing](ray-tracing.md) or
[path tracing](path-tracing.md), rays that hit nothing see the same sky, so
the rasterized and traced images match.

A [background image](backgrounds-and-post-processing.md#backgrounds) still
covers the sky where there is one, but the sky keeps lighting the world.

## Time of day

Move the sun by changing `settings.Light.Direction`; the sky's sun follows.
Change the three colours and `Brightness` along with it for dusk or night.

## Limitations

- The sky is a three-colour gradient and a sun. There are no clouds, stars,
  moon, or physically based atmosphere.
- There are no HDR environment maps or sky boxes (cube maps). A background
  image is flat and does not light the world.
- Time of day is not animated for you; set the light and colours yourself.
