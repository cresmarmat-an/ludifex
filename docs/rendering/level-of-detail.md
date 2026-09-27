# Level of detail

A sphere or capsule that covers only a small part of the screen is drawn from
a simpler mesh. Each comes in three densities, and the renderer picks one by
how much of the view's height the object covers. The choice follows size on
screen, not distance alone, so a large boulder far away keeps its shape while
a pebble at the same distance is simplified.

```cpp
ludifex::DetailSettings& detail = world.GetRenderSettings().Detail;
detail.Enabled = true;
detail.Simpler = 0.05f;    // below this share of the view's height, a simpler mesh
detail.Simplest = 0.015f;  // below this, the simplest one
```

The [quality presets](graphics-quality.md) set these values.

Simplifying can split a batch: objects drawn from different meshes cannot
share a draw call, so a crowd at mixed distances costs two or three draws
instead of one. That is worth it for thousands of objects and not for a few,
which is why it can be turned off.

## Limitations

- Only spheres and capsules have simpler versions. Boxes are already as
  simple as they can be, and models are always drawn at full detail: there is
  no automatic simplification of model meshes, and levels of detail stored in
  model files are not used.
- Switching between levels is instant; there is no blending between them.
