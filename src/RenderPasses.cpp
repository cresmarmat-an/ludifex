// The frame's full-screen passes and the targets they need: the backdrop, TAA,
// the user's post-process materials, tone mapping and grading, FXAA, and the
// scale to the output. Also the shadow cascades' fit around the camera.

#include "Render3D.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ludifex::detail
{
namespace
{

// Mirrors PostUniforms in post.hlsl and world_post.hlsli.
struct PostUniforms
{
    float TexelSize[4];
    float Params[4];
};

Vec3 Subtract(const Vec3& a, const Vec3& b)
{
    return Vec3{ a.X - b.X, a.Y - b.Y, a.Z - b.Z };
}

Vec3 Add(const Vec3& a, const Vec3& b)
{
    return Vec3{ a.X + b.X, a.Y + b.Y, a.Z + b.Z };
}

Vec3 Scale(const Vec3& v, float s)
{
    return Vec3{ v.X * s, v.Y * s, v.Z * s };
}

float Length(const Vec3& v)
{
    return std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z);
}

Vec3 TransformPoint(const Mat4& matrix, const Vec3& point)
{
    const float* m = matrix.M;
    return Vec3{ m[0] * point.X + m[4] * point.Y + m[8] * point.Z + m[12],
                 m[1] * point.X + m[5] * point.Y + m[9] * point.Z + m[13],
                 m[2] * point.X + m[6] * point.Y + m[10] * point.Z + m[14] };
}

// A white balance as a multiplier on linear colour: temperature trades blue
// for orange, tint green for magenta, and the result is scaled so the image's
// brightness stays where it was.
void WhiteBalance(float temperature, float tint, float out[3])
{
    temperature = std::clamp(temperature, -1.0f, 1.0f);
    tint = std::clamp(tint, -1.0f, 1.0f);
    float r = (1.0f + 0.18f * temperature) * (1.0f + 0.07f * tint);
    float g = 1.0f - 0.14f * tint;
    float b = (1.0f - 0.18f * temperature) * (1.0f + 0.07f * tint);
    const float luminance = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    out[0] = r / luminance;
    out[1] = g / luminance;
    out[2] = b / luminance;
}

} // namespace

bool Renderer3D::EnsurePostTargets(bool needHdrWork, bool needLdrWork, bool needHistory, bool needMotion)
{
    const SDL_GPUTextureUsageFlags sampledTarget = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;

    auto Ensure = [&](SDL_GPUTexture*& texture, SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage) {
        if (texture == nullptr)
        {
            texture = CreateTarget(m_Device, format, m_Width, m_Height, SDL_GPU_SAMPLECOUNT_1, usage);
        }
        return texture != nullptr;
    };

    // Created on first need and kept: a frame that stops using an effect costs
    // nothing, and turning it back on costs no allocation.
    if (needHdrWork && (!Ensure(m_HdrWork[0], m_SceneFormat, sampledTarget) ||
                        !Ensure(m_HdrWork[1], m_SceneFormat, sampledTarget)))
    {
        return false;
    }
    if (needLdrWork && (!Ensure(m_LdrWork[0], SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, sampledTarget) ||
                        !Ensure(m_LdrWork[1], SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, sampledTarget)))
    {
        return false;
    }
    if (needHistory)
    {
        if (m_History[0] == nullptr || m_History[1] == nullptr)
        {
            m_HistoryValid = false;
        }
        if (!Ensure(m_History[0], m_SceneFormat, sampledTarget) || !Ensure(m_History[1], m_SceneFormat, sampledTarget))
        {
            return false;
        }
    }
    else
    {
        // History left over from an earlier TAA frame is stale by the time TAA
        // comes back on.
        m_HistoryValid = false;
    }
    if (needMotion && (!Ensure(m_Motion, SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT, sampledTarget) ||
                       !Ensure(m_Normals, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, sampledTarget) ||
                       !Ensure(m_PrepassDepth, m_SampledDepthFormat,
                               SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)))
    {
        return false;
    }
    return true;
}

bool Renderer3D::EnsureShadowMap(int resolution)
{
    if (m_ShadowMap != nullptr && m_ShadowResolution == resolution)
    {
        return true;
    }

    if (m_ShadowMap != nullptr)
    {
        SDL_ReleaseGPUTexture(m_Device, m_ShadowMap);
        m_ShadowMap = nullptr;
    }

    m_ShadowMap = CreateTarget(m_Device, m_SampledDepthFormat, resolution, resolution, SDL_GPU_SAMPLECOUNT_1,
                               SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
    m_ShadowResolution = m_ShadowMap != nullptr ? resolution : 0;
    return m_ShadowMap != nullptr;
}

Mat4 Renderer3D::FitShadow(const FrameCamera& camera, const Vec3& lightDirectionIn, float nearDepth,
                           float farDepth, int resolution, float* outTexelWorld)
{
    // The slice of the view, from nearDepth out to farDepth, as eight corners
    // in the world.
    const Vec3 forward = NormalizeVector(camera.Forward);
    const Vec3 right = NormalizeVector(CrossProduct(forward, camera.Up));
    const Vec3 up = CrossProduct(right, forward);

    const float aspect = static_cast<float>(m_Width) / static_cast<float>(std::max(m_Height, 1));
    const float tanY = std::tan(camera.FieldOfViewRadians * 0.5f);
    const float tanX = tanY * aspect;

    Vec3 corners[8];
    int count = 0;
    for (float depth : { nearDepth, farDepth })
    {
        const Vec3 centre = Add(camera.Position, Scale(forward, depth));
        const float halfWidth = tanX * depth;
        const float halfHeight = tanY * depth;
        for (float sx : { -1.0f, 1.0f })
        {
            for (float sy : { -1.0f, 1.0f })
            {
                corners[count++] = Add(centre, Add(Scale(right, sx * halfWidth), Scale(up, sy * halfHeight)));
            }
        }
    }

    // A bounding sphere rather than a tight box: its size does not change as
    // the camera turns, so shadow edges do not swim when the view rotates.
    Vec3 centre{};
    for (const Vec3& corner : corners)
    {
        centre = Add(centre, corner);
    }
    centre = Scale(centre, 1.0f / 8.0f);

    float radius = 0.0f;
    for (const Vec3& corner : corners)
    {
        radius = std::max(radius, Length(Subtract(corner, centre)));
    }
    radius = std::ceil(radius * 4.0f) / 4.0f;

    const Vec3 lightDirection = NormalizeVector(lightDirectionIn);
    const Vec3 lightUp = std::fabs(lightDirection.Y) > 0.99f ? Vec3{ 1.0f, 0.0f, 0.0f } : Vec3{ 0.0f, 1.0f, 0.0f };

    // A view looking along the light from the origin, so the sphere's centre
    // can be snapped to whole texels in light space. Moving the camera then
    // moves the shadow map in texel steps, and edges stay put instead of
    // crawling.
    const Mat4 lightView = Mat4::LookAt(Vec3{}, lightDirection, lightUp);
    Vec3 centreLight = TransformPoint(lightView, centre);

    const float texel = 2.0f * radius / static_cast<float>(resolution);
    centreLight.X = std::floor(centreLight.X / texel) * texel;
    centreLight.Y = std::floor(centreLight.Y / texel) * texel;
    if (outTexelWorld != nullptr)
    {
        *outTexelWorld = texel;
    }

    // Casters well outside the camera's view can still shadow it, so the box
    // reaches back toward the light well past the sphere.
    constexpr float CasterReach = 100.0f;
    const float nearPlane = -centreLight.Z - radius - CasterReach;
    const float farPlane = -centreLight.Z + radius;

    const Mat4 projection = Mat4::Orthographic(centreLight.X - radius, centreLight.X + radius, centreLight.Y - radius,
                                               centreLight.Y + radius, nearPlane, farPlane);
    return projection * lightView;
}

void Renderer3D::PlanShadows(const FrameCamera& camera, const RenderSettings& settings)
{
    const float distance = std::max(std::min(settings.Shadows.Distance, camera.FarPlane), camera.NearPlane + 0.01f);
    m_Cascades = std::clamp(settings.Shadows.Cascades, 1, 4);

    // A map a side of 8192 texels is the most any device is sure to take, and
    // four cascades share a map twice the side of one.
    const int largest = m_Cascades > 1 ? 4096 : 8192;
    m_CascadeResolution = std::clamp(settings.Shadows.Resolution, 256, largest);

    // Where each cascade ends: part logarithmic, which gives the near slices
    // more resolution, and part even, so the far ones do not grow too long.
    // The logarithm starts half a metre out instead of at the near plane, so
    // the first cascade is not wasted on the space right in front of the
    // camera.
    const float start = std::max(camera.NearPlane, 0.5f);
    const float blend = 0.7f;
    for (int cascade = 0; cascade < m_Cascades; ++cascade)
    {
        const float fraction = static_cast<float>(cascade + 1) / static_cast<float>(m_Cascades);
        const float logarithmic = start * std::pow(distance / start, fraction);
        const float even = start + (distance - start) * fraction;
        m_CascadeEnds[static_cast<size_t>(cascade)] = blend * logarithmic + (1.0f - blend) * even;
    }
    m_CascadeEnds[static_cast<size_t>(m_Cascades - 1)] = distance;

    // Each cascade reaches back over the last stretch of the one before, where
    // the shader blends the two.
    float previousEnd = camera.NearPlane;
    float previousLength = 0.0f;
    for (int cascade = 0; cascade < m_Cascades; ++cascade)
    {
        const float end = m_CascadeEnds[static_cast<size_t>(cascade)];
        const float from = std::max(camera.NearPlane, previousEnd - previousLength * 0.15f);
        float texel = 0.0f;
        m_CascadeMatrices[static_cast<size_t>(cascade)] =
            FitShadow(camera, settings.Light.Direction, from, end, m_CascadeResolution, &texel);

        // How far a look-up is pushed off its surface: one and a half texels
        // of this cascade, enough to stop a surface shadowing itself.
        m_CascadeOffsets[static_cast<size_t>(cascade)] = texel * 1.5f;

        previousLength = end - previousEnd;
        previousEnd = end;
    }

    // Around every cascade at once, for deciding which objects cast.
    m_ShadowViewProjection = FitShadow(camera, settings.Light.Direction, camera.NearPlane, distance,
                                       m_CascadeResolution);
}

void Renderer3D::DrawBackdrop(SDL_GPUCommandBuffer* commandBuffer, SDL_GPURenderPass* pass, TextureId background)
{
    WorldTextureStore& textures = GetWorldTextures();
    const Vec2 size = textures.GetSize(background);
    if (size.X <= 0.0f || size.Y <= 0.0f)
    {
        return;
    }

    // Cover: fill the view, cropping whichever axis of the image overhangs.
    const float imageAspect = size.X / size.Y;
    const float viewAspect = static_cast<float>(m_Width) / static_cast<float>(m_Height);

    PostUniforms uniforms{};
    uniforms.TexelSize[0] = 1.0f / static_cast<float>(m_Width);
    uniforms.TexelSize[1] = 1.0f / static_cast<float>(m_Height);
    uniforms.TexelSize[2] = static_cast<float>(m_Width);
    uniforms.TexelSize[3] = static_cast<float>(m_Height);
    if (viewAspect > imageAspect)
    {
        uniforms.Params[0] = 1.0f;
        uniforms.Params[1] = imageAspect / viewAspect;
    }
    else
    {
        uniforms.Params[0] = viewAspect / imageAspect;
        uniforms.Params[1] = 1.0f;
    }
    uniforms.Params[2] = (1.0f - uniforms.Params[0]) * 0.5f;
    uniforms.Params[3] = (1.0f - uniforms.Params[1]) * 0.5f;

    SDL_BindGPUGraphicsPipeline(pass, m_BackdropPipeline);

    SDL_GPUTextureSamplerBinding binding{ textures.GetGpuTexture(m_Device, background), m_LinearClampSampler };
    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
    SDL_PushGPUFragmentUniformData(commandBuffer, 0, &uniforms, sizeof(uniforms));

    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    ++m_DrawCalls;
}

void Renderer3D::FullscreenPass(SDL_GPUCommandBuffer* commandBuffer, SDL_GPUGraphicsPipeline* pipeline,
                                SDL_GPUTexture* target, const std::vector<SDL_GPUTexture*>& sources,
                                const std::vector<SDL_GPUSampler*>& samplers, const void* uniforms,
                                size_t uniformSize, const void* materialUniforms, size_t materialSize, bool keep)
{
    SDL_GPUColorTargetInfo color{};
    color.texture = target;
    color.load_op = keep ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_DONT_CARE;
    color.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commandBuffer, &color, 1, nullptr);
    if (pass == nullptr)
    {
        return;
    }

    SDL_BindGPUGraphicsPipeline(pass, pipeline);

    std::vector<SDL_GPUTextureSamplerBinding> bindings(sources.size());
    for (size_t index = 0; index < sources.size(); ++index)
    {
        bindings[index].texture = sources[index];
        bindings[index].sampler = samplers[index];
    }
    if (!bindings.empty())
    {
        SDL_BindGPUFragmentSamplers(pass, 0, bindings.data(), static_cast<uint32_t>(bindings.size()));
    }

    SDL_PushGPUFragmentUniformData(commandBuffer, 0, uniforms, static_cast<uint32_t>(uniformSize));
    if (materialUniforms != nullptr)
    {
        SDL_PushGPUFragmentUniformData(commandBuffer, 1, materialUniforms, static_cast<uint32_t>(materialSize));
    }

    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    ++m_DrawCalls;

    SDL_EndGPURenderPass(pass);
}

void Renderer3D::RunPostChain(SDL_GPUCommandBuffer* commandBuffer, const FrameCamera& camera,
                              const RenderSettings& settings, const RenderExtras& extras, SDL_GPUTexture* source,
                              bool temporal)
{
    PostUniforms uniforms{};
    uniforms.TexelSize[0] = 1.0f / static_cast<float>(m_Width);
    uniforms.TexelSize[1] = 1.0f / static_cast<float>(m_Height);
    uniforms.TexelSize[2] = static_cast<float>(m_Width);
    uniforms.TexelSize[3] = static_cast<float>(m_Height);

    // What a user post-process sees in DepthParams.
    PostUniforms userUniforms = uniforms;
    userUniforms.Params[0] = camera.NearPlane;
    userUniforms.Params[1] = camera.FarPlane;
    userUniforms.Params[2] = camera.Orthographic ? 1.0f : 0.0f;

    SDL_GPUTexture* current = source;

    // --- temporal anti-aliasing ----------------------------------------------
    if (temporal && settings.Mode == AntiAliasing::Temporal && m_History[0] != nullptr && m_Motion != nullptr)
    {
        SDL_GPUTexture* history = m_History[m_HistoryIndex];
        SDL_GPUTexture* target = m_History[1 - m_HistoryIndex];

        // With no usable history the first frame takes the current image
        // whole; after that each frame contributes a tenth.
        PostUniforms taa = uniforms;
        taa.Params[0] = m_HistoryValid ? 0.1f : 1.0f;

        FullscreenPass(commandBuffer, m_TaaPipeline, target, { current, history, m_Motion },
                       { m_LinearClampSampler, m_LinearClampSampler, m_PointSampler }, &taa, sizeof(taa), nullptr, 0);

        current = target;
        m_HistoryIndex = 1 - m_HistoryIndex;
        m_HistoryValid = true;
    }

    std::vector<std::pair<SDL_GPUGraphicsPipeline*, WorldMaterialRecord*>> linearPasses;
    std::vector<std::pair<SDL_GPUGraphicsPipeline*, WorldMaterialRecord*>> displayPasses;
    for (const PostProcessEntry& entry : extras.PostProcess)
    {
        WorldMaterialRecord* record = GetWorldMaterials().Resolve(entry.Material);
        if (record == nullptr)
        {
            continue;
        }
        const bool linear = (entry.Point == PassPoint::BeforeToneMap);
        SDL_GPUGraphicsPipeline* pipeline = PostPipelineFor(static_cast<int>(entry.Material.Index), linear);
        if (pipeline == nullptr)
        {
            continue;
        }
        (linear ? linearPasses : displayPasses).push_back({ pipeline, record });
    }

    // --- the user's passes in linear light -----------------------------------
    SDL_GPUTexture* depth = m_PrepassDepth != nullptr ? m_PrepassDepth : m_NoShadow;
    for (const auto& [pipeline, record] : linearPasses)
    {
        SDL_GPUTexture* target = (current == m_HdrWork[0]) ? m_HdrWork[1] : m_HdrWork[0];
        FullscreenPass(commandBuffer, pipeline, target, { current, depth }, { m_LinearClampSampler, m_PointSampler },
                       &userUniforms, sizeof(userUniforms), &record->Uniforms, sizeof(MaterialUniformBlock));
        current = target;
    }

    // --- bloom and exposure, measured on the finished linear image ------------
    SDL_GPUTexture* bloom = m_Clear;
    float bloomIntensity = 0.0f;
    if (settings.Bloom.Enabled && settings.Bloom.Intensity > 0.0f)
    {
        RunBloom(commandBuffer, current, settings);
        if (!m_Bloom.empty())
        {
            bloom = m_Bloom[0];
            bloomIntensity = settings.Bloom.Intensity;
        }
    }

    SDL_GPUTexture* exposure = m_White;
    bool automatic = false;
    if (settings.AutoExposure.Enabled)
    {
        RunExposure(commandBuffer, current, settings);
        if (m_Exposure[static_cast<size_t>(m_ExposureIndex)] != nullptr && m_ExposureValid)
        {
            exposure = m_Exposure[static_cast<size_t>(m_ExposureIndex)];
            automatic = true;
        }
    }
    else
    {
        m_ExposureValid = false;
    }

    // --- tone mapping, FXAA, the user's display-space passes, and the scale ---
    //
    // The last of these writes the output texture itself, so the image a host
    // wrapped once is always the finished frame.
    const bool scaled = m_Width != m_OutputWidth || m_Height != m_OutputHeight;
    const bool fxaa = settings.Mode == AntiAliasing::Fast || settings.Mode == AntiAliasing::High;
    const size_t total = 1 + (fxaa ? 1 : 0) + displayPasses.size();
    size_t step = 0;

    auto NextTarget = [&](SDL_GPUTexture* previous) {
        ++step;
        if (step == total && !scaled)
        {
            return m_Output;
        }
        return (previous == m_LdrWork[0]) ? m_LdrWork[1] : m_LdrWork[0];
    };

    {
        const ColorGrading& grading = settings.Grading;

        EffectUniforms tone{};
        std::memcpy(tone.TexelSize, uniforms.TexelSize, sizeof(tone.TexelSize));
        tone.Params[0] = std::max(0.0f, settings.Exposure);
        tone.Params[1] = bloomIntensity;
        tone.Params[2] = automatic ? 1.0f : 0.0f;
        tone.Params[3] = grading.Curve == ToneCurve::Filmic ? 1.0f : 0.0f;

        WhiteBalance(grading.Temperature, grading.Tint, tone.Extra[0]);
        tone.Extra[0][3] = std::max(0.0f, grading.Contrast);
        tone.Extra[1][0] = grading.Lift.R;
        tone.Extra[1][1] = grading.Lift.G;
        tone.Extra[1][2] = grading.Lift.B;
        tone.Extra[1][3] = std::max(0.0f, grading.Saturation);
        tone.Extra[2][0] = grading.Gain.R;
        tone.Extra[2][1] = grading.Gain.G;
        tone.Extra[2][2] = grading.Gain.B;
        tone.Extra[2][3] = std::clamp(grading.Vignette, 0.0f, 1.0f);

        SDL_GPUTexture* target = NextTarget(nullptr);
        FullscreenPass(commandBuffer, m_ToneMapPipeline, target, { current, bloom, exposure },
                       { m_LinearClampSampler, m_LinearClampSampler, m_PointSampler }, &tone, sizeof(tone), nullptr,
                       0);
        current = target;
    }

    if (fxaa)
    {
        SDL_GPUTexture* target = NextTarget(current);
        FullscreenPass(commandBuffer, m_FxaaPipeline, target, { current }, { m_LinearClampSampler }, &uniforms,
                       sizeof(uniforms), nullptr, 0);
        current = target;
    }

    for (const auto& [pipeline, record] : displayPasses)
    {
        SDL_GPUTexture* target = NextTarget(current);
        FullscreenPass(commandBuffer, pipeline, target, { current, depth }, { m_LinearClampSampler, m_PointSampler },
                       &userUniforms, sizeof(userUniforms), &record->Uniforms, sizeof(MaterialUniformBlock));
        current = target;
    }

    // Drawn at another size, the finished image is filtered to the output's,
    // and a little sharpened when it is enlarged.
    if (scaled)
    {
        EffectUniforms scale{};
        std::memcpy(scale.TexelSize, uniforms.TexelSize, sizeof(scale.TexelSize));
        scale.Params[0] = m_Width < m_OutputWidth ? 0.35f : 0.0f;
        FullscreenPass(commandBuffer, m_UpscalePipeline, m_Output, { current }, { m_LinearClampSampler }, &scale,
                       sizeof(scale), nullptr, 0);
    }
}

} // namespace ludifex::detail
