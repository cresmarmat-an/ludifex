# Reloading while it runs

With hot reload on, saving a model or an image file updates what is on screen
without restarting the program, and every actor keeps its place.

```cpp
ludifex::SetAssetHotReload(true);   // in a development build
ludifex::IsAssetHotReloadEnabled();
```

It is off by default. When on, the timestamp of each loaded file is checked
four times a second, and a file is read only when its timestamp changes. A
file saved again without changes is noticed and ignored, because its contents
are compared as well.

[Custom shaders](../rendering/custom-shaders.md#development-and-shipping)
reload on their own, per material, whether this is on or not.

## What happens on a change

**A model** is parsed again next to the one in use, and swapped in only if
the parse succeeds. A file caught half-written therefore logs an error
instead of leaving a hole in the scene. Every actor using that model has its
collider refitted to the new bounds, the same way as when a
[background load](loading-without-stopping.md) arrives.

**A texture** releases its GPU copy, and the new image is uploaded the next
time it is drawn, at whatever size the new file is.

A file that fails to load keeps the version that worked:

```
(asset) "robot.gltf" is not a glTF file this build can read.
(asset) Reload of "robot.gltf" failed; the model on screen is the one that worked.
```

## Caching by contents

Loading the same path twice returns the same model or texture. Loading the
same contents under a different path does too: files are hashed, and a model
or image already loaded under another name is shared instead of being parsed
and uploaded again.

```
(asset) "props/barrel.gltf" is "shared/barrel.gltf" again; the parse is shared.
```

This catches the same file copied into several folders, one file reached
through two asset roots, and two models embedding the same texture.

Sharing is turned off while hot reload is on. A shared model can only watch
one of its files, so editing the other copy would appear to do nothing.
Shipping builds get sharing; development builds get reloading.

## Limitations

- The check runs when a 3D world is updated. A program with only 2D worlds
  does not reload models or textures.
- A changed model is parsed again on the thread that calls `Update`, so
  saving a large model causes a pause in that frame.
- Sounds are not reloaded. Load them again yourself to hear changes.
- Textures created from bytes in memory have no file to watch.
- A file that failed on its first load is not watched. Load it again once it
  exists.
