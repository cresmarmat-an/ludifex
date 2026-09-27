# Native access

For physics features ludifex does not wrap, `<ludifex/native.h>` returns the
raw Box2D and Box3D handles behind worlds and actors:

```cpp
#include <ludifex/native.h>

b3WorldId rawWorld = ludifex::native::GetNativeWorld(world);
b3BodyId rawBody   = ludifex::native::GetNativeBody(actor);

b2WorldId rawWorld2D = ludifex::native::GetNativeWorld(world2d);
b2BodyId rawBody2D   = ludifex::native::GetNativeBody(actor2d);
```

An invalid world or a destroyed actor returns a zero id. Check it with
`b3World_IsValid` or `b3Body_IsValid` (`b2World_IsValid`, `b2Body_IsValid` in
2D) before using it.

For example, to set linear damping, which ludifex does not expose:

```cpp
b3Body_SetLinearDamping(ludifex::native::GetNativeBody(crate), 0.5f);
```

## Limitations

- Your code is tied to the Box2D and Box3D versions ludifex is built with.
  Box3D is at v0.1.0 and its API is still changing, so code using this
  header may need updating when ludifex updates it. ludifex's own API stays
  the same across those changes.
- ludifex does not know about changes you make through the raw handles.
  Destroying a body, adding or removing shapes, or changing its type
  directly leaves ludifex's records out of step with the physics engine,
  which leads to wrong results or crashes. Use raw access to change
  settings, not to add or remove things.
- Only worlds and bodies are exposed. There is no access to the raw shapes,
  joints, or graphics device through this header.
- Saving a world with [saving and restoring](../worlds/saving-and-restoring.md)
  only records what ludifex tracks, so settings made through the raw handles
  are not saved.
