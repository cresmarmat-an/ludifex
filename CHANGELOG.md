# Changelog

All notable changes to ludifex. The version is set in `CMakeLists.txt`, and
`ludifex/version.h` makes it available to a program at compile time.

## 0.0.1

The first release.

### Worlds

- `World3D` (Box3D) and `World2D` (Box2D) with handle-based actors. Using a
  handle to a destroyed actor logs a warning and does nothing.
- Actors can be created and destroyed at any time, including while iterating
  and inside event handlers. Structural changes are applied at sync points.
- Fixed-step simulation with interpolation, pausing, manual stepping, a
  deterministic mode, and multithreaded stepping. Stepping an unchanged world
  makes no heap allocations.
- Validation of sizes, positions, and velocities before they reach the
  physics engine.
- Saving a 3D world to bytes or a file, and restoring it.
- A transform hierarchy for 3D actors. Destroying a parent destroys its
  children.

### Shapes, joints, and queries

- Boxes, spheres, capsules, ground, and models in 3D; rectangles, circles,
  capsules, ground, and sprites in 2D.
- Hinge, slider, distance, weld, and cone joints in 3D, and hinge, slider,
  distance, weld, and wheel joints in 2D, with limits, motors, and springs.
- Ragdolls built from cone joints and hinges with `AddRagdoll`.
- Collision events with contact point and impact speed, sensors with enter and
  leave events, and 64 collision layers, in 2D and 3D.
- Ray casts, picking from the screen, shape casts, and overlap tests, in 2D
  and 3D.
- Capsule character controllers in 2D and 3D that slide along walls, climb
  steps, respect a slope limit, and report the ground.

### Rendering

- Direct3D 12, Vulkan, and Metal through SDL3's GPU API.
- Forward shading in linear light with a metallic-roughness model, a sun with
  up to four shadow cascades, up to 256 clustered point lights, fog, tone
  mapping, translucency, level of detail, frustum culling, and instancing.
- Colour textures with mip maps and anisotropic filtering, and normal,
  metallic-roughness, occlusion, and emissive maps. Normal maps work without
  mesh tangents.
- A procedural sky that lights the scene, screen-space ambient occlusion,
  bloom, automatic exposure, and neutral and filmic tone curves with colour
  grading.
- MSAA, FXAA, and TAA anti-aliasing presets, and a render scale from a
  quarter to twice the output size.
- Graphics quality presets from `Potato` to `Extreme`. A world starts at
  `High`.
- Ray-traced shadows, ambient occlusion, and reflections in compute shaders,
  on any supported GPU, and a progressive path tracer.
- Sprites with layers and sprite-sheet frames, background images, and debug
  lines.
- Custom surface and post-process materials in HLSL, with per-actor values,
  hot reload during development, and `ludifex_add_material` to compile them
  into a program at build time.

### Models and animation

- glTF models through cgltf, and FBX, OBJ, Collada, 3DS, Blender, PLY, STL,
  and the other formats Assimp reads, with their materials and textures.
  `LUDIFEX_ASSIMP=OFF` builds without Assimp.
- Loading models in the background, with a placeholder until they arrive.
- Skeletal animation with several skins per model, morph targets, crossfading
  between up to four clips, scrubbing, and joints readable in world space.
  Skinning and morphing run on the GPU.

### Sound

- Positional sound with distance falloff, directional cones, the Doppler
  shift, occlusion, and a voice limit with priorities. Looping voices that
  lose their slot keep their place and resume.
- Streamed music, WAV, FLAC, MP3, and Ogg Vorbis files, and manual output for
  tests. Level changes are ramped to avoid clicks.

### Assets

- Files are found through ordered asset roots, identical files are loaded
  once, and models and textures can reload while the program runs.

### Building and installing

- Installs itself, SDL3, Box2D, Box3D, and a CMake package for
  `find_package(ludifex)`, with Debug and Release libraries side by side.
- Shaders are written in HLSL and compiled at build time to DXIL, SPIR-V, and
  MSL as the platform needs.
- `ludifex/version.h` reports the version at compile time.
- MIT licensed. The dependencies' licenses are in `THIRD_PARTY_NOTICES.md`.

### Fixes to dependencies

- miniaudio 0.11.25: the gain smoother's ramp and a stale cache after
  seeking, both of which caused clicks (`cmake/PatchMiniaudio.cmake`).
- Box3D 0.1.0: the cone joint's swing axis, which added energy to a body
  moving around the edge of its cone (`cmake/PatchBox3D.cmake`).

### Known limitations

- Custom materials replace the fragment shader only.
- Tested on Windows only. Linux and macOS builds are supported but have not
  been run on real hardware.
