# Finding files

Every file ludifex loads (models, textures, sounds, music, and material
shaders) is found by name through an ordered list of folders, called asset
roots. That lets you write short names such as `"robot.gltf"` instead of full
paths.

A relative name is looked for in this order:

1. the roots you add, most recent first;
2. `./`, `./assets/`, and `./assets/models`, `images`, `sounds`, `music`,
   `shaders`, and `fonts` under the working directory;
3. the same folders beside the executable, so a program finds its files no
   matter which folder it was started from.

```cpp
ludifex::AddAssetRoot("C:/games/mygame/content");

ludifex::ResolveAssetPath("robot.gltf");   // the full path it resolves to, or ""
ludifex::GetAssetRoots();                  // the roots in search order
ludifex::ClearAssetRoots();                // back to the defaults
```

The first match wins, and the result is remembered, so later loads of the same
name do not search again. Absolute paths are used as they are.

When a name matches nothing, the log lists every path that was tried. What
happens next depends on the loader: a missing texture draws as a
magenta-and-black checkerboard, a missing model stays a plain box, and a
missing sound is silent.

Files a model refers to (its buffers and textures) are looked for beside the
model first.

## Limitations

- Files are read from the file system only. There is no support for reading
  from zip archives or packed asset files.
- Names are matched as the file system matches them, so on Linux they are case
  sensitive.
- Asset roots are shared by the whole process. opane keeps a separate list, set
  with `opane::AddAssetRoot`.
