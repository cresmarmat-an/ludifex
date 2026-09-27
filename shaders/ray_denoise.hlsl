// Smoothing one ray a pixel into something that can be shown.
//
// A single ray says only "blocked" or "not"; the answer a pixel wants is the
// share of rays that would be. These passes find it the way a camera gathers
// light: over time, by blending each frame into a history carried along with
// the motion of the surfaces, and across the screen, by averaging neighbours
// that lie on the same surface. Neighbours at another depth or facing another
// way are left out, so a shadow does not bleed across an edge.
//
// Both passes read six textures and write two, with the rgba of each written
// pair as rays.hlsl lays it out: lighting (sun, ambient, depth, and here how
// many frames the history holds), then reflection.

#include "ray_uniforms.hlsli"

#if defined(__spirv__) && defined(LUDIFEX_METAL)
#define DENOISE_SLOT(vulkan, metal, set) [[vk::binding(metal, set)]]
#elif defined(__spirv__)
#define DENOISE_SLOT(vulkan, metal, set) [[vk::binding(vulkan, set)]]
#else
#define DENOISE_SLOT(vulkan, metal, set)
#endif

#ifdef __spirv__
#define DENOISE_COMBINED [[vk::combinedImageSampler]]
#else
#define DENOISE_COMBINED
#endif

DENOISE_COMBINED DENOISE_SLOT(0, 0, 0) Texture2D<float4> Input0 : register(t0, space0);
DENOISE_COMBINED DENOISE_SLOT(0, 0, 0) SamplerState Sampler0 : register(s0, space0);
DENOISE_COMBINED DENOISE_SLOT(1, 1, 0) Texture2D<float4> Input1 : register(t1, space0);
DENOISE_COMBINED DENOISE_SLOT(1, 1, 0) SamplerState Sampler1 : register(s1, space0);
DENOISE_COMBINED DENOISE_SLOT(2, 2, 0) Texture2D<float4> Input2 : register(t2, space0);
DENOISE_COMBINED DENOISE_SLOT(2, 2, 0) SamplerState Sampler2 : register(s2, space0);
DENOISE_COMBINED DENOISE_SLOT(3, 3, 0) Texture2D<float4> Input3 : register(t3, space0);
DENOISE_COMBINED DENOISE_SLOT(3, 3, 0) SamplerState Sampler3 : register(s3, space0);
DENOISE_COMBINED DENOISE_SLOT(4, 4, 0) Texture2D<float4> Input4 : register(t4, space0);
DENOISE_COMBINED DENOISE_SLOT(4, 4, 0) SamplerState Sampler4 : register(s4, space0);
DENOISE_COMBINED DENOISE_SLOT(5, 5, 0) Texture2D<float4> Input5 : register(t5, space0);
DENOISE_COMBINED DENOISE_SLOT(5, 5, 0) SamplerState Sampler5 : register(s5, space0);

[[vk::image_format("rgba16f")]] DENOISE_SLOT(0, 6, 1) RWTexture2D<float4> LightingOut : register(u0, space1);
[[vk::image_format("rgba16f")]] DENOISE_SLOT(1, 7, 1) RWTexture2D<float4> ReflectionOut : register(u1, space1);

// --- over time -----------------------------------------------------------------------
//
// Input0 and Input1 this frame's rays, Input2 and Input3 the history, Input4
// the screen motion. Params.w the most frames a history may hold: more is
// smoother, and slower to follow a moving shadow.

[numthreads(8, 8, 1)]
void RayTemporalMain(uint3 id : SV_DispatchThreadID)
{
    if (any(float2(id.xy) >= Size.xy))
    {
        return;
    }

    float2 uv = (float2(id.xy) + 0.5) * Size.zw;
    float4 lighting = Input0.SampleLevel(Sampler0, uv, 0);
    float4 reflection = Input1.SampleLevel(Sampler1, uv, 0);

    // Where this point was last frame, and whether what is there now is the
    // same surface: in view, and at the depth it had. Anything else starts
    // its history again.
    float2 previous = uv - Input4.SampleLevel(Sampler4, uv, 0).xy;
    float4 history = Input2.SampleLevel(Sampler2, previous, 0);
    float4 historyReflection = Input3.SampleLevel(Sampler3, previous, 0);

    bool sky = lighting.b >= Params.z * 0.999;
    bool same = (Settings.x & RayEffectRestart) == 0 && all(previous >= 0.0) && all(previous <= 1.0) &&
                abs(history.b - lighting.b) < 0.05 * lighting.b + 0.02 && !sky;

    float frames = same ? min(history.a + 1.0, Params.w) : 1.0;
    float blend = 1.0 / frames;

    LightingOut[id.xy] = float4(lerp(history.rg, lighting.rg, blend), lighting.b, frames);

    // Reflections move across a surface as the camera does, so their history
    // is kept short.
    ReflectionOut[id.xy] = same ? lerp(historyReflection, reflection, max(blend, 0.2)) : reflection;
}

// --- across the screen ------------------------------------------------------------------
//
// One step of an a-trous filter: a 5x5 neighbourhood with gaps of Progress.w
// pixels between its taps, so two steps of 1 and 2 cover a 13x13 area for
// the cost of two 5x5s. Input0 and Input1 what to smooth, Input2 the
// prepass's normals and roughness.

static const float Kernel[3] = { 0.375, 0.25, 0.0625 };

[numthreads(8, 8, 1)]
void RayFilterMain(uint3 id : SV_DispatchThreadID)
{
    if (any(float2(id.xy) >= Size.xy))
    {
        return;
    }

    float2 uv = (float2(id.xy) + 0.5) * Size.zw;
    float4 centre = Input0.SampleLevel(Sampler0, uv, 0);
    float4 centreReflection = Input1.SampleLevel(Sampler1, uv, 0);

    if (centre.b >= Params.z * 0.999)
    {
        LightingOut[id.xy] = centre;
        ReflectionOut[id.xy] = centreReflection;
        return;
    }

    float4 surface = Input2.SampleLevel(Sampler2, uv, 0);
    float3 normal = normalize(surface.xyz);
    float roughness = surface.w;
    float step = float(max(Progress.w, 1u));

    // A mirror's reflection must stay sharp; a satin one may spread.
    float reflectionSpread = 0.5 + roughness * 6.0;

    float2 lightingTotal = 0.0;
    float lightingWeight = 0.0;
    float4 reflectionTotal = 0.0;
    float reflectionWeight = 0.0;

    [unroll] for (int y = -2; y <= 2; ++y)
    {
        [unroll] for (int x = -2; x <= 2; ++x)
        {
            float2 at = uv + float2(x, y) * step * Size.zw;
            float4 lighting = Input0.SampleLevel(Sampler0, at, 0);
            float4 reflection = Input1.SampleLevel(Sampler1, at, 0);
            float3 otherNormal = Input2.SampleLevel(Sampler2, at, 0).xyz;

            float weight = Kernel[abs(x)] * Kernel[abs(y)];
            weight *= exp(-abs(lighting.b - centre.b) / (0.02 * centre.b + 0.01));
            weight *= pow(saturate(dot(normalize(otherNormal), normal)), 16.0);

            lightingTotal += lighting.rg * weight;
            lightingWeight += weight;

            float distance2 = float(x * x + y * y) * step * step;
            float glossy = weight * exp(-distance2 / (2.0 * reflectionSpread * reflectionSpread));
            reflectionTotal += reflection * glossy;
            reflectionWeight += glossy;
        }
    }

    LightingOut[id.xy] = float4(lightingTotal / max(lightingWeight, 1e-5), centre.b, centre.a);
    ReflectionOut[id.xy] = reflectionTotal / max(reflectionWeight, 1e-5);
}
