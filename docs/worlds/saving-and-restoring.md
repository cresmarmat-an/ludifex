# Saving and restoring

A 3D world can be saved as a block of bytes and rebuilt from it, for save
games, level editors, and undo.

```cpp
std::vector<uint8_t> bytes = world.Save();
world.Load(bytes);

world.SaveToFile("saves/slot1.world");
world.LoadFromFile("saves/slot1.world");
```

## What is saved

- Every actor's shape, size, body type, friction, and restitution.
- Where it is, which way it faces, and how fast it is moving.
- Its colour, surface, texture tiling, layer, visibility, and name.
- The model it came from, by path.
- How actors are parented.
- The world's point lights, its camera, and its step settings.

A restored world behaves like the original: stepped the same number of times,
it ends up in the same state.

## What is not saved

- Callbacks your program registered, such as collision handlers.
- Textures and custom materials, which are handles to data your program
  created.
- Joints between actors.
- Sounds, and which voices were playing.

After loading, your program adds these back itself.

## Loading

`Load` destroys everything currently in the world, then rebuilds it. Bytes
that are not a saved world, or that were written by an incompatible version
of the format, are refused with a message, `Load` returns false, and the world
is left as it was.

## Limitations

- Saving is available for 3D worlds only; `World2D` has no `Save` or `Load`.
- Joints, textures, materials, callbacks, and sounds are not saved (see
  above).
- The format is binary and tied to the library version. It is not meant for
  long-term storage across future versions or for editing by hand.
