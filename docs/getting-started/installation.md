# Installation

ludifex is a CMake project. You can fetch it from GitHub while your project
configures, keep a copy inside your project, or install it once and find it
with `find_package`.

## Requirements

- CMake 3.22 or later.
- A C++20 compiler. This release was tested with MSVC from the Visual Studio
  2026 Build Tools.
- A shader compiler for each graphics API you build for:

| Backend | Shader format | Compiler | Built on |
|---|---|---|---|
| Direct3D 12 | DXIL | `dxc` from the Windows SDK | Windows |
| Vulkan | SPIR-V | `dxc` from the [Vulkan SDK](https://vulkan.lunarg.com) | Windows, Linux |
| Metal | MSL | the Vulkan SDK's `dxc` and `spirv-cross` | macOS |

CMake finds the compilers by itself and prints the formats it will build, for
example `ludifex shader formats: DXIL;SPIRV`. On Windows without the Vulkan
SDK, it says so and builds for Direct3D 12 only.

## Adding it to your project

Fetch it from GitHub and link it:

```cmake
include(FetchContent)
FetchContent_Declare(ludifex
    GIT_REPOSITORY https://github.com/cresmarmat-an/ludifex.git
    GIT_TAG v0.0.1)
FetchContent_MakeAvailable(ludifex)

target_link_libraries(my_app PRIVATE ludifex::ludifex)
```

To use a copy of your own instead, replace the three `FetchContent` lines with
`add_subdirectory(path/to/ludifex)`, or point `FetchContent` at it with
`-DFETCHCONTENT_SOURCE_DIR_LUDIFEX=path/to/ludifex`.

When opane is in the same build, fetch it first; the two libraries then share
one copy of SDL3.

SDL3 is built as a shared library, so copy its DLL next to your executable
after each build:

```cmake
add_custom_command(TARGET my_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_RUNTIME_DLLS:my_app>" "$<TARGET_FILE_DIR:my_app>"
    COMMAND_EXPAND_LISTS)
```

## Installing it once

When ludifex is the top-level project, it installs itself, its dependencies,
and a CMake package. Build both configurations and install them into the same
prefix; debug libraries have a `d` suffix, so the two sit side by side:

```bash
cmake -S path/to/ludifex -B build-ludifex
```

```bash
cmake --build build-ludifex --config Release
```

```bash
cmake --build build-ludifex --config Debug
```

```bash
cmake --install build-ludifex --config Debug --prefix C:/sdk
```

```bash
cmake --install build-ludifex --config Release --prefix C:/sdk
```

Then, in your project, with `C:/sdk` on `CMAKE_PREFIX_PATH`:

```cmake
find_package(ludifex 0.0.1 REQUIRED)
target_link_libraries(my_app PRIVATE ludifex::ludifex)
```

The same `$<TARGET_RUNTIME_DLLS:my_app>` step copies `SDL3.dll` beside your
program. Running `cpack -G ZIP -C "Debug;Release"` in the build directory
makes a zip of the install. The zips attached to each
[GitHub release](https://github.com/cresmarmat-an/ludifex/releases) are made
this way, for Windows x64.

When ludifex is a subdirectory of your project, it installs nothing unless you
set `LUDIFEX_INSTALL=ON`.

## Build options

| Option | Default | Meaning |
|---|---|---|
| `LUDIFEX_INSTALL` | on when top-level | Generate install rules and the CMake package. |
| `LUDIFEX_ASSIMP` | on | Read FBX, OBJ, Collada, and the other formats Assimp supports. Off leaves Assimp out, and only glTF can be loaded. |
| `LUDIFEX_SHADER_FORMATS` | empty | Build exactly these formats, from `DXIL`, `SPIRV`, and `MSL`. Every format named must build. |
| `LUDIFEX_DXC` | found | The `dxc` that makes DXIL. It must have `dxil.dll` beside it, as the Windows SDK's does. |
| `LUDIFEX_DXC_SPIRV` | found | The `dxc` that makes SPIR-V, usually the Vulkan SDK's. |
| `LUDIFEX_SPIRV_CROSS` | found | `spirv-cross`, which turns SPIR-V into MSL for Metal. |

## What gets fetched

ludifex fetches SDL3 `release-3.4.16`, Box2D `v3.1.1`, Box3D `v0.1.0`, cgltf
`v1.15`, stb (at a pinned commit), miniaudio `0.11.25`, and Assimp `v6.0.5`
(importers only). Their licenses are reproduced in
[`THIRD_PARTY_NOTICES.md`](https://github.com/cresmarmat-an/ludifex/blob/main/THIRD_PARTY_NOTICES.md);
ship that file with your program.

Two of them are patched while configuring. A module in `cmake/` writes a
corrected copy of the affected file into the build directory, and the fetched
source is left untouched:

- `PatchMiniaudio.cmake`: miniaudio's gain smoothing ran at half speed and its
  seeking replayed old audio, and both caused clicks.
- `PatchBox3D.cmake`: Box3D's cone joint limit could add energy to a body
  swinging around the edge of its cone.

If a later version changes the code a patch applies to, configuration stops
with a message instead of building the unpatched code.

Shaders are compiled and embedded into the library when it builds, so a
program built with ludifex has no shader files to ship. On MSVC, Box3D's own
build selects the static C runtime; ludifex switches it back to the dynamic
runtime so your program has one heap.

## Checking the version

`<ludifex/version.h>` is generated from the version in `CMakeLists.txt` and
included by `<ludifex/ludifex.h>`:

```cpp
std::printf("ludifex %s\n", ludifex::VersionString);   // also VersionMajor, VersionMinor, VersionPatch

#if LUDIFEX_VERSION_MAJOR == 0
// ...
#endif
```

What changed between versions is in
[the changelog](https://github.com/cresmarmat-an/ludifex/blob/main/CHANGELOG.md).

## Limitations

- Only CMake is supported. There is no package for vcpkg, Conan, or other
  package managers yet.
- The first configure downloads and builds several dependencies, including
  Assimp, which takes a while. Later configures reuse the download.
- SDL3 is always built as a shared library and has to ship beside your
  program.
- Prebuilt release zips are for Windows x64 only.
