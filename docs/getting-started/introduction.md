# Introduction

ludifex is a 2D and 3D world library for C++20. A world holds actors (boxes,
spheres, capsules, loaded models, sprites) that are simulated by a physics
engine, drawn by a physically based renderer, animated, and heard through
positional sound. 3D worlds use Box3D and 2D worlds use Box2D v3, behind one
API.

ludifex does not depend on opane. Worlds simulate without a window or a GPU,
so the library also works in tests, on servers, and in tools. When you want a
window, ludifex can open one itself, or [opane](https://cresmarmat-an.github.io/opane/)
can host the world inside its interface.

## A first program

```cpp
#include <ludifex/ludifex.h>

int main()
{
    ludifex::World3D world = ludifex::CreateWorld3D();

    world.AddGround({ .Width = 50.0f, .Depth = 50.0f });

    ludifex::Actor3D crate = world.AddBox({
        .Scale = { 1.0f, 1.0f, 1.0f },
        .Position = { 0.0f, 8.0f, 0.0f },
        .Name = "Crate",
    });

    for (int frame = 0; frame < 180; ++frame)
    {
        world.Update(1.0f / 60.0f);
    }

    const ludifex::Vec3 position = crate.GetPosition();
}
```

Step by step:

1. `CreateWorld3D` makes a world with default settings: gravity of 10 metres
   per second squared downward, and physics stepped 60 times a second.
2. `AddGround` adds a static slab whose top surface is at height 0.
3. `AddBox` adds a dynamic box 8 metres up and returns an `Actor3D`, a handle
   to it.
4. Each `Update` advances the world by one frame. After three seconds the
   crate has fallen and come to rest on the ground.
5. `GetPosition` reads where it ended up. No window or GPU was needed.

To see it, add a call to `world.Run()`, which opens a window and runs the world
until the window is closed. See [Rendering a world](../rendering/overview.md).

## What ludifex includes

- **Worlds and actors:** 3D and 2D worlds, handle-based actors that are safe to
  use after they are destroyed, fixed-step simulation with smooth
  interpolation, a transform hierarchy, and saving a 3D world to bytes.
- **Physics:** collision events with contact points and impact speeds,
  sensors, collision layers, ray casts, shape casts, overlap queries, a
  character controller, joints (hinge, slider, distance, weld, cone, and a 2D
  wheel), ragdolls, and multithreaded stepping.
- **Rendering:** a forward renderer with metallic-roughness shading, cascaded
  shadows from the sun, up to 256 point lights, a procedural sky, fog, ambient
  occlusion, bloom, automatic exposure, colour grading, anti-aliasing presets,
  level of detail, quality presets from Potato to Extreme, and optional ray
  tracing and path tracing in compute shaders.
- **Assets:** models in glTF and every format Assimp reads, textures, loading
  on a background thread, and reloading while the program runs.
- **Animation:** skeletal animation, several skins per model, morph targets,
  and crossfading between clips.
- **Sound:** positional voices with distance falloff, directional cones,
  Doppler shift, occlusion, a voice limit, and streamed music.
- **Tools:** debug drawing, a profiler, worker statistics, validation of every
  input, and access to the underlying Box2D and Box3D handles.

## Platforms

| Platform | Backends |
|---|---|
| Windows | Direct3D 12, Vulkan |
| Linux | Vulkan |
| macOS | Metal |

This release has been tested on Windows with Direct3D 12 and Vulkan. The build
supports Linux and macOS, but they have not been tested on real hardware yet.

## Where to go next

- [Installation](installation.md): add ludifex to a CMake project.
- [Creating a world](../worlds/creating-a-world.md) and
  [Adding actors](../worlds/adding-actors.md).
- [Rendering a world](../rendering/overview.md).
- [Capabilities and limitations](../reference/status.md): what ludifex does and
  does not do.
