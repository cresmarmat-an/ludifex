// ludifex post-process contract.
//
// Include this from a full-screen material and hand the material to
// World3D::AddPostProcess or World2D::AddPostProcess. The engine draws one
// triangle covering the frame and runs your FragmentMain once per pixel, with
// the frame so far in Scene.
//
//   #include "world_post.hlsli"
//
//   float4 FragmentMain(PostInput input) : SV_Target
//   {
//       float4 color = SampleScene(input.UV);
//       float vignette = 1.0 - Param(0).x * length(input.UV - 0.5);
//       return float4(color.rgb * vignette, 1.0);
//   }
//
// Where the pass runs decides what Scene holds. Before tone mapping it is
// linear light, possibly brighter than 1. After tone mapping it is the final
// image, encoded for the display, and a colour uniform you compare against it
// should go through LinearToDisplay first.
//
// SceneDistance gives how far from the camera the surface under a pixel is,
// in world units, so fog, outlines, and depth of field can be written here.

#ifndef LUDIFEX_WORLD_POST_HLSLI
#define LUDIFEX_WORLD_POST_HLSLI

struct PostInput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0; // (0, 0) at the top left
};

// One source serves Direct3D 12, Vulkan, and Metal. The registers are what
// Direct3D reads; for Vulkan and Metal, which are compiled through SPIR-V, a
// texture and its sampler also share one slot as a combined image sampler.
#ifndef LUDIFEX_BINDINGS
#define LUDIFEX_BINDINGS
#ifdef __spirv__
#define LUDIFEX_SAMPLED(slot, set) [[vk::combinedImageSampler]] [[vk::binding(slot, set)]]
#else
#define LUDIFEX_SAMPLED(slot, set)
#endif
#if defined(__spirv__) && defined(LUDIFEX_METAL)
#define LUDIFEX_STORAGE(metalSlot) [[vk::binding(metalSlot, 7)]]
#else
#define LUDIFEX_STORAGE(metalSlot)
#endif
#endif

LUDIFEX_SAMPLED(0, 2) Texture2D<float4> Scene : register(t0, space2);
LUDIFEX_SAMPLED(0, 2) SamplerState SceneSampler : register(s0, space2);

LUDIFEX_SAMPLED(1, 2) Texture2D<float> SceneDepth : register(t1, space2);
LUDIFEX_SAMPLED(1, 2) SamplerState SceneDepthSampler : register(s1, space2);

cbuffer PostUniforms : register(b0, space3)
{
    float4 TexelSize;    // x, y: one pixel in UV units; z, w: the frame's size in pixels
    float4 DepthParams;  // x near plane, y far plane, z 1 for an orthographic camera
};

// Your values, in the order you named them when creating the material.
cbuffer MaterialUniforms : register(b1, space3)
{
    float4 Params[8];
    float4 MaterialTime; // x = seconds since start, y = seconds since last frame
};

float4 Param(uint slot)
{
    return Params[slot];
}

float Seconds()
{
    return MaterialTime.x;
}

float DeltaSeconds()
{
    return MaterialTime.y;
}

float4 SampleScene(float2 uv)
{
    return Scene.SampleLevel(SceneSampler, uv, 0);
}

// The scene offset by a number of pixels, for kernels and edge detection.
float4 SampleSceneOffset(float2 uv, float2 pixels)
{
    return Scene.SampleLevel(SceneSampler, uv + pixels * TexelSize.xy, 0);
}

// Distance from the camera plane to the nearest surface under this point, in
// world units. The far plane where nothing was drawn.
float SceneDistance(float2 uv)
{
    float depth = SceneDepth.SampleLevel(SceneDepthSampler, uv, 0);
    float nearPlane = DepthParams.x;
    float farPlane = DepthParams.y;

    if (DepthParams.z > 0.5)
    {
        return nearPlane + depth * (farPlane - nearPlane);
    }
    return nearPlane * farPlane / (farPlane - depth * (farPlane - nearPlane));
}

float Luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

float3 LinearToDisplay(float3 color)
{
    color = saturate(color);
    float3 low = color * 12.92;
    float3 high = 1.055 * pow(color, 1.0 / 2.4) - 0.055;
    return lerp(high, low, color <= 0.0031308);
}

float3 DisplayToLinear(float3 color)
{
    float3 low = color / 12.92;
    float3 high = pow((color + 0.055) / 1.055, 2.4);
    return lerp(high, low, color <= 0.04045);
}

// Coverage of a signed distance, smoothed across one pixel.
float AntiAlias(float distance)
{
    float width = max(fwidth(distance), 1e-5);
    return saturate(0.5 - distance / width);
}

float AntiAliasedStep(float edge, float value)
{
    return AntiAlias(edge - value);
}

#endif // LUDIFEX_WORLD_POST_HLSLI
