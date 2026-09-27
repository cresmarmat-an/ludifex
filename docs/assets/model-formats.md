# Model formats

glTF (`.gltf`, `.glb`) is read by cgltf. Everything else is read by
[Assimp](https://github.com/assimp/assimp): FBX, OBJ, Collada (`.dae`), 3DS,
Blender (`.blend`), PLY, STL, DirectX (`.x`), LightWave, Milkshape, Quake and
Half-Life models, Ogre, IFC, and the other formats Assimp supports. Whichever
reader is used, the model ends up in the same form, and the rest of ludifex
does not know which one read it.

```cpp
ludifex::Actor3D robot = world.AddModel({
    .Path = "characters/robot.glb",
    .Position = { 0.0f, 0.0f, 2.0f },
    .Scale = 1.0f,
});
```

## What is read

- **Geometry:** triangles, normals, and the first set of texture coordinates.
  Polygons are split into triangles, and smooth normals are generated where a
  file has none.
- **Materials:** metallic-roughness values and maps where the file has them.
  Older files that describe diffuse, specular, and shininess are converted:
  the diffuse colour becomes the base colour, and the shininess becomes a
  roughness that gives a highlight of about the same size. Separate metalness
  and roughness maps are packed into the single map the shader reads. Normal,
  emissive, and occlusion maps are read too. glTF's
  `KHR_materials_emissive_strength` is honoured, so emission can be brighter
  than 1. A part with opacity below 1, or a glTF part in blend mode, is drawn
  as transparent.
- **Textures:** files next to the model or embedded in it. When a file records
  a texture by an absolute path from the artist's machine, which is common in
  FBX, the texture is looked for next to the model under its file name.
- **Skeletons, skins, and animation clips.** Parts that a clip moves as a
  whole, such as a door on a hinge or a turret on a tank, follow their node as
  if they were skinned to a single joint. glTF's step, linear, and cubic
  spline interpolation are all supported.
- **Morph targets** (blend shapes) and the clips that animate their weights.
- **Units and axes:** FBX files in centimetres are scaled to metres, and files
  from Z-up tools are turned so that Y is up, as in ludifex.

The same file loaded twice, or two files with identical contents, are parsed
and uploaded once. See
[Reloading while it runs](reloading-while-it-runs.md#caching-by-contents).

## Leaving Assimp out

Configure with `-DLUDIFEX_ASSIMP=OFF` to build without Assimp. The build is
much faster and smaller, and only glTF files can be loaded. Converting your
models to glTF with Blender or another tool is a good way to ship without it.
See [Installation](../getting-started/installation.md#build-options).

## Limitations

- Only the first set of texture coordinates is used, and vertex colours are
  ignored.
- Each vertex is influenced by at most four joints, and a model can have at
  most 255 joints across all its skins. A mesh that needs more is drawn
  without skinning, and the log says so.
- Cameras and lights stored in a model file are ignored. Add lights with
  `AddPointLight` and the sun settings instead.
- glTF extensions other than emissive strength are not read. Draco and
  meshopt compressed meshes, KTX2 and Basis textures, texture transforms,
  clearcoat, sheen, transmission, and volume are not supported.
- A model's [collider](../worlds/adding-actors.md) is a box fitted to its
  bounds, not its actual shape.
- How faithfully a non-glTF file arrives depends on Assimp. Materials from
  FBX and Blender files in particular can come through only partly; check
  them, and prefer glTF for anything you ship.
