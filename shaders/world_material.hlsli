// ludifex world material contract.
//
// Include this from a custom world shader. It defines what the vertex stage
// hands you, the scene's lighting, the uniform block your Params land in, and
// helpers, including Shade(), the built-in lighting, so a material can change
// an object's colour without reimplementing its lighting.
//
// A material replaces the fragment stage only. The vertex stage stays
// ludifex's, so an actor with a custom material is still instanced, culled,
// and moved by physics as before.
//
//   #include "world_material.hlsli"
//
//   float4 FragmentMain(SurfaceInput input) : SV_Target
//   {
//       float stripes = AntiAliasedStep(0.5, frac(input.LocalPosition.y * 6.0));
//       float3 base = lerp(Param(input, 0).rgb, Param(input, 1).rgb, stripes);
//       return float4(Shade(input, base), 1.0);
//   }
//
// Colour: everything here is linear light. Colours you pass from C++ (the
// actor's colour, Color uniforms) arrive already converted from the sRGB
// they were authored in, and textures are sampled as linear. Return linear
// light; the engine tone maps and encodes the final image.
//
// Anti-aliasing: geometric edges are multisampled for you. Alpha below 1 is
// turned into coverage (alpha-to-coverage) for an opaque material and blended
// for a transparent one. Procedural edges you draw inside a face are yours to
// smooth: use AntiAlias and AntiAliasedStep instead of step(), which leaves
// hard, aliased edges. Shade() already
// widens specular highlights on tightly curved surfaces so they do not crawl.

#ifndef LUDIFEX_WORLD_MATERIAL_HLSLI
#define LUDIFEX_WORLD_MATERIAL_HLSLI

static const float Pi = 3.14159265358979;

struct SurfaceInput
{
    float4 Position      : SV_Position;
    float3 WorldNormal   : TEXCOORD0;
    float3 WorldPosition : TEXCOORD1;
    float4 Color         : TEXCOORD2; // the actor's colour, linear, straight alpha
    float3 LocalPosition : TEXCOORD3; // position in the mesh's own space
    float2 UV            : TEXCOORD4; // texture coordinates, 0..1 across a face
    nointerpolation uint Instance : TEXCOORD5; // which actor, for Param() and friends
};

// One per drawn object. Mirrors InstanceData in the renderer.
struct InstanceData
{
    float4x4 Model;
    float4x4 PreviousModel;
    float4   BaseColor;   // linear
    float4   Surface;     // x roughness, y metallic, z occlusion strength, w normal-map strength
    float4   UVTransform; // xy scale, zw offset, applied to texture coordinates
    float4   Emission;    // rgb linear light given off, before any map
    uint4    Extra;       // x first per-actor param, y param mask, z flags, w joint palette offset
    uint4    Morph;       // x first active morph target in the frame's table, y how many
};

static const uint InstanceFlagUnlit = 1;
static const uint InstanceFlagFlipX = 2;

// This instance's vertices are skinned, and Extra.w is where its joint
// matrices start in the frame's shared palette.
static const uint InstanceFlagSkinned = 4;

struct PointLight
{
    float4 PositionRange;   // xyz position, w range
    float4 ColorIntensity;  // rgb linear colour, w intensity
};

// --- resources ---------------------------------------------------------------
//
// One source serves Direct3D 12, Vulkan, and Metal. The registers are what
// Direct3D reads. Vulkan and Metal are compiled through SPIR-V, where a
// texture and its sampler share one slot as a combined image sampler
// (LUDIFEX_SAMPLED). Metal numbers a stage's storage buffers after its uniform
// blocks, so on the way there each one is given that number instead
// (LUDIFEX_STORAGE). None of this is anything a material has to repeat.

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

// The fragment stage's uniform blocks: the scene's, and a material's own.
#ifdef LUDIFEX_DEFAULT_SHADER
#define LUDIFEX_SURFACE_UNIFORMS 1
#else
#define LUDIFEX_SURFACE_UNIFORMS 2
#endif

LUDIFEX_SAMPLED(0, 2) Texture2D<float4> BaseColorTexture : register(t0, space2);
LUDIFEX_SAMPLED(0, 2) SamplerState BaseColorSampler : register(s0, space2);

LUDIFEX_SAMPLED(1, 2) Texture2D<float> ShadowMap : register(t1, space2);
LUDIFEX_SAMPLED(1, 2) SamplerComparisonState ShadowSampler : register(s1, space2);

// The rest of a surface's maps. Every actor has all four bound: one without a
// map samples a single neutral texel (a flat normal, full roughness and
// metalness factors, no emission, no occlusion).
LUDIFEX_SAMPLED(2, 2) Texture2D<float4> NormalTexture : register(t2, space2);
LUDIFEX_SAMPLED(2, 2) SamplerState NormalSampler : register(s2, space2);
LUDIFEX_SAMPLED(3, 2) Texture2D<float4> MetallicRoughnessTexture : register(t3, space2);
LUDIFEX_SAMPLED(3, 2) SamplerState MetallicRoughnessSampler : register(s3, space2);
LUDIFEX_SAMPLED(4, 2) Texture2D<float4> EmissiveTexture : register(t4, space2);
LUDIFEX_SAMPLED(4, 2) SamplerState EmissiveSampler : register(s4, space2);
LUDIFEX_SAMPLED(5, 2) Texture2D<float4> OcclusionTexture : register(t5, space2);
LUDIFEX_SAMPLED(5, 2) SamplerState OcclusionSampler : register(s5, space2);

// What the frame worked out for every pixel before the surfaces were drawn,
// read where the pixel lands on screen: how much ambient light reaches it
// (green, from screen-space or traced occlusion), how much of the sun does
// when shadows are traced (red), and what it reflects when reflections are
// traced (colour, and in alpha how much to trust it). An effect that is off
// leaves a texel that changes nothing. Use SurfaceOcclusion, SunVisibility,
// and EnvironmentReflection rather than reading them directly.
LUDIFEX_SAMPLED(6, 2) Texture2D<float4> ScreenOcclusion : register(t6, space2);
LUDIFEX_SAMPLED(6, 2) SamplerState ScreenOcclusionSampler : register(s6, space2);
LUDIFEX_SAMPLED(7, 2) Texture2D<float4> ScreenSunlight : register(t7, space2);
LUDIFEX_SAMPLED(7, 2) SamplerState ScreenSunlightSampler : register(s7, space2);
LUDIFEX_SAMPLED(8, 2) Texture2D<float4> ScreenReflections : register(t8, space2);
LUDIFEX_SAMPLED(8, 2) SamplerState ScreenReflectionsSampler : register(s8, space2);

LUDIFEX_STORAGE(LUDIFEX_SURFACE_UNIFORMS + 0) StructuredBuffer<InstanceData> Instances : register(t9, space2);
LUDIFEX_STORAGE(LUDIFEX_SURFACE_UNIFORMS + 1) StructuredBuffer<float4> InstanceParams : register(t10, space2);
LUDIFEX_STORAGE(LUDIFEX_SURFACE_UNIFORMS + 2) StructuredBuffer<PointLight> Lights : register(t11, space2);

// Clustered lighting: one range per cell naming where its slice of the index
// list starts and how long it is, and the list itself.
LUDIFEX_STORAGE(LUDIFEX_SURFACE_UNIFORMS + 3) StructuredBuffer<uint2> ClusterRanges : register(t12, space2);
LUDIFEX_STORAGE(LUDIFEX_SURFACE_UNIFORMS + 4) StructuredBuffer<uint> ClusterLights : register(t13, space2);

// Per frame, shared by every material.
cbuffer SceneUniforms : register(b0, space3)
{
    float4   LightDirection;        // xyz points from the light toward the scene
    float4   LightColor;            // rgb linear, w intensity
    float4   AmbientColor;          // rgb linear, w 1 when the sky lights the scene instead
    float4   CameraPosition;
    float4   CameraForward;         // xyz where the camera looks, w near plane
    float4x4 ShadowViewProjection[4]; // one per cascade
    float4   ShadowSplits;          // where each cascade ends, in depth along the view
    float4   ShadowOffsets;         // how far each cascade's lookups are pushed off the surface
    float4   ShadowParams;          // x texel size, y softness in texels, z cascades (0 off), w cascades a row
    float4   ShadowFade;            // x distance where fading starts, y fade length
    float4   FogColor;              // rgb linear, w enabled
    float4   FogParams;             // x start, y end
    float4   ClusterParams;         // x depth scale, y depth bias, z tiles across, w tiles down
    float4   ClusterViewport;       // xy the target's size in pixels
    uint4    Counts;                // x point lights, y depth slices
    float4   ScreenParams;          // xy one pixel in texture units, z screen textures apply, w sun from them
    float4   SkyZenith;             // rgb linear, w brightness
    float4   SkyHorizon;            // rgb linear, w cosine of the sun disc's radius
    float4   SkyGround;             // rgb linear, w the sun disc's brightness
};

// The built-in shader has no Params, so it opts out; a material gets them
// without having to define anything.
#ifndef LUDIFEX_DEFAULT_SHADER
// Your values, in the order you named them when creating the material.
// Anything you did not set reads as zero. Prefer Param(), which also sees
// values set on one actor alone with Actor3D::SetUniform.
cbuffer MaterialUniforms : register(b1, space3)
{
    float4 Params[8];
    float4 MaterialTime; // x = seconds since start, y = seconds since last frame
};

float Seconds()
{
    return MaterialTime.x;
}

float DeltaSeconds()
{
    return MaterialTime.y;
}

// A uniform as this actor sees it: its own value if SetUniform gave it one,
// the material's otherwise. Actors with differing values still share a draw.
float4 Param(SurfaceInput input, uint slot)
{
    InstanceData instance = Instances[input.Instance];
    if ((instance.Extra.y & (1u << slot)) != 0)
    {
        return InstanceParams[instance.Extra.x + slot];
    }
    return Params[slot];
}
#endif

// --- the surface -------------------------------------------------------------

// The geometric normal, before any normal map.
float3 GeometryNormal(SurfaceInput input)
{
    return normalize(input.WorldNormal);
}

// The normal the surface is lit with: the geometry's, bent by the normal map
// when the actor has one.
//
// There is no tangent in the vertex. The frame the map is read in is found
// from how the position and the texture coordinates change across the pixel,
// which works for any surface with sensible coordinates and needs no vertex
// data, so one vertex layout serves every mesh. Everything
// here runs whether or not there is a map, because derivatives are only
// defined when every pixel of a quad computes them.
float3 SurfaceNormal(SurfaceInput input)
{
    float3 normal = GeometryNormal(input);

    // Screen-space rates of change, taken rightward and *upward*. ddy runs
    // down the screen in this API, and the frame below would come out with
    // the wrong handedness (every bump read as a dent) unless it is flipped
    // to point up, as the construction assumes.
    float3 dp1 = ddx(input.WorldPosition);
    float3 dp2 = -ddy(input.WorldPosition);
    float2 duv1 = ddx(input.UV);
    float2 duv2 = -ddy(input.UV);

    float3 dp2perp = cross(dp2, normal);
    float3 dp1perp = cross(normal, dp1);
    float3 tangent = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 bitangent = dp2perp * duv1.y + dp1perp * duv2.y;
    float scale = rsqrt(max(max(dot(tangent, tangent), dot(bitangent, bitangent)), 1e-20));

    float3 mapped = NormalTexture.Sample(NormalSampler, input.UV).xyz * 2.0 - 1.0;
    float strength = Instances[input.Instance].Surface.w;
    mapped.xy *= strength;

    // Texture coordinates run down the image and a normal map's green points
    // up it, as glTF specifies, so the bitangent is taken the other way.
    float3 bent = tangent * scale * mapped.x - bitangent * scale * mapped.y + normal * max(mapped.z, 1e-4);
    return strength > 0.0 ? normalize(bent) : normal;
}

// Light the surface gives off, in linear light: the emissive map times the
// actor's emission.
float3 SurfaceEmission(SurfaceInput input)
{
    return EmissiveTexture.Sample(EmissiveSampler, input.UV).rgb * Instances[input.Instance].Emission.rgb;
}

// Where this pixel lands in the frame's screen-sized textures.
float2 ScreenUV(SurfaceInput input)
{
    return input.Position.xy * ScreenParams.xy;
}

// How much ambient light reaches this point: 1 in the open, less in creases
// the occlusion map darkens, scaled by the actor's occlusion strength, and
// less again where ambient occlusion found something close by.
float SurfaceOcclusion(SurfaceInput input)
{
    float occlusion = OcclusionTexture.Sample(OcclusionSampler, input.UV).r;
    float mapped = lerp(1.0, occlusion, Instances[input.Instance].Surface.z);
    if (ScreenParams.z > 0.5)
    {
        mapped *= ScreenOcclusion.SampleLevel(ScreenOcclusionSampler, ScreenUV(input), 0).g;
    }
    return mapped;
}

float3 ToLight()
{
    return normalize(-LightDirection.xyz);
}

float3 ToCamera(SurfaceInput input)
{
    return normalize(CameraPosition.xyz - input.WorldPosition);
}

// The actor's texture at this point, linear. White when it has none.
float4 SampleBaseColor(SurfaceInput input)
{
    return BaseColorTexture.Sample(BaseColorSampler, input.UV);
}

// The actor's colour times its texture: what the built-in shader lights.
float4 BaseColor(SurfaceInput input)
{
    return input.Color * SampleBaseColor(input);
}

// The actor's roughness times the metallic-roughness map's green channel,
// and its metalness times the blue, as glTF lays them out.
float SurfaceRoughness(SurfaceInput input)
{
    return Instances[input.Instance].Surface.x *
           MetallicRoughnessTexture.Sample(MetallicRoughnessSampler, input.UV).g;
}

float SurfaceMetallic(SurfaceInput input)
{
    return Instances[input.Instance].Surface.y *
           MetallicRoughnessTexture.Sample(MetallicRoughnessSampler, input.UV).b;
}

bool IsUnlit(SurfaceInput input)
{
    return (Instances[input.Instance].Extra.z & InstanceFlagUnlit) != 0;
}

// How much the surface faces away from the viewer, 0 head-on to 1 at the
// silhouette. Raise it to a power to tighten the band.
float Fresnel(SurfaceInput input, float power)
{
    float facing = saturate(dot(SurfaceNormal(input), ToCamera(input)));
    return pow(1.0 - facing, power);
}

// --- anti-aliasing -----------------------------------------------------------

// Coverage of a signed distance, smoothed across one pixel from the
// screen-space derivative.
float AntiAlias(float distance)
{
    float width = max(fwidth(distance), 1e-5);
    return saturate(0.5 - distance / width);
}

// A smooth replacement for step(edge, value).
float AntiAliasedStep(float edge, float value)
{
    return AntiAlias(edge - value);
}

// Specular anti-aliasing. Where the normal changes quickly from one pixel to
// the next (a small sphere, a tight bevel), a sharp highlight would flicker
// between pixels as the object moves. Widening the highlight by how much the
// normal varies across the pixel removes the flicker without blurring flat
// surfaces.
float SpecularAntiAliasedRoughness(float3 normal, float roughness)
{
    float3 du = ddx(normal);
    float3 dv = ddy(normal);
    float variance = 0.25 * (dot(du, du) + dot(dv, dv));
    float kernel = min(2.0 * variance, 0.18);

    float alpha = roughness * roughness;
    float widened = saturate(alpha * alpha + kernel);
    return sqrt(sqrt(widened));
}

// --- the sky -----------------------------------------------------------------
//
// With SkySettings on, the sky is three colours and the sun, and it lights the
// world as well as filling the background. The same functions draw the
// background, light the surfaces, and fill the rays that miss everything when
// tracing, so the three always agree.

bool SkyLightsScene()
{
    return AmbientColor.w > 0.5;
}

// The sky seen along a direction, in linear light. withSun adds the sun's disc
// and the glow around it; leave it out where the sun's light is counted
// already, as it is on any surface the directional light reaches.
float3 SkyRadiance(float3 direction, bool withSun)
{
    float up = direction.y;
    float3 above = lerp(SkyHorizon.rgb, SkyZenith.rgb, pow(saturate(up), 0.45));
    float3 below = lerp(SkyHorizon.rgb, SkyGround.rgb, 1.0 - exp(-max(-up, 0.0) * 14.0));
    float3 color = up >= 0.0 ? above : below;

    if (withSun)
    {
        float facing = dot(direction, normalize(-LightDirection.xyz));
        float3 sun = LightColor.rgb * LightColor.w;
        color += sun * (pow(saturate(facing), 400.0) * 0.6 + pow(saturate(facing), 12.0) * 0.08);
        float edge = max(1.0 - SkyHorizon.w, 1e-6) * 0.25;
        color += sun * SkyGround.w * smoothstep(SkyHorizon.w - edge, SkyHorizon.w + edge, facing);
    }
    return color * SkyZenith.w;
}

// The sky's light arriving at a surface facing `normal`, averaged over
// everything that surface can see: mostly the zenith for a floor, the horizon
// and the ground for a wall.
float3 SkyAmbient(float3 normal)
{
    float3 above = lerp(SkyHorizon.rgb, SkyZenith.rgb, 0.55);
    float3 below = lerp(SkyHorizon.rgb, SkyGround.rgb, 0.85);
    float t = normal.y * 0.5 + 0.5;
    return lerp(below, above, t * t * (3.0 - 2.0 * t)) * SkyZenith.w;
}

// --- lighting ----------------------------------------------------------------

// The ambient light reaching a surface facing `normal`, before occlusion: the
// sky's when it lights the scene, the ambient colour otherwise. Full from
// above and less from below, so shapes keep their form on the side the sun
// does not reach.
float3 AmbientLight(float3 normal)
{
    if (SkyLightsScene())
    {
        return SkyAmbient(normal);
    }
    return AmbientColor.rgb * lerp(0.55, 1.0, normal.y * 0.5 + 0.5);
}

// What a glossy surface reflects along `direction`: the sky or the ambient
// light, blurred toward the diffuse ambient light as the surface roughens, or
// what ray-traced reflections found there when they are on.
float3 EnvironmentReflection(SurfaceInput input, float3 direction, float3 normal, float roughness)
{
    float3 environment = SkyLightsScene() ? SkyRadiance(direction, false) : AmbientLight(direction);
    environment = lerp(environment, AmbientLight(normal), saturate(roughness * roughness * 1.5));

    if (ScreenParams.z > 0.5)
    {
        float4 traced = ScreenReflections.SampleLevel(ScreenReflectionsSampler, ScreenUV(input), 0);
        environment = lerp(environment, traced.rgb, saturate(traced.a));
    }
    return environment;
}

// The share of reflected light a surface sends back toward the viewer, as a
// scale and a bias on its reflectance at normal incidence: Karis's fit to the
// integrated GGX response.
float2 EnvironmentResponse(float roughness, float facing)
{
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * facing)) * r.x + r.y;
    return float2(-1.04, 1.04) * a004 + r.zw;
}

// One cascade's look-up: 1 lit, 0 in shadow, filtered across ShadowParams.y
// texels, or 1 when the point is outside what the cascade covers.
float ShadowCascade(uint cascade, float3 worldPosition, float3 normal, float grazing)
{
    // Pushing the lookup out along the normal, more at grazing angles, is what
    // stops a surface shadowing itself in stripes.
    float3 offsetPosition = worldPosition + normal * ShadowOffsets[cascade] * (0.5 + grazing);

    float4 clip = mul(ShadowViewProjection[cascade], float4(offsetPosition, 1.0));
    float3 ndc = clip.xyz / clip.w;
    float2 uv = ndc.xy * float2(0.5, -0.5) + 0.5;

    if (any(uv < 0.0) || any(uv > 1.0) || ndc.z > 1.0)
    {
        return 1.0;
    }

    // The cascades share one texture, two a row when there is more than one;
    // the taps are kept inside this cascade's own square of it.
    float perRow = max(ShadowParams.w, 1.0);
    float cell = 1.0 / perRow;
    float2 corner = float2(cascade % uint(perRow), cascade / uint(perRow)) * cell;
    float spread = ShadowParams.x * ShadowParams.y;
    float margin = spread + ShadowParams.x;
    float2 atlas = corner + clamp(uv * cell, margin, cell - margin);

    // Nine hardware-filtered taps, each itself a 2x2 comparison, give a soft
    // edge without the banding of a plain grid.
    float lit = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            lit += ShadowMap.SampleCmpLevelZero(ShadowSampler, atlas + float2(x, y) * spread, ndc.z);
        }
    }
    return lit / 9.0;
}

// How much of the directional light the shadow maps let reach this point, 1
// fully lit to 0 in shadow. Each pixel reads the nearest cascade that covers
// it, and near a cascade's far edge blends into the next so the change in
// sharpness never shows as a line.
float ShadowFactor(float3 worldPosition, float3 normal)
{
    uint cascades = uint(ShadowParams.z + 0.5);
    if (cascades == 0)
    {
        return 1.0;
    }

    float grazing = 1.0 - saturate(dot(normal, ToLight()));
    float depth = dot(worldPosition - CameraPosition.xyz, CameraForward.xyz);

    uint cascade = 0;
    [unroll] for (uint index = 0; index < 3; ++index)
    {
        if (index + 1 < cascades && depth > ShadowSplits[index])
        {
            cascade = index + 1;
        }
    }

    float lit = ShadowCascade(cascade, worldPosition, normal, grazing);

    float end = ShadowSplits[cascade];
    float start = cascade > 0 ? ShadowSplits[cascade - 1] : 0.0;
    float band = (end - start) * 0.15;
    float blend = saturate((depth - (end - band)) / max(band, 1e-3));
    if (blend > 0.0 && cascade + 1 < cascades)
    {
        lit = lerp(lit, ShadowCascade(cascade + 1, worldPosition, normal, grazing), blend);
    }

    // Fading out toward the far edge of the shadowed region hides where the
    // maps end.
    float distance = length(CameraPosition.xyz - worldPosition);
    float fade = saturate((distance - ShadowFade.x) / max(ShadowFade.y, 1e-3));
    return lerp(lit, 1.0, fade);
}

// How much of the sun reaches this point: traced, when ray-traced shadows are
// on, or from the shadow maps.
float SunVisibility(SurfaceInput input, float3 normal)
{
    if (ScreenParams.z > 0.5 && ScreenParams.w > 0.5)
    {
        return ScreenSunlight.SampleLevel(ScreenSunlightSampler, ScreenUV(input), 0).r;
    }
    return ShadowFactor(input.WorldPosition, normal);
}

// GGX distribution, height-correlated Smith visibility, and Schlick Fresnel:
// the standard physically based specular, so metals and plastics read right.
float3 EvaluateLight(float3 normal, float3 toView, float3 toLight, float3 diffuseColor, float3 f0,
                     float roughness)
{
    float nl = saturate(dot(normal, toLight));
    if (nl <= 0.0)
    {
        return 0.0;
    }

    float3 halfway = normalize(toView + toLight);
    float nv = max(dot(normal, toView), 1e-4);
    float nh = saturate(dot(normal, halfway));
    float vh = saturate(dot(toView, halfway));

    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;

    float d = nh * nh * (alpha2 - 1.0) + 1.0;
    float distribution = alpha2 / (Pi * d * d);

    float lambdaV = nl * sqrt(nv * nv * (1.0 - alpha2) + alpha2);
    float lambdaL = nv * sqrt(nl * nl * (1.0 - alpha2) + alpha2);
    float visibility = 0.5 / max(lambdaV + lambdaL, 1e-5);

    float3 fresnel = f0 + (1.0 - f0) * pow(1.0 - vh, 5.0);

    // Scaled by Pi so a light of intensity 1 lights a white surface to 1, the
    // same scale the diffuse term uses.
    float3 specular = distribution * visibility * fresnel * Pi;
    float3 diffuse = diffuseColor * (1.0 - fresnel);

    return (diffuse + specular) * nl;
}

float3 ApplyFog(SurfaceInput input, float3 color)
{
    if (FogColor.w < 0.5)
    {
        return color;
    }
    float distance = length(CameraPosition.xyz - input.WorldPosition);
    float amount = saturate((distance - FogParams.x) / max(FogParams.y - FogParams.x, 1e-3));
    return lerp(color, FogColor.rgb, amount);
}

// The engine's lighting with the surface properties spelled out: the sun with
// its shadows, every point light, ambient light from the sky or the ambient
// colour with its reflection, and fog.
float3 ShadeSurface(SurfaceInput input, float3 albedo, float roughness, float metallic)
{
    // Read before anything branches: the normal map's frame uses derivatives,
    // which every pixel of a quad has to compute together.
    float3 normal = SurfaceNormal(input);
    float3 emission = SurfaceEmission(input);
    float occlusion = SurfaceOcclusion(input);

    if (IsUnlit(input))
    {
        return ApplyFog(input, albedo + emission);
    }

    float3 toView = ToCamera(input);

    roughness = SpecularAntiAliasedRoughness(normal, clamp(roughness, 0.045, 1.0));
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 diffuseColor = albedo * (1.0 - metallic);

    float3 color = 0.0;

    float shadow = SunVisibility(input, normal);
    color += EvaluateLight(normal, toView, ToLight(), diffuseColor, f0, roughness) * LightColor.rgb *
             LightColor.w * shadow;

    // The cell this pixel falls in: its tile across the screen, and its slice
    // in depth. Slices are spaced logarithmically, because a metre near the
    // camera covers far more of the screen than a metre far away.
    uint2 tile = uint2(input.Position.xy / ClusterViewport.xy *
                       float2(ClusterParams.z, ClusterParams.w));
    tile = min(tile, uint2(uint(ClusterParams.z) - 1, uint(ClusterParams.w) - 1));

    float viewDepth = dot(input.WorldPosition - CameraPosition.xyz, CameraForward.xyz);
    uint slice = 0;
    if (viewDepth > CameraForward.w)
    {
        slice = uint(max(log(viewDepth) * ClusterParams.x + ClusterParams.y, 0.0));
    }
    slice = min(slice, Counts.y - 1);

    uint cell = slice * uint(ClusterParams.z) * uint(ClusterParams.w) +
                tile.y * uint(ClusterParams.z) + tile.x;
    uint2 range = ClusterRanges[cell];

    for (uint entry = 0; entry < range.y; ++entry)
    {
        uint index = ClusterLights[range.x + entry];
        PointLight light = Lights[index];
        float3 offset = light.PositionRange.xyz - input.WorldPosition;
        float distanceSquared = dot(offset, offset);
        float range = light.PositionRange.w;

        // Inverse-square falloff, windowed so it reaches exactly zero at the
        // range rather than trailing on forever.
        float ratio = distanceSquared / (range * range);
        float window = saturate(1.0 - ratio * ratio);
        float attenuation = window * window / (distanceSquared + 1.0);
        if (attenuation <= 0.0)
        {
            continue;
        }

        float3 toLight = offset * rsqrt(max(distanceSquared, 1e-8));
        color += EvaluateLight(normal, toView, toLight, diffuseColor, f0, roughness) *
                 light.ColorIntensity.rgb * light.ColorIntensity.w * attenuation;
    }

    // Ambient light: diffuse from every direction the surface faces, and
    // specular from the one it mirrors, weighted by how much a surface of
    // this roughness sends back toward the viewer.
    float facing = max(dot(normal, toView), 1e-4);
    color += AmbientLight(normal) * diffuseColor * occlusion;

    float3 mirrored = reflect(-toView, normal);
    float2 response = EnvironmentResponse(roughness, facing);
    color += EnvironmentReflection(input, mirrored, normal, roughness) * (f0 * response.x + response.y) *
             occlusion;

    // A faint rim of the flat ambient light, which keeps silhouettes readable
    // against a dark background. The sky has a horizon of its own to do that.
    if (!SkyLightsScene())
    {
        color += pow(1.0 - facing, 4.0) * AmbientColor.rgb * 0.35 * occlusion;
    }

    // Emission is light, not lighting: it is there in the dark and shadows
    // do not touch it.
    color += emission;

    return ApplyFog(input, color);
}

// The engine's lighting for this actor: its own roughness and metallic, as
// set with SetRoughness and SetMetallic. Call this to keep a custom colour lit
// the same way as everything else.
float3 Shade(SurfaceInput input, float3 baseColor)
{
    return ShadeSurface(input, baseColor, SurfaceRoughness(input), SurfaceMetallic(input));
}

#endif // LUDIFEX_WORLD_MATERIAL_HLSLI
