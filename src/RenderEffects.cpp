// The frame's lighting and image effects: screen-space ambient occlusion,
// bloom, and automatic exposure. Their shaders are in effects.hlsl, and the
// tone map that uses what they make is in RenderPasses.cpp.

#include "Render3D.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ludifex::detail
{
namespace
{

void CopyMatrix(float out[16], const Mat4& matrix)
{
    std::memcpy(out, matrix.M, sizeof(float) * 16);
}

// The brightness the eye settles an average scene at, before compensation.
// A little above the photographer's middle grey, because ludifex's neutral
// tone curve shows the authored colours unchanged below 0.9.
constexpr float SettledBrightness = 0.25f;

// The measured brightness is written at this size, then reduced to its
// average by the mip chain.
constexpr int LuminanceSize = 256;

} // namespace

bool Renderer3D::CreateEffectPipelines()
{
    const SDL_GPUTextureFormat occlusionFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

    // Half-float for the logarithm of the brightness, which is negative in
    // the dark; single-channel where the device renders to it.
    const SDL_GPUTextureFormat luminanceFormat =
        SDL_GPUTextureSupportsFormat(m_Device, SDL_GPU_TEXTUREFORMAT_R16_FLOAT, SDL_GPU_TEXTURETYPE_2D,
                                     SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET)
            ? SDL_GPU_TEXTUREFORMAT_R16_FLOAT
            : SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;

    m_OcclusionPipeline = CreateFullscreenPipeline(m_OcclusionFragment, occlusionFormat, SDL_GPU_SAMPLECOUNT_1);
    m_OcclusionBlurPipeline =
        CreateFullscreenPipeline(m_OcclusionBlurFragment, occlusionFormat, SDL_GPU_SAMPLECOUNT_1);
    m_BloomPrefilterPipeline =
        CreateFullscreenPipeline(m_BloomPrefilterFragment, m_SceneFormat, SDL_GPU_SAMPLECOUNT_1);
    m_BloomDownPipeline = CreateFullscreenPipeline(m_BloomDownFragment, m_SceneFormat, SDL_GPU_SAMPLECOUNT_1);
    m_BloomUpPipeline =
        CreateFullscreenPipeline(m_BloomUpFragment, m_SceneFormat, SDL_GPU_SAMPLECOUNT_1, false, true);
    m_LuminancePipeline = CreateFullscreenPipeline(m_LuminanceFragment, luminanceFormat, SDL_GPU_SAMPLECOUNT_1);
    m_AdaptPipeline =
        CreateFullscreenPipeline(m_AdaptFragment, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, SDL_GPU_SAMPLECOUNT_1);
    m_UpscalePipeline =
        CreateFullscreenPipeline(m_UpscaleFragment, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_SAMPLECOUNT_1);

    const std::pair<SDL_GPUGraphicsPipeline*, const char*> pipelines[] = {
        { m_OcclusionPipeline, "ambient occlusion" }, { m_OcclusionBlurPipeline, "occlusion blur" },
        { m_BloomPrefilterPipeline, "bloom threshold" }, { m_BloomDownPipeline, "bloom reduction" },
        { m_BloomUpPipeline, "bloom spread" },        { m_LuminancePipeline, "brightness measure" },
        { m_AdaptPipeline, "exposure" },              { m_UpscalePipeline, "scale" },
    };
    for (const auto& [pipeline, name] : pipelines)
    {
        if (pipeline == nullptr)
        {
            LogMessage(LogLevel::Error, "render", "Could not create the %s pipeline: %s", name, SDL_GetError());
            return false;
        }
    }

    // The luminance is created with its pipeline, since both need its format.
    uint32_t levels = 1;
    while ((LuminanceSize >> levels) > 0)
    {
        ++levels;
    }
    m_LuminanceLevels = levels;
    m_Luminance = CreateTarget(m_Device, luminanceFormat, LuminanceSize, LuminanceSize, SDL_GPU_SAMPLECOUNT_1,
                               SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, levels);
    for (SDL_GPUTexture*& exposure : m_Exposure)
    {
        exposure = CreateTarget(m_Device, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, 1, 1, SDL_GPU_SAMPLECOUNT_1,
                                SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
    }
    return m_Luminance != nullptr && m_Exposure[0] != nullptr && m_Exposure[1] != nullptr;
}

void Renderer3D::ReleaseEffectPipelines()
{
    for (SDL_GPUGraphicsPipeline** pipeline :
         { &m_OcclusionPipeline, &m_OcclusionBlurPipeline, &m_BloomPrefilterPipeline, &m_BloomDownPipeline,
           &m_BloomUpPipeline, &m_LuminancePipeline, &m_AdaptPipeline, &m_UpscalePipeline })
    {
        if (*pipeline != nullptr && m_Device != nullptr)
        {
            SDL_ReleaseGPUGraphicsPipeline(m_Device, *pipeline);
        }
        *pipeline = nullptr;
    }
}

void Renderer3D::ReleaseEffectTargets()
{
    for (SDL_GPUTexture*& texture : m_Occlusion)
    {
        if (texture != nullptr)
        {
            SDL_ReleaseGPUTexture(m_Device, texture);
            texture = nullptr;
        }
    }
    for (SDL_GPUTexture* texture : m_Bloom)
    {
        SDL_ReleaseGPUTexture(m_Device, texture);
    }
    m_Bloom.clear();
    m_BloomSizes.clear();
}

void Renderer3D::RunOcclusion(SDL_GPUCommandBuffer* commandBuffer, const FrameCamera& camera,
                              const RenderSettings& settings)
{
    // Half the resolution each way: occlusion is soft, and a quarter of the
    // pixels is most of the cost saved for a difference that shows only as a
    // slightly softer edge where the surfaces read it back.
    const int width = std::max(1, m_Width / 2);
    const int height = std::max(1, m_Height / 2);

    for (SDL_GPUTexture*& texture : m_Occlusion)
    {
        if (texture == nullptr)
        {
            texture = CreateTarget(m_Device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, width, height,
                                   SDL_GPU_SAMPLECOUNT_1,
                                   SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
        }
        if (texture == nullptr || m_PrepassDepth == nullptr || m_Normals == nullptr)
        {
            return;
        }
    }

    const AmbientOcclusionSettings& occlusion = settings.AmbientOcclusion;
    const Mat4 viewProjection = camera.Projection * camera.View;
    const Vec3 forward = NormalizeVector(camera.Forward);

    EffectUniforms uniforms{};
    CopyMatrix(uniforms.ViewProjection, viewProjection);
    CopyMatrix(uniforms.InverseViewProjection, InvertMatrix(viewProjection));
    uniforms.TexelSize[0] = 1.0f / static_cast<float>(width);
    uniforms.TexelSize[1] = 1.0f / static_cast<float>(height);
    uniforms.TexelSize[2] = static_cast<float>(width);
    uniforms.TexelSize[3] = static_cast<float>(height);
    uniforms.Params[0] = std::max(occlusion.Radius, 0.01f);
    uniforms.Params[1] = std::max(occlusion.Intensity, 0.0f);
    uniforms.Params[2] = static_cast<float>(std::clamp(occlusion.Samples, 4, 32));
    uniforms.Extra[0][0] = camera.Position.X;
    uniforms.Extra[0][1] = camera.Position.Y;
    uniforms.Extra[0][2] = camera.Position.Z;
    uniforms.Extra[1][0] = forward.X;
    uniforms.Extra[1][1] = forward.Y;
    uniforms.Extra[1][2] = forward.Z;

    FullscreenPass(commandBuffer, m_OcclusionPipeline, m_Occlusion[0], { m_PrepassDepth, m_Normals },
                   { m_PointSampler, m_PointSampler }, &uniforms, sizeof(uniforms), nullptr, 0);

    // Across, then down: the sample pattern turns from pixel to pixel, and the
    // two passes average it into a smooth shade that stops at edges.
    EffectUniforms blur{};
    blur.Params[2] = camera.NearPlane;
    blur.Params[3] = camera.FarPlane;

    blur.Params[0] = 1.0f / static_cast<float>(width);
    blur.Params[1] = 0.0f;
    FullscreenPass(commandBuffer, m_OcclusionBlurPipeline, m_Occlusion[1], { m_Occlusion[0], m_PrepassDepth },
                   { m_PointSampler, m_PointSampler }, &blur, sizeof(blur), nullptr, 0);

    blur.Params[0] = 0.0f;
    blur.Params[1] = 1.0f / static_cast<float>(height);
    FullscreenPass(commandBuffer, m_OcclusionBlurPipeline, m_Occlusion[0], { m_Occlusion[1], m_PrepassDepth },
                   { m_PointSampler, m_PointSampler }, &blur, sizeof(blur), nullptr, 0);
}

void Renderer3D::RunBloom(SDL_GPUCommandBuffer* commandBuffer, SDL_GPUTexture* source,
                          const RenderSettings& settings)
{
    // Six levels, from half the scene down, or fewer for a small one: the
    // smallest still needs a few texels to blur.
    if (m_Bloom.empty())
    {
        int width = m_Width / 2;
        int height = m_Height / 2;
        while (m_Bloom.size() < 6 && width >= 4 && height >= 4)
        {
            SDL_GPUTexture* level = CreateTarget(m_Device, m_SceneFormat, width, height, SDL_GPU_SAMPLECOUNT_1,
                                                 SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
            if (level == nullptr)
            {
                break;
            }
            m_Bloom.push_back(level);
            m_BloomSizes.push_back({ width, height });
            width /= 2;
            height /= 2;
        }
        if (m_Bloom.empty())
        {
            return;
        }
    }

    const BloomSettings& bloom = settings.Bloom;
    EffectUniforms uniforms{};
    uniforms.Params[0] = std::max(bloom.Threshold, 0.0f);
    uniforms.Params[1] = std::max(bloom.Threshold * 0.5f, 1e-3f);
    uniforms.Params[2] = std::clamp(bloom.Radius, 0.1f, 4.0f);

    auto TexelOf = [&](int width, int height) {
        uniforms.TexelSize[0] = 1.0f / static_cast<float>(width);
        uniforms.TexelSize[1] = 1.0f / static_cast<float>(height);
        uniforms.TexelSize[2] = static_cast<float>(width);
        uniforms.TexelSize[3] = static_cast<float>(height);
    };

    TexelOf(m_Width, m_Height);
    FullscreenPass(commandBuffer, m_BloomPrefilterPipeline, m_Bloom[0], { source }, { m_LinearClampSampler },
                   &uniforms, sizeof(uniforms), nullptr, 0);

    for (size_t level = 1; level < m_Bloom.size(); ++level)
    {
        TexelOf(m_BloomSizes[level - 1].first, m_BloomSizes[level - 1].second);
        FullscreenPass(commandBuffer, m_BloomDownPipeline, m_Bloom[level], { m_Bloom[level - 1] },
                       { m_LinearClampSampler }, &uniforms, sizeof(uniforms), nullptr, 0);
    }

    // Back up: each level's blur added onto the level above, so the top holds
    // every level's glow, tight and wide together.
    for (size_t level = m_Bloom.size() - 1; level > 0; --level)
    {
        TexelOf(m_BloomSizes[level].first, m_BloomSizes[level].second);
        FullscreenPass(commandBuffer, m_BloomUpPipeline, m_Bloom[level - 1], { m_Bloom[level] },
                       { m_LinearClampSampler }, &uniforms, sizeof(uniforms), nullptr, 0, true);
    }
}

void Renderer3D::RunExposure(SDL_GPUCommandBuffer* commandBuffer, SDL_GPUTexture* source,
                             const RenderSettings& settings)
{
    if (m_Luminance == nullptr || m_Exposure[0] == nullptr || m_Exposure[1] == nullptr)
    {
        return;
    }

    EffectUniforms uniforms{};
    uniforms.TexelSize[0] = 1.0f / static_cast<float>(LuminanceSize);
    uniforms.TexelSize[1] = 1.0f / static_cast<float>(LuminanceSize);
    FullscreenPass(commandBuffer, m_LuminancePipeline, m_Luminance, { source }, { m_LinearClampSampler }, &uniforms,
                   sizeof(uniforms), nullptr, 0);

    // The chain's smallest level is the average of the largest.
    SDL_GenerateMipmapsForGPUTexture(commandBuffer, m_Luminance);

    const AutoExposureSettings& automatic = settings.AutoExposure;
    const float minimum = std::max(automatic.Minimum, 1e-3f);
    const float maximum = std::max(automatic.Maximum, minimum);

    // An eye adjusts by a fixed share of the way each moment, which over a
    // frame of any length is this.
    const float move = m_ExposureValid ? 1.0f - std::exp(-std::max(automatic.Speed, 0.0f) * m_FrameSeconds) : 1.0f;

    EffectUniforms adapt{};
    adapt.Params[0] = static_cast<float>(m_LuminanceLevels - 1);
    adapt.Params[1] = move;
    adapt.Params[2] = minimum;
    adapt.Params[3] = maximum;
    adapt.Extra[0][0] = SettledBrightness * std::exp2(automatic.Compensation);

    const int previous = m_ExposureIndex;
    const int next = 1 - m_ExposureIndex;
    FullscreenPass(commandBuffer, m_AdaptPipeline, m_Exposure[static_cast<size_t>(next)],
                   { m_Luminance, m_Exposure[static_cast<size_t>(previous)] }, { m_MipSampler, m_PointSampler },
                   &adapt, sizeof(adapt), nullptr, 0);

    m_ExposureIndex = next;
    m_ExposureValid = true;
}

} // namespace ludifex::detail
