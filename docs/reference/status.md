# Capabilities and limitations

This page lists what ludifex 0.0.1 can do and what it cannot, in one place.
Each topic page has a Limitations section with more detail.

## What ludifex can do

| Area | Capabilities |
|---|---|
| Worlds | 3D and 2D worlds, several at once. Fixed-step simulation with smooth interpolation, pausing, manual stepping, and deterministic mode. See [Creating a world](../worlds/creating-a-world.md) and [Stepping the simulation](../worlds/stepping.md). |
| Actors | Boxes, spheres, capsules, ground, and models in 3D; rectangles, circles, capsules, ground, and sprites in 2D. Static, kinematic, and dynamic bodies. Handles that stay safe after the actor is destroyed. See [Adding actors](../worlds/adding-actors.md). |
| Structure | A transform hierarchy that moves children with their parents, and saving a 3D world to bytes or a file and restoring it. See [The transform hierarchy](../worlds/transform-hierarchy.md) and [Saving and restoring](../worlds/saving-and-restoring.md). |
| Physics | Collision events with contact point and impact speed, sensors with enter and leave events, 64 collision layers, and multithreaded stepping. See [Collision events](../physics/collision-events.md) and [Sensors and triggers](../physics/sensors-and-triggers.md). |
| Joints | Hinge, slider, distance, weld, and cone joints with limits, motors, and springs; a wheel joint in 2D; humanoid ragdolls. See [Joints](../physics/joints.md). |
| Queries | Ray casts, shape casts, overlap tests, and picking from the screen, in 3D and 2D. See [Queries](../physics/queries.md). |
| Characters | A capsule character controller that climbs steps, respects a slope limit, and slides along walls. See [Characters](../physics/characters.md). |
| Graphics | Direct3D 12, Vulkan, and Metal through SDL3's GPU API, on its own window or inside opane. See [Rendering a world](../rendering/overview.md) and [Graphics backends](../rendering/graphics-backends.md). |
| Lighting | Metallic-roughness shading in linear light, a sun with up to four shadow cascades, up to 256 point lights, fog, and a procedural sky that lights the scene. See [Light, shadow, and fog](../rendering/light-shadow-and-fog.md) and [The sky](../rendering/sky.md). |
| Surfaces | Colour, normal, metallic-roughness, occlusion, and emissive maps, translucency, mip maps, and anisotropic filtering. See [Surfaces and textures](../rendering/surfaces-and-textures.md). |
| Image quality | MSAA, FXAA, and TAA; screen-space ambient occlusion; bloom; automatic exposure; neutral and filmic tone curves with grading; render scale from a quarter to twice the output; level of detail; quality presets from Potato to Extreme. See [Graphics quality](../rendering/graphics-quality.md). |
| Ray tracing | Traced shadows, ambient occlusion, and reflections in compute shaders, on any supported GPU, and a progressive path tracer for stills. See [Ray tracing](../rendering/ray-tracing.md) and [Path tracing](../rendering/path-tracing.md). |
| Custom shaders | HLSL fragment shaders on any actor with per-actor values, full-screen post-process shaders, hot reload during development, and embedded bytecode for shipping. See [Custom shaders](../rendering/custom-shaders.md). |
| Models | glTF, and FBX, OBJ, Collada, and the other formats Assimp reads, with their materials and textures, loaded in the foreground or in the background. See [Model formats](../assets/model-formats.md) and [Loading without stopping](../assets/loading-without-stopping.md). |
| Animation | Skeletal animation, several skins per model, morph targets, crossfading between up to four clips, scrubbing, and reading joints in world space. See [Animation](../animation/animation.md). |
| Sound | Positional sound at a point or on an actor, distance falloff, directional cones, Doppler shift, occlusion, a voice limit with priorities, streamed music, and manual output for tests. See [Sound](../sound/sound.md). |
| Development | Reloading models and textures while the program runs, sharing identical files, debug drawing, a profiler, worker statistics, and validation of every input. See [Reloading while it runs](../assets/reloading-while-it-runs.md) and [Measuring a frame](../tools/measuring-a-frame.md). |
| Native access | The raw Box2D and Box3D handles for features ludifex does not wrap. See [Native access](../tools/native-access.md). |

## What ludifex does not do

**Platforms**

- This release has been tested on Windows only, with Direct3D 12 and Vulkan.
  Linux (Vulkan) and macOS (Metal) builds are supported by the build scripts
  but have not been run on real hardware.
- There is no OpenGL, WebGPU, mobile, or web support.
- There is no editor. Worlds are built in C++.

**Worlds and physics**

- A world must be used from one thread at a time.
- Colliders are boxes, spheres, and capsules (rectangles, circles, and
  capsules in 2D). Models collide as a box around their bounds. There are no
  convex hull, triangle mesh, height field, or compound colliders, and no
  polygon or chain shapes in 2D.
- Actors cannot be resized after they are created.
- There is no damping, gravity scale, or axis locking on actors, except
  through [native access](../tools/native-access.md).
- Collision events report only the start of a contact, with one contact
  point.
- Queries return only the nearest hit, use unrotated shapes, and cannot be
  filtered by layer.
- Characters are not pushed by bodies or carried by moving platforms.
- Joints do not break on their own, and ragdolls use one fixed humanoid
  layout.
- 2D worlds have no transform hierarchy, saving, or ragdolls.
- Determinism has only been verified on the same build and machine.

**Rendering**

- One sun per world, and only the sun casts shadows. There are no spot
  lights or area lights.
- No baked lighting or real-time global illumination outside the path tracer.
- One perspective camera per 3D world, with no split screen or VR.
- No depth of field, motion blur, lens flares, or volumetric effects.
- No compressed textures, and HDR images are loaded at 8 bits per channel.
- Custom shaders replace the fragment stage only, with up to eight uniforms
  and no extra textures.
- The path tracer has no denoiser and is not fast enough for real-time use.
- 2D worlds have no lighting, shadows, ambient occlusion, or ray tracing.

**Assets and animation**

- Only the first set of texture coordinates is used, and vertex colours are
  ignored.
- glTF extensions other than emissive strength are not read.
- A model can have at most 255 joints.
- No root motion, inverse kinematics, animation state machine, additive
  blending, or per-joint masking.

**Sound**

- Stereo panning only, with no HRTF, reverb, or filtering.
- Voices cannot be paused or have their pitch changed after they start.

## Design notes that affect how you use it

There is no single `Actor` type. ludifex has `Actor3D` and `Actor2D`, so an
actor from one kind of world cannot be passed to the other by mistake; the
compiler catches it.

Actors are handles, not pointers. A handle to a destroyed actor stays safe to
use: calls through it do nothing and log a warning, and `IsValid()` returns
false. See [The mutability contract](../worlds/mutability-contract.md) for
when changes take effect.
