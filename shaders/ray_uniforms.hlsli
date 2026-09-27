// The uniform block every ray-tracing pass reads. Mirrors RayUniforms in the
// renderer; the layouts must agree.
//
// Compute stages take their uniforms in the third register space and set,
// after the textures and buffers they read and the textures they write.

#ifndef LUDIFEX_RAY_UNIFORMS_HLSLI
#define LUDIFEX_RAY_UNIFORMS_HLSLI

#ifdef __spirv__
#define LUDIFEX_RAY_UNIFORM_SLOT [[vk::binding(0, 2)]]
#else
#define LUDIFEX_RAY_UNIFORM_SLOT
#endif

LUDIFEX_RAY_UNIFORM_SLOT cbuffer RayUniforms : register(b0, space2)
{
    float4x4 InverseViewProjection; // unjittered
    float4   CameraPosition;
    float4   CameraForward;         // xyz, w the near plane
    float4   SunDirection;          // xyz from the sun toward the scene, w cosine of its radius for soft shadows
    float4   SunLight;              // rgb the sun's colour times its intensity
    float4   AmbientColor;          // rgb, w 1 when the sky lights the scene instead
    float4   SkyColor;              // rgb the flat background, w 1 when a backdrop image fills it
    float4   SkyZenith;             // as sky_model.hlsli takes them
    float4   SkyHorizon;
    float4   SkyGround;
    float4   FogColor;              // rgb, w enabled
    float4   FogParams;             // x start, y end
    float4   Size;                  // xy the output's size in pixels, zw one pixel in texture units
    float4   Params;                // x occlusion reach, y roughest reflection, z far plane, w longest history
    float4   Backdrop;              // xy scale and zw offset of the backdrop image's coordinates
    uint4    Settings;              // x which effects, y frame, z point lights, w instances
    uint4    Progress;              // x samples gathered, y samples this frame, z bounces, w filter step
};

static const uint RayEffectShadows = 1;
static const uint RayEffectOcclusion = 2;
static const uint RayEffectReflections = 4;
static const uint RayEffectRestart = 8;

#endif // LUDIFEX_RAY_UNIFORMS_HLSLI
