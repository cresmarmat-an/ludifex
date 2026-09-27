# Adding actors

An actor is one object in the world: a body the physics engine simulates and a
shape the renderer draws. Each `Add` call returns an `Actor3D` handle.

## Primitive shapes

```cpp
ludifex::Actor3D box = world.AddBox({
    .Scale = { 1.0f, 1.0f, 1.0f },     // full size along each axis, in metres
    .Position = { 0.0f, 5.0f, 0.0f },  // the centre
    .Rotation = {},
    .Type = ludifex::BodyType::Dynamic,
    .Density = 1.0f,                    // kilograms per cubic metre of shape
    .Friction = 0.3f,
    .Restitution = 0.0f,                // bounciness, 0 to 1
    .IsBullet = false,                  // extra collision checks for small, fast bodies
    .IsSensor = false,                  // detects overlaps without colliding
    .Name = "Crate",
});

ludifex::Actor3D ball = world.AddSphere({
    .Radius = 0.5f,
    .Position = { 0.0f, 10.0f, 0.0f },
    .Restitution = 0.4f,
});

ludifex::Actor3D pillar = world.AddCapsule({
    .Radius = 0.4f,
    .Height = 2.0f,                     // total height, including both rounded ends
    .Position = { 2.0f, 1.0f, 0.0f },
});
```

## The ground

`AddGround` adds a static slab. Its `Position` is the **top surface**, not the
centre, so the default puts the walkable surface at height 0:

```cpp
ludifex::Actor3D ground = world.AddGround({
    .Width = 50.0f,
    .Depth = 50.0f,
    .Thickness = 1.0f,
    .Friction = 0.6f,
});
```

## Models

```cpp
ludifex::Actor3D prop = world.AddModel({
    .Path = "gem.gltf",                 // found through the asset roots
    .Position = { 0.0f, 6.0f, 0.0f },
    .Rotation = {},
    .Scale = 1.0f,                      // uniform scale of the model as authored
    .Type = ludifex::BodyType::Dynamic,
    .Name = "Gem",
});

ludifex::Actor3D quick = world.AddModel("gem.gltf");   // defaults for everything else
```

A model file is glTF or any format Assimp reads; see
[Model formats](../assets/model-formats.md). Each file is loaded once however
many actors use it, and those actors share one set of GPU buffers. A model is
drawn as its file describes: every part with its own material and maps, placed
through the file's node hierarchy. See
[Surfaces and textures](../rendering/surfaces-and-textures.md).

A model's collider is a box fitted to its bounds. A flat model, such as a sign
or a leaf, gets a box a centimetre thick instead of being refused. A model that
cannot be found or read still appears, as a one-metre cube with the
missing-texture checkerboard, named `missing: <path>`, so the program keeps
running and the problem is easy to see. Models can also load on a background
thread; see [Loading without stopping](../assets/loading-without-stopping.md).

## Body types

| `BodyType` | Behaviour |
|---|---|
| `Static` | Never moves. Ground, walls, and scenery. |
| `Dynamic` | Moved by forces, gravity, and collisions. Needs a density above zero. |
| `Kinematic` | Moved only by you, through its position or velocity; pushes dynamic bodies but is not pushed back. Platforms and doors. |

## Other actors

These are added with calls of their own and covered on their own pages:

- Characters (`AddCharacter`): see [Characters](../physics/characters.md).
- Ragdolls (`AddRagdoll`): see [Joints](../physics/joints.md#ragdolls).
- Sprites and 2D shapes: see [2D worlds](2d-worlds.md).

## Limitations

- Collision shapes are boxes, spheres, and capsules. Models collide as a box
  fitted to their bounds. There are no convex hull, triangle mesh, height
  field, or compound colliders; join several actors with a
  [weld joint](../physics/joints.md) to build a compound shape.
- An actor's size is fixed when it is created. `GetScale` reads it, but there
  is no way to resize an actor; destroy it and add a new one.
- A model can only be scaled uniformly.
- There are no planes, cylinders, or cones as primitive shapes.
