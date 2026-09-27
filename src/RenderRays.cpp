// Ray tracing and path tracing: uploading the ray scene and running the
// compute passes that trace it. The scene itself is RayScene.cpp; the shaders
// are rays.hlsl, ray_denoise.hlsl, and pathtrace.hlsl.

#include "Render3D.h"

#include "Backend.h"
#include "RayScene.h"
#include "WorldTextures.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ludifex::detail
{
namespace
{

constexpr float Pi = 3.14159265358979323846f;

// Mirrors RayUniforms in ray_uniforms.hlsli.
struct RayUniforms
{
    float InverseViewProjection[16];
    float CameraPosition[4];
    float CameraForward[4];
    float SunDirection[4];
    float SunLight[4];
    float AmbientColor[4];
    float SkyColor[4];
    float SkyZenith[4];
    float SkyHorizon[4];
    float SkyGround[4];
    float FogColor[4];
    float FogParams[4];
    float Size[4];
    float Params[4];
    float Backdrop[4];
    uint32_t Settings[4];
    uint32_t Progress[4];
};

// Which effects a hybrid frame traces, as rays.hlsl reads them.
constexpr uint32_t EffectShadows = 1;
constexpr uint32_t EffectOcclusion = 2;
constexpr uint32_t EffectReflections = 4;
constexpr uint32_t EffectRestart = 8;

// How many frames a traced pixel's history may hold. More is smoother and
// slower to follow a moving shadow.
constexpr float LongestHistory = 24.0f;

// As in Render3D.cpp: the sun disc's size and brightness in the sky.
constexpr float SunDiscDegrees = 0.75f;
constexpr float SunDiscBrightness = 40.0f;

uint64_t HashBytes(uint64_t hash, const void* data, size_t size)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t index = 0; index < size; ++index)
    {
        hash ^= bytes[index];
        hash *= 0x100000001B3ull;
    }
    return hash;
}

void Store(float out[4], float x, float y, float z, float w)
{
    out[0] = x;
    out[1] = y;
    out[2] = z;
    out[3] = w;
}

} // namespace

bool Renderer3D::CreateRayPipelines()
{
    m_RayHybridPipeline =
        CreateComputePipeline(m_Device, "RayHybridMain", GetBuiltInShader(BuiltInShader::RayHybrid), 3, 6, 2, 1);
    m_RayTemporalPipeline =
        CreateComputePipeline(m_Device, "RayTemporalMain", GetBuiltInShader(BuiltInShader::RayTemporal), 6, 0, 2, 1);
    m_RayFilterPipeline =
        CreateComputePipeline(m_Device, "RayFilterMain", GetBuiltInShader(BuiltInShader::RayFilter), 6, 0, 2, 1);
    m_PathTracePipeline =
        CreateComputePipeline(m_Device, "PathTraceMain", GetBuiltInShader(BuiltInShader::PathTrace), 3, 6, 2, 1);

    if (m_RayHybridPipeline == nullptr || m_RayTemporalPipeline == nullptr || m_RayFilterPipeline == nullptr ||
        m_PathTracePipeline == nullptr)
    {
        ReleaseRayPipelines();
        return false;
    }
    return true;
}

void Renderer3D::ReleaseRayPipelines()
{
    for (SDL_GPUComputePipeline** pipeline :
         { &m_RayHybridPipeline, &m_RayTemporalPipeline, &m_RayFilterPipeline, &m_PathTracePipeline })
    {
        if (*pipeline != nullptr && m_Device != nullptr)
        {
            SDL_ReleaseGPUComputePipeline(m_Device, *pipeline);
        }
        *pipeline = nullptr;
    }
}

void Renderer3D::ReleaseRayTargets()
{
    auto Release = [&](SDL_GPUTexture*& texture) {
        if (texture != nullptr)
        {
            SDL_ReleaseGPUTexture(m_Device, texture);
            texture = nullptr;
        }
    };

    Release(m_RayLighting);
    Release(m_RayReflection);
    for (size_t index = 0; index < 2; ++index)
    {
        Release(m_RayHistory[index]);
        Release(m_RayHistoryReflection[index]);
        Release(m_RayFiltered[index]);
        Release(m_RayFilteredReflection[index]);
        Release(m_PathSum[index]);
    }
    Release(m_PathImage);

    m_RayWidth = 0;
    m_RayHeight = 0;
    m_RayHistoryValid = false;
    m_PathSamples = 0;
    m_PathFingerprint = 0;
}

bool Renderer3D::EnsureRayTargets(int width, int height)
{
    if (m_RayLighting != nullptr && m_RayWidth == width && m_RayHeight == height)
    {
        return true;
    }

    // Everything but the path tracer's, which has its own size and lifetime.
    auto Release = [&](SDL_GPUTexture*& texture) {
        if (texture != nullptr)
        {
            SDL_ReleaseGPUTexture(m_Device, texture);
            texture = nullptr;
        }
    };
    Release(m_RayLighting);
    Release(m_RayReflection);
    for (size_t index = 0; index < 2; ++index)
    {
        Release(m_RayHistory[index]);
        Release(m_RayHistoryReflection[index]);
        Release(m_RayFiltered[index]);
        Release(m_RayFilteredReflection[index]);
    }

    const SDL_GPUTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
    auto Make = [&](SDL_GPUTexture*& texture) {
        texture = CreateTarget(m_Device, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, width, height,
                               SDL_GPU_SAMPLECOUNT_1, usage);
        return texture != nullptr;
    };

    bool made = Make(m_RayLighting) && Make(m_RayReflection);
    for (size_t index = 0; index < 2 && made; ++index)
    {
        made = Make(m_RayHistory[index]) && Make(m_RayHistoryReflection[index]) && Make(m_RayFiltered[index]) &&
               Make(m_RayFilteredReflection[index]);
    }

    m_RayWidth = made ? width : 0;
    m_RayHeight = made ? height : 0;
    m_RayHistoryValid = false;
    return made;
}

bool Renderer3D::EnsurePathTargets()
{
    if (m_PathImage != nullptr)
    {
        return true;
    }

    const SDL_GPUTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
    m_PathSum[0] = CreateTarget(m_Device, SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT, m_Width, m_Height,
                                SDL_GPU_SAMPLECOUNT_1, usage);
    m_PathSum[1] = CreateTarget(m_Device, SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT, m_Width, m_Height,
                                SDL_GPU_SAMPLECOUNT_1, usage);
    m_PathImage = CreateTarget(m_Device, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, m_Width, m_Height,
                               SDL_GPU_SAMPLECOUNT_1, usage);
    m_PathSamples = 0;
    m_PathFingerprint = 0;
    return m_PathSum[0] != nullptr && m_PathSum[1] != nullptr && m_PathImage != nullptr;
}

void Renderer3D::Dispatch(SDL_GPUCommandBuffer* commandBuffer, SDL_GPUComputePipeline* pipeline,
                          const std::vector<SDL_GPUTexture*>& sources, const std::vector<SDL_GPUSampler*>& samplers,
                          const std::vector<SDL_GPUBuffer*>& buffers, const std::vector<SDL_GPUTexture*>& outputs,
                          const void* uniforms, size_t uniformSize, int width, int height)
{
    std::vector<SDL_GPUStorageTextureReadWriteBinding> written(outputs.size());
    for (size_t index = 0; index < outputs.size(); ++index)
    {
        written[index].texture = outputs[index];
    }

    SDL_GPUComputePass* pass =
        SDL_BeginGPUComputePass(commandBuffer, written.data(), static_cast<Uint32>(written.size()), nullptr, 0);
    if (pass == nullptr)
    {
        return;
    }

    SDL_BindGPUComputePipeline(pass, pipeline);

    std::vector<SDL_GPUTextureSamplerBinding> bindings(sources.size());
    for (size_t index = 0; index < sources.size(); ++index)
    {
        bindings[index].texture = sources[index];
        bindings[index].sampler = samplers[index];
    }
    if (!bindings.empty())
    {
        SDL_BindGPUComputeSamplers(pass, 0, bindings.data(), static_cast<Uint32>(bindings.size()));
    }
    if (!buffers.empty())
    {
        SDL_BindGPUComputeStorageBuffers(pass, 0, buffers.data(), static_cast<Uint32>(buffers.size()));
    }
    SDL_PushGPUComputeUniformData(commandBuffer, 0, uniforms, static_cast<Uint32>(uniformSize));

    // Eight by eight threads a group, as every tracing pass declares.
    SDL_DispatchGPUCompute(pass, static_cast<Uint32>((width + 7) / 8), static_cast<Uint32>((height + 7) / 8), 1);
    SDL_EndGPUComputePass(pass);
}

bool Renderer3D::TraceRays(SDL_GPUCommandBuffer* commandBuffer, const FrameCamera& camera,
                           const RenderSettings& settings, const RenderExtras& extras, bool pathTracing)
{
    if (m_Rays == nullptr || !m_Rays->Upload(m_Device, commandBuffer))
    {
        return false;
    }

    const RayTracingSettings& rays = settings.RayTracing;
    WorldTextureStore& textures = GetWorldTextures();

    RayUniforms uniforms{};
    std::memcpy(uniforms.InverseViewProjection, InvertMatrix(camera.Projection * camera.View).M, sizeof(float) * 16);

    const Vec3 forward = NormalizeVector(camera.Forward);
    Store(uniforms.CameraPosition, camera.Position.X, camera.Position.Y, camera.Position.Z, 0.0f);
    Store(uniforms.CameraForward, forward.X, forward.Y, forward.Z, camera.NearPlane);

    // The sun as a disc of the angle asked for, which softens traced
    // shadows: a point further from what casts it sees more of the disc
    // around the caster's edge.
    const Vec3 sun = NormalizeVector(settings.Light.Direction);
    const float sunRadius = std::clamp(rays.SunAngle, 0.0f, 20.0f) * 0.5f * Pi / 180.0f;
    const Color sunColor = ToLinear(settings.Light.Tint);
    const float intensity = std::max(settings.Light.Intensity, 0.0f);
    Store(uniforms.SunDirection, sun.X, sun.Y, sun.Z, std::cos(sunRadius));
    Store(uniforms.SunLight, sunColor.R * intensity, sunColor.G * intensity, sunColor.B * intensity, 0.0f);

    const Color ambient = ToLinear(settings.AmbientColor);
    const Color background = ToLinear(settings.SkyColor);
    const bool backdrop = extras.Background.IsValid();
    Store(uniforms.AmbientColor, ambient.R, ambient.G, ambient.B, settings.Sky.Enabled ? 1.0f : 0.0f);
    Store(uniforms.SkyColor, background.R, background.G, background.B, backdrop ? 1.0f : 0.0f);

    const Color zenith = ToLinear(settings.Sky.Zenith);
    const Color horizon = ToLinear(settings.Sky.Horizon);
    const Color ground = ToLinear(settings.Sky.Ground);
    const float discRadius = std::max(settings.Sky.SunSize, 0.0f) * SunDiscDegrees * 0.5f * Pi / 180.0f;
    Store(uniforms.SkyZenith, zenith.R, zenith.G, zenith.B, std::max(settings.Sky.Brightness, 0.0f));
    Store(uniforms.SkyHorizon, horizon.R, horizon.G, horizon.B, std::cos(discRadius));
    Store(uniforms.SkyGround, ground.R, ground.G, ground.B, settings.Sky.SunSize > 0.0f ? SunDiscBrightness : 0.0f);

    const Color fog = ToLinear(settings.Fog.Tint);
    Store(uniforms.FogColor, fog.R, fog.G, fog.B, settings.Fog.Enabled ? 1.0f : 0.0f);
    Store(uniforms.FogParams, settings.Fog.Start, settings.Fog.End, 0.0f, 0.0f);

    // The backdrop image covers the view as the rasterizer draws it, cropped
    // on whichever axis overhangs.
    Store(uniforms.Backdrop, 1.0f, 1.0f, 0.0f, 0.0f);
    if (backdrop)
    {
        const Vec2 size = textures.GetSize(extras.Background);
        if (size.X > 0.0f && size.Y > 0.0f)
        {
            const float imageAspect = size.X / size.Y;
            const float viewAspect = static_cast<float>(m_Width) / static_cast<float>(m_Height);
            const float scaleX = viewAspect > imageAspect ? 1.0f : viewAspect / imageAspect;
            const float scaleY = viewAspect > imageAspect ? imageAspect / viewAspect : 1.0f;
            Store(uniforms.Backdrop, scaleX, scaleY, (1.0f - scaleX) * 0.5f, (1.0f - scaleY) * 0.5f);
        }
    }

    Store(uniforms.Params, std::max(rays.OcclusionRadius, 0.01f), std::clamp(rays.MaxRoughness, 0.0f, 1.0f),
          camera.FarPlane, LongestHistory);
    uniforms.Settings[1] = m_FrameIndex;
    uniforms.Settings[2] = m_Rays->GetLightCount();
    uniforms.Settings[3] = m_Rays->GetInstanceCount();

    const std::vector<SDL_GPUBuffer*> buffers = { m_Rays->GetTopNodes(),  m_Rays->GetNodes(),
                                                  m_Rays->GetTriangles(), m_Rays->GetShading(),
                                                  m_Rays->GetInstances(), m_Rays->GetLights() };
    SDL_GPUTexture* atlas = m_Rays->GetAtlas();

    // --- path tracing ---------------------------------------------------------
    if (pathTracing)
    {
        if (!EnsurePathTargets())
        {
            return false;
        }

        Store(uniforms.Size, static_cast<float>(m_Width), static_cast<float>(m_Height),
              1.0f / static_cast<float>(m_Width), 1.0f / static_cast<float>(m_Height));

        // Anything that would change what a path finds starts the sum again:
        // the scene, the camera, the light, the settings.
        const PathTracingSettings& path = settings.PathTracing;
        uniforms.Progress[2] = static_cast<uint32_t>(std::clamp(path.MaxBounces, 0, 32));
        RayUniforms stable = uniforms;
        stable.Settings[1] = 0;
        uint64_t fingerprint = HashBytes(m_Rays->GetFingerprint(), &stable, sizeof(stable));
        const uint64_t backdropKey = (static_cast<uint64_t>(extras.Background.Index) << 32) |
                                     extras.Background.Generation;
        fingerprint = HashBytes(fingerprint, &backdropKey, sizeof(backdropKey));
        if (fingerprint != m_PathFingerprint)
        {
            m_PathFingerprint = fingerprint;
            m_PathSamples = 0;
        }

        // Converged: the image holds.
        if (m_PathSamples >= static_cast<uint32_t>(std::max(path.MaxSamples, 1)))
        {
            return true;
        }

        const uint32_t samples = static_cast<uint32_t>(std::clamp(path.SamplesPerFrame, 1, 64));
        uniforms.Progress[0] = m_PathSamples;
        uniforms.Progress[1] = samples;

        SDL_GPUTexture* previous = m_PathSum[static_cast<size_t>(m_PathSumIndex)];
        SDL_GPUTexture* next = m_PathSum[static_cast<size_t>(1 - m_PathSumIndex)];
        SDL_GPUTexture* backdropTexture = backdrop ? textures.GetGpuTexture(m_Device, extras.Background) : m_White;

        Dispatch(commandBuffer, m_PathTracePipeline, { previous, backdropTexture, atlas },
                 { m_PointSampler, m_LinearClampSampler, m_LinearClampSampler }, buffers, { next, m_PathImage },
                 &uniforms, sizeof(uniforms), m_Width, m_Height);

        m_PathSumIndex = 1 - m_PathSumIndex;
        m_PathSamples += samples;
        return true;
    }

    // --- hybrid: shadows, occlusion, reflections --------------------------------
    if (m_PrepassDepth == nullptr || m_Normals == nullptr || m_Motion == nullptr)
    {
        return false;
    }

    const float scale = std::clamp(rays.ResolutionScale, 0.25f, 1.0f);
    const int width = std::max(1, static_cast<int>(std::lround(static_cast<float>(m_Width) * scale)));
    const int height = std::max(1, static_cast<int>(std::lround(static_cast<float>(m_Height) * scale)));
    if (!EnsureRayTargets(width, height))
    {
        return false;
    }

    uint32_t effects = (rays.Shadows ? EffectShadows : 0u) | (rays.AmbientOcclusion ? EffectOcclusion : 0u) |
                       (rays.Reflections ? EffectReflections : 0u);

    // A history made with other effects, or none at all, is no history.
    if (!m_RayHistoryValid || effects != m_RayEffects)
    {
        effects |= EffectRestart;
    }
    m_RayEffects = effects & ~EffectRestart;

    uniforms.Settings[0] = effects;
    Store(uniforms.Size, static_cast<float>(width), static_cast<float>(height), 1.0f / static_cast<float>(width),
          1.0f / static_cast<float>(height));

    Dispatch(commandBuffer, m_RayHybridPipeline, { m_PrepassDepth, m_Normals, atlas },
             { m_PointSampler, m_PointSampler, m_LinearClampSampler }, buffers, { m_RayLighting, m_RayReflection },
             &uniforms, sizeof(uniforms), width, height);

    SDL_GPUTexture* history = m_RayHistory[static_cast<size_t>(m_RayHistoryIndex)];
    SDL_GPUTexture* historyReflection = m_RayHistoryReflection[static_cast<size_t>(m_RayHistoryIndex)];
    SDL_GPUTexture* nextHistory = m_RayHistory[static_cast<size_t>(1 - m_RayHistoryIndex)];
    SDL_GPUTexture* nextHistoryReflection = m_RayHistoryReflection[static_cast<size_t>(1 - m_RayHistoryIndex)];

    Dispatch(commandBuffer, m_RayTemporalPipeline,
             { m_RayLighting, m_RayReflection, history, historyReflection, m_Motion, m_White },
             { m_PointSampler, m_PointSampler, m_LinearClampSampler, m_LinearClampSampler, m_PointSampler,
               m_PointSampler },
             {}, { nextHistory, nextHistoryReflection }, &uniforms, sizeof(uniforms), width, height);

    // Two steps across the screen, the second reaching twice as far.
    uniforms.Progress[3] = 1;
    Dispatch(commandBuffer, m_RayFilterPipeline,
             { nextHistory, nextHistoryReflection, m_Normals, m_White, m_White, m_White },
             { m_PointSampler, m_PointSampler, m_PointSampler, m_PointSampler, m_PointSampler, m_PointSampler }, {},
             { m_RayFiltered[0], m_RayFilteredReflection[0] }, &uniforms, sizeof(uniforms), width, height);

    uniforms.Progress[3] = 2;
    Dispatch(commandBuffer, m_RayFilterPipeline,
             { m_RayFiltered[0], m_RayFilteredReflection[0], m_Normals, m_White, m_White, m_White },
             { m_PointSampler, m_PointSampler, m_PointSampler, m_PointSampler, m_PointSampler, m_PointSampler }, {},
             { m_RayFiltered[1], m_RayFilteredReflection[1] }, &uniforms, sizeof(uniforms), width, height);

    m_RayHistoryIndex = 1 - m_RayHistoryIndex;
    m_RayHistoryValid = true;

    // What the surfaces read: traced where it was traced.
    if (rays.Shadows)
    {
        m_ScreenSunlight = m_RayFiltered[1];
    }
    if (rays.AmbientOcclusion)
    {
        m_ScreenOcclusion = m_RayFiltered[1];
    }
    if (rays.Reflections)
    {
        m_ScreenReflections = m_RayFilteredReflection[1];
    }
    return true;
}

} // namespace ludifex::detail
