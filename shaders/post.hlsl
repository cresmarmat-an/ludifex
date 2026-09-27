// ludifex's full-screen passes: the backdrop and the final-image anti-aliasing
// filters. Tone mapping and the other image effects are in effects.hlsl. Each
// entry point is compiled on its own, so each one declares only the resources
// it reads.
//
// Every pass draws one triangle that covers the target, which avoids the seam
// a two-triangle quad has along its diagonal.

struct PostInput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0; // (0, 0) at the top left
};

PostInput FullscreenVertexMain(uint vertexId : SV_VertexID)
{
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);

    PostInput output;
    output.Position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    output.UV = uv;
    return output;
}

// --- shared resources ---------------------------------------------------------

// Vulkan and Metal, compiled through SPIR-V, want each texture and its
// sampler in one slot; Direct3D reads the registers.
#ifdef __spirv__
#define LUDIFEX_SAMPLED(slot, set) [[vk::combinedImageSampler]] [[vk::binding(slot, set)]]
#else
#define LUDIFEX_SAMPLED(slot, set)
#endif

LUDIFEX_SAMPLED(0, 2) Texture2D<float4> Source : register(t0, space2);
LUDIFEX_SAMPLED(0, 2) SamplerState SourceSampler : register(s0, space2);

// x, y: one texel in UV units; z, w: the target size in pixels.
// Params' meaning depends on the pass.
cbuffer PostUniforms : register(b0, space3)
{
    float4 TexelSize;
    float4 Params;
};

float Luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

// --- backdrop -----------------------------------------------------------------

// An image behind the scene, cropped to cover the view without stretching.
// Params.xy scale and Params.zw offset the texture coordinates; the renderer
// works them out from the image's shape and the view's.
float4 BackdropFragmentMain(PostInput input) : SV_Target
{
    float2 uv = input.UV * Params.xy + Params.zw;
    return float4(Source.Sample(SourceSampler, uv).rgb, 1.0);
}

// --- FXAA ---------------------------------------------------------------------

// Level 7 of the anti-aliasing policy, the fast preset. Finds the local edge
// direction from the luminance of the four diagonal neighbours and blends
// along it, which smooths a stair-stepped edge without softening flat areas.
float4 FxaaFragmentMain(PostInput input) : SV_Target
{
    const float ReduceMinimum = 1.0 / 128.0;
    const float ReduceMultiplier = 1.0 / 8.0;
    const float SpanMaximum = 8.0;

    float2 texel = TexelSize.xy;
    float2 uv = input.UV;

    float3 northWest = Source.SampleLevel(SourceSampler, uv + float2(-1.0, -1.0) * texel, 0).rgb;
    float3 northEast = Source.SampleLevel(SourceSampler, uv + float2(1.0, -1.0) * texel, 0).rgb;
    float3 southWest = Source.SampleLevel(SourceSampler, uv + float2(-1.0, 1.0) * texel, 0).rgb;
    float3 southEast = Source.SampleLevel(SourceSampler, uv + float2(1.0, 1.0) * texel, 0).rgb;
    float3 middle = Source.SampleLevel(SourceSampler, uv, 0).rgb;

    float lumaNW = Luminance(northWest);
    float lumaNE = Luminance(northEast);
    float lumaSW = Luminance(southWest);
    float lumaSE = Luminance(southEast);
    float lumaM = Luminance(middle);

    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    float2 direction;
    direction.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    direction.y = ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float reduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * ReduceMultiplier), ReduceMinimum);
    float inverseSmallest = 1.0 / (min(abs(direction.x), abs(direction.y)) + reduce);
    direction = clamp(direction * inverseSmallest, -SpanMaximum, SpanMaximum) * texel;

    float3 nearBlend = 0.5 * (Source.SampleLevel(SourceSampler, uv + direction * (1.0 / 3.0 - 0.5), 0).rgb +
                              Source.SampleLevel(SourceSampler, uv + direction * (2.0 / 3.0 - 0.5), 0).rgb);
    float3 farBlend = nearBlend * 0.5 + 0.25 * (Source.SampleLevel(SourceSampler, uv + direction * -0.5, 0).rgb +
                                                Source.SampleLevel(SourceSampler, uv + direction * 0.5, 0).rgb);

    // The wider blend is used unless it strays outside the local range, which
    // would mean it crossed into a different feature.
    float lumaFar = Luminance(farBlend);
    float3 result = (lumaFar < lumaMin || lumaFar > lumaMax) ? nearBlend : farBlend;
    return float4(result, 1.0);
}

// --- TAA ----------------------------------------------------------------------

// Level 7, the temporal preset. Every frame the camera is jittered by a
// different sub-pixel amount; blending each frame into a history reprojected
// through the motion vectors accumulates many samples per pixel over time.
// Clamping the history to the current frame's neighbourhood is what stops a
// moving object leaving a ghost behind it.

LUDIFEX_SAMPLED(1, 2) Texture2D<float4> History : register(t1, space2);
LUDIFEX_SAMPLED(1, 2) SamplerState HistorySampler : register(s1, space2);
LUDIFEX_SAMPLED(2, 2) Texture2D<float2> Motion : register(t2, space2);
LUDIFEX_SAMPLED(2, 2) SamplerState MotionSampler : register(s2, space2);

float3 ToYCoCg(float3 color)
{
    return float3(0.25 * color.r + 0.5 * color.g + 0.25 * color.b,
                  0.5 * color.r - 0.5 * color.b,
                  -0.25 * color.r + 0.5 * color.g - 0.25 * color.b);
}

float3 FromYCoCg(float3 color)
{
    return float3(color.x + color.y - color.z, color.x + color.z, color.x - color.y - color.z);
}

// Params.x is how much of the current frame to take: 1 when there is no usable
// history (the first frame, or after a resize), the steady-state blend
// otherwise.
float4 TaaFragmentMain(PostInput input) : SV_Target
{
    float2 uv = input.UV;
    float2 texel = TexelSize.xy;

    float3 current = Source.SampleLevel(SourceSampler, uv, 0).rgb;

    // The neighbourhood's colour range, in a space where a box around the
    // samples hugs them more tightly than in RGB.
    float3 minimum = ToYCoCg(current);
    float3 maximum = minimum;
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            float3 sample = ToYCoCg(Source.SampleLevel(SourceSampler, uv + float2(x, y) * texel, 0).rgb);
            minimum = min(minimum, sample);
            maximum = max(maximum, sample);
        }
    }

    float2 previousUV = uv - Motion.SampleLevel(MotionSampler, uv, 0);
    float currentWeight = Params.x;
    if (any(previousUV < 0.0) || any(previousUV > 1.0))
    {
        currentWeight = 1.0;
    }

    float3 history = History.SampleLevel(HistorySampler, previousUV, 0).rgb;
    history = FromYCoCg(clamp(ToYCoCg(history), minimum, maximum));

    // Weighting by inverse brightness keeps one bright sample from dominating
    // the blend and flickering.
    float currentFactor = currentWeight / (1.0 + Luminance(current));
    float historyFactor = (1.0 - currentWeight) / (1.0 + Luminance(history));
    float3 result = (current * currentFactor + history * historyFactor) / max(currentFactor + historyFactor, 1e-5);

    return float4(result, 1.0);
}
