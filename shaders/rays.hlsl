// Hybrid ray tracing: the rasterizer still draws every surface, and for each
// pixel it drew this pass traces what rasterizing cannot know: how much of
// the sun's disc and of the sky is hidden from it, and what it reflects. One
// ray per effect per pixel, at a fraction of the resolution; the
// passes in ray_denoise.hlsl smooth the result over time and across the
// screen, and the surface shader reads it where it lands.
//
// Input0 is the prepass depth, Input1 its normals and roughness.

#include "ray_common.hlsli"

// Red the sun's visibility, green the ambient light's, blue the pixel's depth
// along the view (which the smoothing passes use to tell surfaces apart), and
// alpha one. Then the reflection's colour, and in alpha how much of the
// surface's reflection it should replace.
[[vk::image_format("rgba16f")]] RAY_SLOT(0, 3, 1) RWTexture2D<float4> Lighting : register(u0, space1);
[[vk::image_format("rgba16f")]] RAY_SLOT(1, 4, 1) RWTexture2D<float4> Reflection : register(u1, space1);

// What a reflection ray sees where it lands: the surface lit by the sun,
// with a shadow ray of its own, and by the ambient light. Point lights are
// left out; a reflection is a small part of a pixel, and they are many rays.
float3 ShadeReflected(HitSurface surface, inout uint seed)
{
    if ((surface.Flags & RayFlagUnlit) != 0)
    {
        return surface.Albedo + surface.Emission;
    }

    float3 diffuse = surface.Albedo * (1.0 - surface.Metallic);
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), surface.Albedo, surface.Metallic);
    float3 color = surface.Emission;

    float3 toSun = ConeDirection(ToSun(), SunDirection.w, NextRandom2(seed));
    float facing = dot(surface.Normal, toSun);
    if (facing > 0.0 && any(SunLight.rgb > 0.0))
    {
        if (!Blocked(LeaveSurface(surface.Position, surface.Geometric), toSun, 1e5,
                     RayFlagTransparent | RayFlagUnlit))
        {
            color += (diffuse + f0 * 0.25) * facing * SunLight.rgb;
        }
    }

    color += AmbientFrom(surface.Normal) * (diffuse + f0 * 0.5);
    return color;
}

[numthreads(8, 8, 1)]
void RayHybridMain(uint3 id : SV_DispatchThreadID)
{
    if (any(float2(id.xy) >= Size.xy))
    {
        return;
    }

    float2 uv = (float2(id.xy) + 0.5) * Size.zw;
    float depth = Input0.SampleLevel(Input0Sampler, uv, 0).r;

    // The sky: nothing to shadow, occlude, or reflect.
    if (depth >= 1.0)
    {
        Lighting[id.xy] = float4(1.0, 1.0, Params.z, 1.0);
        Reflection[id.xy] = 0.0;
        return;
    }

    float4 world = mul(InverseViewProjection, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
    float3 position = world.xyz / world.w;
    float4 surface = Input1.SampleLevel(Input1Sampler, uv, 0);
    float3 normal = normalize(surface.xyz);
    float roughness = surface.w;
    float viewDepth = dot(position - CameraPosition.xyz, CameraForward.xyz);

    uint seed = SeedFor(id.xy, Settings.y);
    float3 origin = LeaveSurface(position, normal) + normal * (viewDepth * 1e-3);
    uint effects = Settings.x;

    // The sun: one ray toward a random point of its disc. Averaged over
    // frames and neighbours, the share that got through is the penumbra.
    float sun = 1.0;
    if ((effects & RayEffectShadows) != 0)
    {
        float3 toSun = ConeDirection(ToSun(), SunDirection.w, NextRandom2(seed));
        sun = dot(normal, toSun) <= 0.0 ? 0.0 : (Blocked(origin, toSun, 1e5, RayFlagTransparent | RayFlagUnlit) ? 0.0 : 1.0);
    }

    // The ambient light: one ray over the hemisphere, weighted toward the
    // normal. Something close blocks more than something at the edge of
    // reach, so occlusion fades out rather than stopping at a line.
    float ambient = 1.0;
    if ((effects & RayEffectOcclusion) != 0)
    {
        float3 direction = CosineDirection(normal, NextRandom2(seed));
        RayHit hit;
        if (TraceScene(origin, direction, Params.x, RayFlagTransparent | RayFlagUnlit, false, hit))
        {
            ambient = smoothstep(0.0, 1.0, hit.T / Params.x);
        }
    }

    // Reflections, for surfaces smooth enough to show one: a ray drawn from
    // the surface's glossy lobe, and what it finds. Rougher surfaces fade to
    // the environment the surface shader already has.
    float4 reflected = 0.0;
    if ((effects & RayEffectReflections) != 0 && roughness < Params.y)
    {
        float3 toView = normalize(CameraPosition.xyz - position);
        float3 direction = GlossyDirection(normal, toView, roughness, NextRandom2(seed));
        if (dot(direction, normal) > 0.0)
        {
            RayHit hit;
            float3 color;
            if (TraceScene(origin, direction, 1e5, RayFlagTransparent, false, hit))
            {
                color = ShadeReflected(SurfaceAt(hit, origin, direction), seed);
            }
            else
            {
                color = EnvironmentAlong(direction);
            }
            float trust = 1.0 - smoothstep(Params.y * 0.6, Params.y, roughness);
            reflected = float4(color, trust);
        }
    }

    Lighting[id.xy] = float4(sun, ambient, viewDepth, 1.0);
    Reflection[id.xy] = reflected;

}
