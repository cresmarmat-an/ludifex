// ludifex's lighting and image effects: the procedural sky, ambient occlusion,
// bloom, automatic exposure, tone mapping with colour grading, and the last
// scale to the target's size.
//
// Each entry point is compiled on its own and drawn with the full-screen
// triangle from post.hlsl. They share one uniform block and three texture
// slots, whose meaning each pass spells out above it.

#include "sky_model.hlsli"

struct PostInput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0; // (0, 0) at the top left
};

#ifdef __spirv__
#define LUDIFEX_SAMPLED(slot, set) [[vk::combinedImageSampler]] [[vk::binding(slot, set)]]
#else
#define LUDIFEX_SAMPLED(slot, set)
#endif

LUDIFEX_SAMPLED(0, 2) Texture2D<float4> Input0 : register(t0, space2);
LUDIFEX_SAMPLED(0, 2) SamplerState Sampler0 : register(s0, space2);
LUDIFEX_SAMPLED(1, 2) Texture2D<float4> Input1 : register(t1, space2);
LUDIFEX_SAMPLED(1, 2) SamplerState Sampler1 : register(s1, space2);
LUDIFEX_SAMPLED(2, 2) Texture2D<float4> Input2 : register(t2, space2);
LUDIFEX_SAMPLED(2, 2) SamplerState Sampler2 : register(s2, space2);

// Mirrors EffectUniforms in the renderer.
cbuffer EffectUniforms : register(b0, space3)
{
    float4x4 ViewProjection;
    float4x4 InverseViewProjection;
    float4   TexelSize; // xy one texel of the input, zw the target's size in pixels
    float4   Params;
    float4   Extra[6];
};

static const float Pi = 3.14159265358979;

float Luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

float3 LinearToSrgb(float3 color)
{
    color = saturate(color);
    float3 low = color * 12.92;
    float3 high = 1.055 * pow(color, 1.0 / 2.4) - 0.055;
    return lerp(high, low, color <= 0.0031308);
}

// Where a pixel of the screen is in the world, from its depth.
float3 WorldFromDepth(float2 uv, float depth)
{
    float4 world = mul(InverseViewProjection, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
    return world.xyz / world.w;
}

// --- sky ------------------------------------------------------------------------
//
// Drawn first in the scene pass, where nothing else is: the view direction of
// each pixel, looked up in the sky model.
// Extra: [0] toward the scene from the sun, [1] the sun's light, [2] zenith,
// [3] horizon, [4] ground, as sky_model.hlsli takes them.

float4 SkyFragmentMain(PostInput input) : SV_Target
{
    float2 ndc = float2(input.UV.x * 2.0 - 1.0, 1.0 - input.UV.y * 2.0);
    float4 nearPoint = mul(InverseViewProjection, float4(ndc, 0.0, 1.0));
    float4 farPoint = mul(InverseViewProjection, float4(ndc, 1.0, 1.0));
    float3 direction = normalize(farPoint.xyz / farPoint.w - nearPoint.xyz / nearPoint.w);

    float3 sky = SkyModel(direction, true, Extra[2], Extra[3], Extra[4], normalize(-Extra[0].xyz), Extra[1].rgb);
    return float4(sky, 1.0);
}

// --- ambient occlusion ------------------------------------------------------------
//
// For each pixel, points are scattered through the hemisphere above its
// surface, out to Radius; each one the depth buffer shows to be buried inside
// something counts against the light reaching it. A point hidden by something
// far in front of the surface (a pole a metre closer to the camera, say)
// counts for less, so distant silhouettes do not cast dark halos.
//
// Input0 the prepass depth, Input1 its normals. Params: x radius, y
// intensity, z samples. Extra: [0] the camera's position, [1] its forward
// direction.

float ViewDepth(float3 world)
{
    return dot(world - Extra[0].xyz, Extra[1].xyz);
}

// A value from 0 to 1 that changes from one pixel to the next in a pattern a
// small blur averages away cleanly (Jimenez's interleaved gradient noise).
float InterleavedGradientNoise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

void Basis(float3 normal, out float3 tangent, out float3 bitangent)
{
    float flip = normal.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (flip + normal.z);
    float b = normal.x * normal.y * a;
    tangent = float3(1.0 + flip * normal.x * normal.x * a, flip * b, -flip * normal.x);
    bitangent = float3(b, flip + normal.y * normal.y * a, -normal.y);
}

float4 AmbientOcclusionFragmentMain(PostInput input) : SV_Target
{
    float depth = Input0.SampleLevel(Sampler0, input.UV, 0).r;
    if (depth >= 1.0)
    {
        return 1.0;
    }

    float3 position = WorldFromDepth(input.UV, depth);
    float3 normal = normalize(Input1.SampleLevel(Sampler1, input.UV, 0).xyz);
    float centre = ViewDepth(position);

    float radius = Params.x;
    uint samples = max(uint(Params.z), 1u);
    float noise = InterleavedGradientNoise(input.Position.xy);

    float3 tangent;
    float3 bitangent;
    Basis(normal, tangent, bitangent);

    // Lifted a hair off the surface, in proportion to the distance, so a flat
    // floor does not occlude itself through depth precision.
    float3 origin = position + normal * (0.002 * centre + 0.01 * radius);

    float occluded = 0.0;
    for (uint index = 0; index < samples; ++index)
    {
        // A golden-angle spiral, turned differently at each pixel: cosine
        // weighted, so directions near the normal, which matter most to a
        // surface's lighting, are sampled most.
        float along = (float(index) + 0.5) / float(samples);
        float angle = float(index) * 2.39996323 + noise * 2.0 * Pi;
        float spread = sqrt(along);
        float3 direction = (tangent * cos(angle) + bitangent * sin(angle)) * spread + normal * sqrt(1.0 - along);

        // Nearer points more often than far ones: close creases are what the
        // eye reads as contact.
        float reach = frac(along * 7.31 + noise);
        reach = lerp(0.1, 1.0, reach * reach);
        float3 target = origin + direction * radius * reach;

        float4 clip = mul(ViewProjection, float4(target, 1.0));
        float2 uv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
        if (any(uv < 0.0) || any(uv > 1.0))
        {
            continue;
        }

        float sceneDepth = Input0.SampleLevel(Sampler0, uv, 0).r;
        float scene = ViewDepth(WorldFromDepth(uv, sceneDepth));
        float wanted = ViewDepth(target);
        if (scene < wanted - 0.02 * radius)
        {
            occluded += smoothstep(0.0, 1.0, radius / max(abs(centre - scene), 1e-4));
        }
    }

    float light = saturate(1.0 - Params.y * occluded / float(samples));
    return float4(light, light, light, 1.0);
}

// Smooths the occlusion along one axis without smearing it across an edge:
// a neighbour at a very different depth (the wall behind a pillar) barely
// counts.
//
// Input0 the occlusion, Input1 the prepass depth. Params: xy one step in
// texture units along the axis, z the near plane, w the far plane.

float LinearDepth(float depth)
{
    return Params.z * Params.w / (Params.w - depth * (Params.w - Params.z));
}

float4 OcclusionBlurFragmentMain(PostInput input) : SV_Target
{
    float centre = LinearDepth(Input1.SampleLevel(Sampler1, input.UV, 0).r);

    float total = 0.0;
    float weights = 0.0;
    [unroll] for (int step = -4; step <= 4; ++step)
    {
        float2 uv = input.UV + Params.xy * float(step);
        float light = Input0.SampleLevel(Sampler0, uv, 0).g;
        float depth = LinearDepth(Input1.SampleLevel(Sampler1, uv, 0).r);

        float weight = exp(-float(step * step) / 18.0) * exp(-abs(depth - centre) / (0.03 * centre + 1e-3));
        total += light * weight;
        weights += weight;
    }

    float light = total / max(weights, 1e-5);
    return float4(light, light, light, 1.0);
}

// --- bloom ---------------------------------------------------------------------
//
// The bright parts of the image are taken down a chain of ever smaller
// textures and brought back up, each level adding its blur to the one above,
// so the glow is tight near a light and wide far from it (Jimenez's method,
// from Call of Duty: Advanced Warfare).
//
// Input0 the texture being read. TexelSize: one of its texels. Params: x the
// threshold, y the softness of its knee, z the spread when coming back up.

// Keeps what is brighter than the threshold, eased in across the knee rather
// than cut, so the glow does not pop on and off as something brightens.
float3 AboveThreshold(float3 color)
{
    float brightness = max(color.r, max(color.g, color.b));
    float knee = max(Params.y, 1e-4);
    float soft = clamp(brightness - Params.x + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    float contribution = max(soft, brightness - Params.x) / max(brightness, 1e-4);
    return color * contribution;
}

float4 BloomPrefilterFragmentMain(PostInput input) : SV_Target
{
    float2 texel = TexelSize.xy;
    float3 a = Input0.SampleLevel(Sampler0, input.UV + texel * float2(-1.0, -1.0), 0).rgb;
    float3 b = Input0.SampleLevel(Sampler0, input.UV + texel * float2(1.0, -1.0), 0).rgb;
    float3 c = Input0.SampleLevel(Sampler0, input.UV + texel * float2(-1.0, 1.0), 0).rgb;
    float3 d = Input0.SampleLevel(Sampler0, input.UV + texel * float2(1.0, 1.0), 0).rgb;

    // Each quarter weighted down by its own brightness, so one pixel much
    // brighter than its neighbours (a specular sparkle) cannot make the whole
    // glow flicker (Karis average).
    float wa = 1.0 / (1.0 + Luminance(a));
    float wb = 1.0 / (1.0 + Luminance(b));
    float wc = 1.0 / (1.0 + Luminance(c));
    float wd = 1.0 / (1.0 + Luminance(d));
    float3 average = (a * wa + b * wb + c * wc + d * wd) / (wa + wb + wc + wd);

    return float4(AboveThreshold(average), 1.0);
}

float3 Tap(float2 uv, float2 offset)
{
    return Input0.SampleLevel(Sampler0, uv + offset * TexelSize.xy, 0).rgb;
}

float4 BloomDownFragmentMain(PostInput input) : SV_Target
{
    // Thirteen taps as five overlapping boxes, the middle one weighted most:
    // a wide, smooth reduction that does not shimmer as things move.
    float2 uv = input.UV;
    float3 a = Tap(uv, float2(-2.0, -2.0));
    float3 b = Tap(uv, float2(0.0, -2.0));
    float3 c = Tap(uv, float2(2.0, -2.0));
    float3 d = Tap(uv, float2(-1.0, -1.0));
    float3 e = Tap(uv, float2(1.0, -1.0));
    float3 f = Tap(uv, float2(-2.0, 0.0));
    float3 g = Tap(uv, float2(0.0, 0.0));
    float3 h = Tap(uv, float2(2.0, 0.0));
    float3 i = Tap(uv, float2(-1.0, 1.0));
    float3 j = Tap(uv, float2(1.0, 1.0));
    float3 k = Tap(uv, float2(-2.0, 2.0));
    float3 l = Tap(uv, float2(0.0, 2.0));
    float3 m = Tap(uv, float2(2.0, 2.0));

    float3 result = (d + e + i + j) * 0.125;
    result += (a + b + g + f) * 0.03125;
    result += (b + c + h + g) * 0.03125;
    result += (f + g + l + k) * 0.03125;
    result += (g + h + m + l) * 0.03125;
    return float4(result, 1.0);
}

float4 BloomUpFragmentMain(PostInput input) : SV_Target
{
    // A 3x3 tent, spread by Params.z, added onto the level above by blending.
    float2 uv = input.UV;
    float spread = Params.z;
    float3 result = Tap(uv, float2(-1.0, -1.0) * spread);
    result += Tap(uv, float2(0.0, -1.0) * spread) * 2.0;
    result += Tap(uv, float2(1.0, -1.0) * spread);
    result += Tap(uv, float2(-1.0, 0.0) * spread) * 2.0;
    result += Tap(uv, float2(0.0, 0.0)) * 4.0;
    result += Tap(uv, float2(1.0, 0.0) * spread) * 2.0;
    result += Tap(uv, float2(-1.0, 1.0) * spread);
    result += Tap(uv, float2(0.0, 1.0) * spread) * 2.0;
    result += Tap(uv, float2(1.0, 1.0) * spread);
    return float4(result / 16.0, 1.0);
}

// --- automatic exposure ----------------------------------------------------------
//
// The scene's brightness, as the average of its logarithm: a few bright
// highlights move it far less than a plain average would. It is written into a
// small texture whose smallest mip level is that average, and the exposure
// eases toward what it calls for.

// Input0 the scene.
float4 LuminanceFragmentMain(PostInput input) : SV_Target
{
    float2 texel = TexelSize.xy;
    float3 a = Input0.SampleLevel(Sampler0, input.UV + texel * float2(-0.5, -0.5), 0).rgb;
    float3 b = Input0.SampleLevel(Sampler0, input.UV + texel * float2(0.5, -0.5), 0).rgb;
    float3 c = Input0.SampleLevel(Sampler0, input.UV + texel * float2(-0.5, 0.5), 0).rgb;
    float3 d = Input0.SampleLevel(Sampler0, input.UV + texel * float2(0.5, 0.5), 0).rgb;
    float luminance = Luminance(a + b + c + d) * 0.25;
    return float4(log2(max(luminance, 1e-4)), 0.0, 0.0, 1.0);
}

// Input0 the logarithms, read at their smallest level; Input1 last frame's
// exposure. Params: x that level, y how far to move this frame (1 on the
// first), z and w the lowest and highest exposure. Extra[0].x the brightness
// the eye settles the average at.
float4 AdaptFragmentMain(PostInput input) : SV_Target
{
    float average = exp2(Input0.SampleLevel(Sampler0, float2(0.5, 0.5), Params.x).r);
    float wanted = clamp(Extra[0].x / max(average, 1e-4), Params.z, Params.w);
    float previous = max(Input1.SampleLevel(Sampler1, float2(0.5, 0.5), 0).r, 1e-4);

    // Eased in stops rather than in brightness, the way an eye adapts. The
    // first frame has no previous exposure worth reading.
    float exposure = Params.y >= 1.0 ? wanted : exp2(lerp(log2(previous), log2(wanted), saturate(Params.y)));
    return float4(exposure, average, 0.0, 1.0);
}

// --- tone mapping and grading ------------------------------------------------------
//
// Input0 the scene in linear light, Input1 the bloom, Input2 the automatic
// exposure. Params: x the exposure, y the bloom's intensity, z 1 to apply the
// automatic exposure, w the curve (0 neutral, 1 filmic). Extra: [0] white
// balance, w contrast; [1] lift, w saturation; [2] gain, w vignette.

// Identity below the knee and a smooth roll-off to 1 above it, applied to the
// brightest channel so hue is kept. An ordinary scene, including the sky
// colour you chose, comes out as authored; only highlights brighter than the
// display can show are compressed, and they soften instead of clipping.
float3 NeutralCurve(float3 color)
{
    const float knee = 0.9;
    const float headroom = 1.0 - knee;

    float peak = max(max(color.r, color.g), color.b);
    if (peak <= knee)
    {
        return color;
    }

    float excess = peak - knee;
    float mapped = knee + headroom * (excess / (excess + headroom));
    return color * (mapped / peak);
}

// Narkowicz's fit to the ACES reference curve.
float3 FilmicCurve(float3 color)
{
    color *= 0.8;
    return saturate((color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14));
}

float4 ToneMapFragmentMain(PostInput input) : SV_Target
{
    float3 color = Input0.SampleLevel(Sampler0, input.UV, 0).rgb;
    color += Input1.SampleLevel(Sampler1, input.UV, 0).rgb * Params.y;

    float exposure = Params.x;
    if (Params.z > 0.5)
    {
        exposure *= Input2.SampleLevel(Sampler2, float2(0.5, 0.5), 0).r;
    }
    color *= exposure * Extra[0].rgb;

    color = Params.w > 0.5 ? FilmicCurve(color) : NeutralCurve(color);

    // Grading happens on the encoded image, where contrast, lift, and gain
    // behave the way a colourist expects.
    float3 display = LinearToSrgb(color);
    display = (display - 0.5) * Extra[0].w + 0.5;
    display = lerp(Luminance(display).xxx, display, Extra[1].w);
    display = display * Extra[2].rgb + Extra[1].rgb * (1.0 - display);

    float2 fromCentre = (input.UV - 0.5) * 1.41421356;
    display *= 1.0 - Extra[2].w * smoothstep(0.3, 1.0, dot(fromCentre, fromCentre));

    return float4(saturate(display), 1.0);
}

// --- scaling to the target ---------------------------------------------------------
//
// The finished image, drawn at the render scale, filtered up or down to the
// target's size. Scaling up sharpens a little, which gives back some of what
// the lower resolution softened.
//
// Input0 the finished image. TexelSize: one of its texels. Params.x the
// sharpening.

float4 UpscaleFragmentMain(PostInput input) : SV_Target
{
    float3 centre = Input0.SampleLevel(Sampler0, input.UV, 0).rgb;
    if (Params.x <= 0.0)
    {
        return float4(centre, 1.0);
    }

    float2 texel = TexelSize.xy;
    float3 around = Input0.SampleLevel(Sampler0, input.UV + float2(0.0, -texel.y), 0).rgb +
                    Input0.SampleLevel(Sampler0, input.UV + float2(0.0, texel.y), 0).rgb +
                    Input0.SampleLevel(Sampler0, input.UV + float2(-texel.x, 0.0), 0).rgb +
                    Input0.SampleLevel(Sampler0, input.UV + float2(texel.x, 0.0), 0).rgb;
    float3 sharpened = centre + (centre * 4.0 - around) * 0.25 * Params.x;
    return float4(saturate(sharpened), 1.0);
}
