# Debug drawing

Draw lines, boxes, and spheres into a world to see what your code is doing, or
turn on drawing of the physics shapes themselves:

```cpp
world.DrawLine(from, to, ludifex::Color{ 1, 0.2f, 0.2f, 1 });
world.DrawBox(center, size);                 // an outlined box, axis-aligned
world.DrawSphere(center, radius);            // an outlined sphere

world.SetPhysicsDebugDraw(true);             // every collider's bounds and every joint's anchors
```

The colour defaults to yellow. In a 2D world, `DrawLine` takes `Vec2` points.

Debug lines last for one frame, so call these every frame you want them
shown. They are depth-tested against the scene, so walls hide lines behind
them, and they cost nothing once you stop calling them.

## Limitations

- Lines are one pixel wide, and there is no text, filled shape, or arrow
  drawing in the world. For labels, convert a position to the screen with
  `WorldToView` (2D) or your camera, and draw the text with opane or your own
  interface.
- Lines cannot be drawn on top of everything; they are always depth-tested.
- In 2D, only `DrawLine` is available.
