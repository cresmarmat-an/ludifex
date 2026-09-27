# Custom shaders

A **material** is a fragment shader of your own plus up to eight uniform
values. Give one to an actor and only the colour of its pixels changes. The
actor still instances, culls, casts shadows, and moves with physics as before,
because the vertex stage stays ludifex's. Actors in 2D worlds take materials
the same way.

## Writing the shader

Include `world_material.hlsli`. You can compute a colour and keep the built-in
lighting (sun, shadows, point lights, ambient light, and fog) by passing it to
`Shade()`:

```hlsl
#include "world_material.hlsli"

float4 FragmentMain(SurfaceInput input) : SV_Target
{
    // Stripes in the mesh's own space stay on the object as it turns.
    float band = frac(input.LocalPosition.y * Param(input, 2).x + Seconds() * Param(input, 2).y);
    float stripe = AntiAlias(abs(band - 0.5) - 0.25);

    float3 base = lerp(Param(input, 0).rgb, Param(input, 1).rgb, stripe);
    return float4(Shade(input, base), 1.0);
}
```

Or skip the built-in lighting and draw your own look:

```hlsl
float4 FragmentMain(SurfaceInput input) : SV_Target
{
    float rim = Fresnel(input, 2.0);
    return float4(Param(input, 0).rgb * (0.2 + rim * 1.7), 1.0);
}
```

The shader works in linear light. Colours from C++ (the actor's colour and any
`Color` uniform) arrive converted from sRGB, and textures are sampled as
linear. Return linear light. Values above 1 are fine; the tone map rolls them
off.

What `world_material.hlsli` gives you:

| Name | Purpose |
| --- | --- |
| `Param(input, slot)` | A uniform as this actor sees it: the actor's own value if set with `SetUniform`, otherwise the material's |
| `Params[0..7]` | The material's own values, in the order they were declared |
| `input.Color` | The actor's colour, linear |
| `input.UV` | Texture coordinates, with the actor's tiling applied |
| `input.LocalPosition` | Position in the mesh's own space |
| `input.WorldPosition`, `input.WorldNormal` | World-space position and normal |
| `SampleBaseColor(input)`, `BaseColor(input)` | The actor's texture, and the texture times its colour |
| `Shade(input, color)` | The built-in lighting with the actor's roughness and metalness |
| `ShadeSurface(input, color, roughness, metallic)` | The same with the surface values given explicitly |
| `ApplyFog(input, color)` | The scene's fog, for a shader that does its own lighting |
| `Fresnel(input, power)` | 0 facing the viewer, 1 at the silhouette |
| `ToLight()`, `ToCamera(input)` | Directions toward the sun and the camera |
| `SurfaceNormal(input)`, `GeometryNormal(input)` | The normal used for lighting (with any normal map applied), and the mesh's own |
| `SurfaceRoughness(input)`, `SurfaceMetallic(input)` | The actor's values times any metallic-roughness map |
| `SurfaceEmission(input)`, `SurfaceOcclusion(input)` | Emitted light, and how much ambient light reaches the point, including ambient occlusion |
| `SunVisibility(input, normal)` | How much of the sun reaches the point, from the shadow maps or traced |
| `AmbientLight(normal)` | The ambient light a surface facing that way receives: from the sky, or the ambient colour |
| `EnvironmentReflection(input, direction, normal, roughness)` | What a glossy surface reflects: the sky or ambient light, or the traced reflection |
| `SkyRadiance(direction, withSun)` | The sky's colour along a direction |
| `AntiAlias(distance)`, `AntiAliasedStep(edge, v)` | Smooth edges for procedural patterns |
| `Seconds()`, `DeltaSeconds()` | Real time in seconds since materials were first used, and the time since the previous frame |

## Using it

```cpp
ludifex::MaterialId stripes = ludifex::CreateMaterial({
    .ShaderPath = "stripes.hlsl",               // found through the asset roots
    .Uniforms = {
        { "ColorA", ludifex::Color::FromBytes(236, 170, 70) },
        { "ColorB", ludifex::Color::FromBytes(40, 44, 58) },
        { "Pattern", 6.0f, 0.35f },             // Params[2].x and .y
    },
});

crate.SetMaterial(stripes);
ludifex::SetMaterialUniform(stripes, "ColorA", ludifex::Color::FromBytes(255, 90, 90));
crate.SetMaterial({});                           // back to the built-in shader
ludifex::DestroyMaterial(stripes);
```

A uniform given as a `Color` reaches the shader in linear light; one given as
numbers reaches it unchanged. Each uniform is four floats, and unused
components are 0. Materials belong to the process, not to a world, so actors
in several worlds can share one. Setting a uniform name the material did not
declare does nothing and logs a warning. If the shader fails to compile,
`CreateMaterial` returns an invalid id and the compiler's message goes to the
log.

## Per-actor values

Each actor can override any of its material's uniforms for itself, and the
shader reads the result with `Param`:

```cpp
for (int index = 0; index < 64; ++index)
{
    spheres[index].SetMaterial(tinted);
    spheres[index].SetUniform("Tint", Rainbow(index / 64.0f));
    spheres[index].SetUniform("Glow", index % 3 == 0 ? 0.6f : 0.0f);
}
```

The values are stored per object rather than in the material, so actors that
share a material are still drawn together. The 64 spheres above are one draw
call. Changing an actor's material clears its overrides, because the slots mean
something else in another material. `ClearUniforms()` clears them yourself.

## Opaque and transparent materials

A material is opaque by default. When multisampling is on, alpha below 1 is
turned into coverage, so a dissolve or a cutout gets smooth edges without
sorting or blending and still writes depth:

```hlsl
float edge = noise - Param(input, 1).x;                    // below 0 where dissolved
float alpha = saturate(edge / max(fwidth(edge), 1e-4) + 0.5);
clip(alpha - 0.001);                                       // drop what is fully gone
return float4(Shade(input, input.Color.rgb), alpha);
```

The `clip` keeps the cutout working when multisampling is off, since there is
no coverage to turn alpha into then.

For glass, smoke, or a force field, anything you see through, mark the
material transparent. It is then drawn after everything opaque, sorted back to
front, and blended, without writing depth:

```cpp
ludifex::MaterialId shield = ludifex::CreateMaterial({
    .ShaderPath = "shield.hlsl",
    .Transparent = true,
});
```

Edges you draw inside a face, such as a stripe, a ring, or a scan line, are
only smoothed if you use `AntiAlias` or `AntiAliasedStep` instead of `step`.
Anti-aliasing smooths the edges of triangles, not the edges your shader
computes.

## Post-process shaders

A full-screen material includes `world_post.hlsli` instead and reads the frame
so far from `Scene`:

```hlsl
#include "world_post.hlsli"

float4 FragmentMain(PostInput input) : SV_Target
{
    float3 color = SampleScene(input.UV).rgb;
    float distance = length(input.UV - 0.5);
    float vignette = 1.0 - Param(0).x * smoothstep(0.35, 0.95, distance);
    return float4(color * vignette, 1.0);
}
```

| Name | Purpose |
| --- | --- |
| `SampleScene(uv)`, `SampleSceneOffset(uv, pixels)` | The frame so far |
| `SceneDistance(uv)` | The distance to the surface at that point, in world units, for fog or outlines |
| `TexelSize` | One pixel in UV units (`xy`) and the frame's size in pixels (`zw`) |
| `Param(slot)`, `Params[0..7]` | Your values |
| `Luminance(color)`, `LinearToDisplay(c)`, `DisplayToLinear(c)` | Colour helpers |
| `Seconds()`, `AntiAlias(d)` | As in a surface material |

Before tone mapping, `Scene` holds linear light. After it, `Scene` holds the
display-encoded image, so pass a colour uniform through `LinearToDisplay`
before comparing or mixing it there. Where post-process materials run is set
when you add them; see
[Backgrounds and post-processing](backgrounds-and-post-processing.md#post-processing).

## Development and shipping

With `ShaderPath`, the shader is found through the asset roots and compiled
when the material is created. Saving the file recompiles it, and uniform
values are kept. A compile error leaves the last working shader on screen and
sends the compiler's message, with file, line, and column, to the log. Set
`HotReload = false` in the description to stop watching the file.

For a release, compile the shader when your program builds, so no compiler and
no `.hlsl` file has to ship with it. `ludifex_add_material` does this and
embeds the result in your program:

```cmake
ludifex_add_material(my_app shaders/glow.hlsl SYMBOL GlowShader)
```

```cpp
#include "GlowShader.h"

ludifex::MaterialId glow = ludifex::CreateMaterial({
    .Bytecode = GlowShader(),
    .Uniforms = { /* as before */ },
});
```

`GlowShader()` returns a `ludifex::ShaderBytecode` holding the shader compiled
for every [backend](graphics-backends.md) the build targets; the material uses
the one its device runs. The shader is compiled against the
`world_material.hlsli` and `world_post.hlsli` of the ludifex you build with, is
rebuilt when it or those files change, and takes `ENTRY <name>` when the entry
point is not `FragmentMain`. `Bytecode` wins over `ShaderPath` when both are
set, so the same description works for both kinds of build.

### Finding the compiler

A material created from `ShaderPath` is compiled for the backend most likely
to draw it, and again for another backend the first time a device of that kind
draws it. The compilers are looked for when first needed:

| Backend | Looked for, in order |
|---|---|
| Direct3D 12 | `dxc` named by the `LUDIFEX_DXC` environment variable, the `dxc` ludifex was built with, then the newest Windows SDK's |
| Vulkan | a `dxc` with SPIR-V output named by `LUDIFEX_DXC_SPIRV`, the one ludifex was built with, then the Vulkan SDK's (through `VULKAN_SDK`) |
| Metal | the same `dxc`, plus `spirv-cross` named by `LUDIFEX_SPIRV_CROSS`, the one ludifex was built with, or the Vulkan SDK's |

The include files are stored inside the ludifex library and written next to
the shader cache, so they always match the version that runs the result.

## Cost

Actors are batched by material, then texture, then mesh. A material adds one
draw for each distinct mesh and texture it is used with, not one per actor.
The [world-shaders example](https://github.com/cresmarmat-an/opane-ludifex-examples/tree/main/05-world-shaders)
draws sixteen actors with three materials, including shadows and tone mapping,
in eight draw calls.

## Limitations

- Only the fragment stage can be replaced. There are no custom vertex,
  geometry, or compute shaders, so a material cannot move vertices (for
  waves or wind, for example).
- A material has at most eight uniforms of four floats each. There are no
  arrays, matrices, or larger buffers.
- A material cannot bind textures of its own. It can read the actor's texture
  and maps through the helpers above, and anything else has to be computed.
- Compiling from `ShaderPath` at runtime needs `dxc` (and `spirv-cross` for
  Metal) on the machine. Programs that ship without them must use
  `ludifex_add_material`.
- Shaders are written in HLSL only.
- [Ray-traced](ray-tracing.md) reflections and [path-traced](path-tracing.md)
  images see an actor with its plain colour and texture, not its material.
- Shadows are cast from the mesh's shape. A material that cuts holes with
  `clip` does not cut them from the shadow.
- `Seconds()` is real time shared by every material. It keeps running while
  a world is paused and cannot be set per world; pass your own time as a
  uniform if you need that.
