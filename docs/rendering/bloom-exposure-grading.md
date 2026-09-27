# Bloom, exposure, and grading

Three groups of settings shape the finished image: bloom makes the brightest
light glow, automatic exposure adapts to how bright the view is, and grading
sets the tone curve and the colour.

## Bloom

```cpp
ludifex::RenderSettings& settings = world.GetRenderSettings();

settings.Bloom.Enabled = true;     // on from the Medium preset up
settings.Bloom.Threshold = 1.2f;   // the brightness where the glow begins, with a soft start
settings.Bloom.Intensity = 0.35f;
settings.Bloom.Radius = 1.0f;      // how far the glow spreads
```

Light brighter than the display can show spreads into its surroundings: a lamp
glows and a reflection of the sun flares, while ordinary brightness is not
affected. The bright parts of the image are shrunk step by step and blurred
back up, so the glow is tight near a light and wide farther out. Each step
weights pixels by their brightness, so a single very bright pixel cannot make
the glow flicker.

Emissive surfaces with a strength above 1 and the sky's sun are the usual
sources of bloom.

## Exposure

```cpp
settings.Exposure = 1.0f;                    // a fixed multiplier, before tone mapping

settings.AutoExposure.Enabled = true;
settings.AutoExposure.Compensation = 0.0f;   // in stops: +1 is twice as bright
settings.AutoExposure.Minimum = 0.25f;       // the darkest it may make the image
settings.AutoExposure.Maximum = 4.0f;        // the brightest
settings.AutoExposure.Speed = 1.5f;          // how quickly it adapts
```

Automatic exposure adapts like an eye: walk from sunlight into a cave and the
cave brightens over about a second. It measures the image's average brightness
on a logarithmic scale, so a few bright highlights do not darken everything,
and eases toward the right exposure. `Exposure` still multiplies the result.

## Grading

```cpp
settings.Grading.Curve = ludifex::ToneCurve::Filmic;
settings.Grading.Contrast = 1.1f;
settings.Grading.Saturation = 1.05f;
settings.Grading.Temperature = 0.2f;    // -1 cooler to 1 warmer
settings.Grading.Tint = 0.0f;           // -1 greener to 1 more magenta
settings.Grading.Lift = ludifex::Color{ 0.02f, 0.02f, 0.05f, 1.0f };   // added to the shadows
settings.Grading.Gain = ludifex::Color{ 1.0f, 0.98f, 0.94f, 1.0f };    // multiplies the highlights
settings.Grading.Vignette = 0.3f;       // darkening toward the corners, 0 to 1
```

The tone curve turns the rendered light into display colours:

- `Neutral` (default) keeps colours below 0.9 exactly as you chose them and
  rolls brighter values off smoothly.
- `Filmic` is the ACES filmic curve: deeper shadows, richer colour, and
  highlights that fade toward white.

The other grading settings are applied after the curve. With every field at
its default, grading changes nothing.

For grading of your own, use a [post-process material](backgrounds-and-post-processing.md#post-processing).

## Limitations

- There are no lens effects such as depth of field, motion blur, lens flares,
  or chromatic aberration.
- Automatic exposure uses the average of the whole image; there are no
  metering modes such as centre-weighted or spot.
- Grading has no lookup-table (LUT) support.
