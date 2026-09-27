# Units and conventions

## Units

ludifex uses **metres, kilograms, and seconds**. Both physics engines are tuned
for objects sized like real ones, from a few centimetres to tens of metres, so
model your world at real scale and the simulation behaves well. The default
gravity is 10 metres per second squared downward.

## Axes

3D worlds are **right-handed with +Y up**. An actor faces down its own -Z
axis, which matters for directional sounds.

2D worlds use X to the right and **+Y up**, with angles in radians measured
counter-clockwise.

## Rotations

3D rotations are unit quaternions. `Quat{}` is no rotation.

```cpp
ludifex::Quat tilt = ludifex::Quat::FromAxisAngle({ 1.0f, 0.0f, 0.0f }, 0.3f);   // radians
ludifex::Quat look = ludifex::Quat::FromEuler(yaw, pitch, roll);                  // Y, then X, then Z

ludifex::Quat both = tilt.Then(look);          // tilt first, then look
ludifex::Quat undo = tilt.Inverse();
ludifex::Vec3 turned = tilt.Rotate({ 0.0f, 0.0f, -1.0f });
```

## Types

`Vec2`, `Vec3`, `Quat`, `Color`, `Transform2`, `Transform3`, and `Mat4` are
plain structs with public members (`X`, `Y`, `Z`, `W`, and so on). `Mat4` is
column-major, as the shaders expect, and has `Perspective`, `Orthographic`,
`LookAt`, and `FromTransform` helpers.

## Colours

Colours are given in **sRGB**, the way a colour picker or an image file gives
them, with values from 0 to 1. `Color::FromBytes(r, g, b, a)` takes 0 to 255.
Lighting is computed in linear light, and colours are converted on their way
to the GPU.

## Handles

Actors, joints, lights, textures, materials, sounds, and voices are handles:
an index plus a generation counter. A handle to something that has been
destroyed is recognised as stale, and using it does nothing and logs a
message. See [The mutability contract](../worlds/mutability-contract.md).

## Limitations

- Positions are 32-bit floats. Precision drops far from the origin, so
  simulation and rendering get less accurate beyond a few kilometres. There is
  no double-precision or origin-shifting mode.
- 2D and 3D types are separate (`Vec2` and `Vec3`, `Actor2D` and `Actor3D`).
  An actor cannot move between a 2D and a 3D world.
