<p align="center">
  <img src="Icon.png" alt="" width="128" />
</p>

<h1 align="center">ludifex</h1>

<p align="center">
  2D and 3D world library for C++20, built on Box2D v3 and Box3D.
</p>

<p align="center">
  <a href="https://cresmarmat-an.github.io/ludifex/"><strong>Documentation</strong></a> ·
  <a href="https://cresmarmat-an.github.io/ludifex/getting-started/installation/">Installation</a> ·
  <a href="https://github.com/cresmarmat-an/ludifex-examples">Examples</a> ·
  <a href="CHANGELOG.md">Changelog</a>
</p>

<p align="center">
  <img alt="Version 0.0.1" src="https://img.shields.io/badge/version-0.0.1-4E39FC" />
  <img alt="C++20" src="https://img.shields.io/badge/C%2B%2B-20-161F34" />
  <img alt="Direct3D 12, Vulkan, and Metal" src="https://img.shields.io/badge/backends-Direct3D%2012%20%7C%20Vulkan%20%7C%20Metal-4E39FC" />
  <a href="LICENSE"><img alt="MIT License" src="https://img.shields.io/badge/license-MIT-161F34" /></a>
</p>

ludifex simulates 2D and 3D worlds with Box2D and Box3D and renders them with
physically based shading, cascaded shadows, and optional ray and path tracing in
compute shaders. It also loads models, plays animation, and handles positional
sound. It does not depend on [opane](https://github.com/cresmarmat-an/opane):
worlds can step without a window, so ludifex works in tests, servers, and
tools. With opane, a world can fill the window or appear inside a viewport in
your interface.

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

## Getting it

Fetch it from GitHub and link it:

```cmake
include(FetchContent)
FetchContent_Declare(ludifex
    GIT_REPOSITORY https://github.com/cresmarmat-an/ludifex.git
    GIT_TAG v0.0.1)
FetchContent_MakeAvailable(ludifex)

target_link_libraries(my_app PRIVATE ludifex::ludifex)
```

SDL3, Box2D, Box3D, cgltf, stb_image, miniaudio, and Assimp are fetched with it.
SDL3 is built as a shared library; copy its DLL beside your program:

```cmake
add_custom_command(TARGET my_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_RUNTIME_DLLS:my_app>" "$<TARGET_FILE_DIR:my_app>"
    COMMAND_EXPAND_LISTS)
```

It needs CMake 3.22 or later, a C++20 compiler, and a shader compiler for each
backend you build: `dxc` from the Windows SDK for Direct3D 12, and the
[Vulkan SDK](https://vulkan.lunarg.com) for Vulkan. [Installation](https://cresmarmat-an.github.io/ludifex/getting-started/installation/)
covers installing it once and using `find_package`, and every build option.

## Documentation

The documentation lives at
**[cresmarmat-an.github.io/ludifex](https://cresmarmat-an.github.io/ludifex/)**.
Its pages are the Markdown files in [`docs/`](docs), organized by what you
want to do:

| | |
|---|---|
| [Getting started](https://cresmarmat-an.github.io/ludifex/getting-started/introduction/) | Installation, finding files, and the units and conventions |
| [Worlds](https://cresmarmat-an.github.io/ludifex/worlds/creating-a-world/) | Creating a world, actors, stepping, the transform hierarchy, saving, 2D worlds, and the contracts that keep them safe |
| [Physics](https://cresmarmat-an.github.io/ludifex/physics/joints/) | Joints, collision events, sensors, queries, and characters |
| [Rendering](https://cresmarmat-an.github.io/ludifex/rendering/overview/) | Backends, light and shadow, surfaces, quality presets, the sky, ambient occlusion, bloom, ray and path tracing, and custom shaders |
| [Assets](https://cresmarmat-an.github.io/ludifex/assets/model-formats/) | Model formats, loading without stopping, and reloading while it runs |
| [Animation](https://cresmarmat-an.github.io/ludifex/animation/animation/) | Skeletons, skins, morph targets, and clips |
| [Sound](https://cresmarmat-an.github.io/ludifex/sound/sound/) | Positional voices, the listener, occlusion, and music |
| [Measuring and debugging](https://cresmarmat-an.github.io/ludifex/tools/measuring-a-frame/) | Measuring a frame, diagnostics and validation, and native access |
| [Reference](https://cresmarmat-an.github.io/ludifex/reference/status/) | Capabilities and limitations, the license, the changelog, and third-party notices |

## Examples

- [ludifex-examples](https://github.com/cresmarmat-an/ludifex-examples): the world checks, running standalone, and the benchmark
- [opane-ludifex-examples](https://github.com/cresmarmat-an/opane-ludifex-examples): ludifex worlds hosted by opane, from a falling box to the graphics showcase

## Status

0.0.1 is the first release. It has been tested on Windows with Direct3D 12 and
Vulkan. Linux and macOS builds are supported but have not been tested on real
hardware yet.
[Capabilities and limitations](https://cresmarmat-an.github.io/ludifex/reference/status/)
lists what ludifex can and cannot do.

## License

ludifex is released under the [MIT License](LICENSE). Copyright (c) 2026 Cresmar
Mat-an.

A program built with ludifex also contains SDL3, Box2D, Box3D, cgltf, stb,
miniaudio, and Assimp, each under its own permissive license;
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) reproduces them all.
