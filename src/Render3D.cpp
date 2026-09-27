#include "Render3D.h"

#include "Backend.h"
#include "Profile.h"
#include "RayScene.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <utility>
#include <vector>

namespace ludifex::detail
{
namespace
{

constexpr float Pi = 3.14159265358979323846f;

// At most this many point lights reach the shader in one frame.
// Clustering is what makes this number affordable: a pixel shades against
// the lights in its own cell, not against all of them.
constexpr size_t MaxPointLights = 256;

// Mirrors InstanceData in world_material.hlsli. The layouts must agree.
struct GpuInstance
{
    float Model[16];
    float PreviousModel[16];
    float BaseColor[4];
    float Surface[4];      // roughness, metallic, occlusion strength, normal strength
    float UVTransform[4];
    float Emission[4];     // linear rgb
    uint32_t Extra[4];
    uint32_t Morph[4];     // first active morph target in the frame's table, how many
};

static_assert(sizeof(GpuInstance) == 224, "instance layout drifted from the shader");

// Objects share a draw only when every map they sample is the same.
bool SameMaps(const PendingInstance& a, const PendingInstance& b)
{
    return a.Texture == b.Texture && a.NormalMap == b.NormalMap && a.MetallicRoughnessMap == b.MetallicRoughnessMap &&
           a.EmissiveMap == b.EmissiveMap && a.OcclusionMap == b.OcclusionMap;
}

constexpr uint32_t InstanceFlagUnlit = 1;
constexpr uint32_t InstanceFlagFlipX = 2;
constexpr uint32_t InstanceFlagSkinned = 4;

// Mirrors DrawUniforms in world.hlsl.
struct DrawUniforms
{
    float ViewProjection[16];
    float PreviousViewProjection[16];
    float Jitter[4];
    uint32_t InstanceOffset = 0;
    uint32_t Padding[3] = {};
};

// Mirrors SceneUniforms in world_material.hlsli.
struct SceneUniforms
{
    float LightDirection[4];
    float LightColor[4];
    float AmbientColor[4];          // w: the sky lights the scene
    float CameraPosition[4];
    float CameraForward[4];         // xyz forward, w near plane
    float ShadowViewProjection[4][16];
    float ShadowSplits[4];
    float ShadowOffsets[4];
    float ShadowParams[4];          // x texel, y softness, z cascades, w cascades a row
    float ShadowFade[4];
    float FogColor[4];
    float FogParams[4];
    float ClusterParams[4];         // x scale, y bias, z tiles across, w tiles down
    float ClusterViewport[4];       // x width, y height in pixels
    uint32_t Counts[4];             // x point lights, y depth slices
    float ScreenParams[4];          // xy a pixel, z screen lighting, w sun from the screen
    float SkyZenith[4];
    float SkyHorizon[4];
    float SkyGround[4];
};

// The sun's disc in the sky: its radius at SunSize 1, which is larger than the
// real sun's so it reads at the size a game shows it, and how much brighter
// than the light it casts its disc looks.
constexpr float SunDiscDegrees = 0.75f;
constexpr float SunDiscBrightness = 40.0f;

// A texture of one texel, of one colour.
SDL_GPUTexture* CreateSolidTexture(SDL_GPUDevice* device, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    SDL_GPUTexture* texture = CreateTarget(device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 1, 1, SDL_GPU_SAMPLECOUNT_1,
                                           SDL_GPU_TEXTUREUSAGE_SAMPLER);
    if (texture == nullptr)
    {
        return nullptr;
    }

    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    info.size = 4;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device, &info);
    auto* mapped = transfer != nullptr ? static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device, transfer, false))
                                       : nullptr;
    if (mapped == nullptr)
    {
        if (transfer != nullptr)
        {
            SDL_ReleaseGPUTransferBuffer(device, transfer);
        }
        SDL_ReleaseGPUTexture(device, texture);
        return nullptr;
    }
    mapped[0] = r;
    mapped[1] = g;
    mapped[2] = b;
    mapped[3] = a;
    SDL_UnmapGPUTransferBuffer(device, transfer);

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);
    SDL_GPUTextureTransferInfo source{};
    source.transfer_buffer = transfer;
    SDL_GPUTextureRegion region{};
    region.texture = texture;
    region.w = 1;
    region.h = 1;
    region.d = 1;
    SDL_UploadToGPUTexture(copyPass, &source, &region, false);
    SDL_EndGPUCopyPass(copyPass);
    SDL_SubmitGPUCommandBuffer(commandBuffer);
    SDL_ReleaseGPUTransferBuffer(device, transfer);
    return texture;
}

SDL_GPUSampleCount SamplesFor(AntiAliasing mode)
{
    switch (mode)
    {
        case AntiAliasing::Off:      return SDL_GPU_SAMPLECOUNT_1;
        case AntiAliasing::Fast:     return SDL_GPU_SAMPLECOUNT_1;
        case AntiAliasing::Balanced: return SDL_GPU_SAMPLECOUNT_4;
        case AntiAliasing::High:     return SDL_GPU_SAMPLECOUNT_8;
        case AntiAliasing::Temporal: return SDL_GPU_SAMPLECOUNT_2;
    }
    return SDL_GPU_SAMPLECOUNT_4;
}

bool UsesFxaa(AntiAliasing mode)
{
    return mode == AntiAliasing::Fast || mode == AntiAliasing::High;
}

bool UsesTaa(AntiAliasing mode)
{
    return mode == AntiAliasing::Temporal;
}

// The low-discrepancy sequence TAA jitters by: successive frames land on
// sample positions that fill the pixel evenly rather than clumping.
float Halton(uint32_t index, uint32_t base)
{
    float result = 0.0f;
    float fraction = 1.0f / static_cast<float>(base);
    while (index > 0)
    {
        result += fraction * static_cast<float>(index % base);
        index /= base;
        fraction /= static_cast<float>(base);
    }
    return result;
}

Vec3 TransformPoint(const Mat4& matrix, const Vec3& point)
{
    const float* m = matrix.M;
    return Vec3{ m[0] * point.X + m[4] * point.Y + m[8] * point.Z + m[12],
                 m[1] * point.X + m[5] * point.Y + m[9] * point.Z + m[13],
                 m[2] * point.X + m[6] * point.Y + m[10] * point.Z + m[14] };
}

Vec3 Rotate(const Quat& q, const Vec3& v)
{
    const Quat p{ v.X, v.Y, v.Z, 0.0f };
    const Quat r = QuatMultiply(QuatMultiply(q, p), QuatConjugate(q));
    return Vec3{ r.X, r.Y, r.Z };
}

float Dot(const Vec3& a, const Vec3& b)
{
    return a.X * b.X + a.Y * b.Y + a.Z * b.Z;
}

uint64_t TextureKey(const TextureId& texture)
{
    return (static_cast<uint64_t>(texture.Index) << 32) | texture.Generation;
}

void CopyMatrix(float out[16], const Mat4& matrix)
{
    std::memcpy(out, matrix.M, sizeof(float) * 16);
}

bool EnsureBuffer(SDL_GPUDevice* device, SDL_GPUBuffer*& buffer, size_t& capacity, size_t needed,
                  SDL_GPUBufferUsageFlags usage)
{
    needed = std::max<size_t>(needed, 256);
    if (buffer != nullptr && capacity >= needed)
    {
        return true;
    }

    // Grow geometrically so a scene that keeps adding objects stops
    // reallocating quickly rather than once per frame.
    size_t size = std::max<size_t>(capacity, 4096);
    while (size < needed)
    {
        size *= 2;
    }

    if (buffer != nullptr)
    {
        SDL_ReleaseGPUBuffer(device, buffer);
    }

    SDL_GPUBufferCreateInfo info{};
    info.usage = usage;
    info.size = static_cast<uint32_t>(size);
    buffer = SDL_CreateGPUBuffer(device, &info);
    capacity = buffer != nullptr ? size : 0;

    if (buffer == nullptr)
    {
        LogMessage(LogLevel::Error, "render", "Could not allocate a frame buffer: %s", SDL_GetError());
        return false;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

bool Renderer3D::CreateShaders()
{
    const SDL_GPUShaderStage vertex = SDL_GPU_SHADERSTAGE_VERTEX;
    const SDL_GPUShaderStage fragment = SDL_GPU_SHADERSTAGE_FRAGMENT;

    struct Stage
    {
        SDL_GPUShader** Shader;
        SDL_GPUShaderStage Kind;
        const char* EntryPoint;
        BuiltInShader Code;
        uint32_t Samplers;
        uint32_t StorageBuffers;
        uint32_t UniformBuffers;
    };

    // Resource counts must match what each stage declares: the surface
    // fragment stage has nine samplers (base colour, shadow map, the four
    // other maps, and the three screen-space lighting textures) and five
    // storage buffers (instances, per-actor parameters, lights, and the two
    // that say which lights reach which cell).
    const Stage stages[] = {
        { &m_SurfaceVertex, vertex, "VertexMain", BuiltInShader::WorldVertex, 0, 4, 1 },
        { &m_SurfaceFragment, fragment, "FragmentMain", BuiltInShader::WorldFragment, 9, 5, 1 },
        { &m_ShadowVertex, vertex, "ShadowVertexMain", BuiltInShader::ShadowVertex, 0, 4, 1 },
        { &m_ShadowFragment, fragment, "ShadowFragmentMain", BuiltInShader::ShadowFragment, 0, 0, 0 },
        { &m_MotionVertex, vertex, "MotionVertexMain", BuiltInShader::MotionVertex, 0, 4, 1 },
        { &m_MotionFragment, fragment, "MotionFragmentMain", BuiltInShader::MotionFragment, 0, 0, 0 },
        { &m_LineVertex, vertex, "LineVertexMain", BuiltInShader::LineVertex, 0, 0, 1 },
        { &m_LineFragment, fragment, "LineFragmentMain", BuiltInShader::LineFragment, 0, 0, 0 },
        { &m_FullscreenVertex, vertex, "FullscreenVertexMain", BuiltInShader::FullscreenVertex, 0, 0, 0 },
        { &m_BackdropFragment, fragment, "BackdropFragmentMain", BuiltInShader::BackdropFragment, 1, 0, 1 },
        { &m_ToneMapFragment, fragment, "ToneMapFragmentMain", BuiltInShader::ToneMapFragment, 3, 0, 1 },
        { &m_FxaaFragment, fragment, "FxaaFragmentMain", BuiltInShader::FxaaFragment, 1, 0, 1 },
        { &m_TaaFragment, fragment, "TaaFragmentMain", BuiltInShader::TaaFragment, 3, 0, 1 },
        { &m_SkyFragment, fragment, "SkyFragmentMain", BuiltInShader::SkyFragment, 0, 0, 1 },
        { &m_OcclusionFragment, fragment, "AmbientOcclusionFragmentMain", BuiltInShader::OcclusionFragment, 2, 0, 1 },
        { &m_OcclusionBlurFragment, fragment, "OcclusionBlurFragmentMain", BuiltInShader::OcclusionBlurFragment, 2,
          0, 1 },
        { &m_BloomPrefilterFragment, fragment, "BloomPrefilterFragmentMain", BuiltInShader::BloomPrefilterFragment,
          1, 0, 1 },
        { &m_BloomDownFragment, fragment, "BloomDownFragmentMain", BuiltInShader::BloomDownFragment, 1, 0, 1 },
        { &m_BloomUpFragment, fragment, "BloomUpFragmentMain", BuiltInShader::BloomUpFragment, 1, 0, 1 },
        { &m_LuminanceFragment, fragment, "LuminanceFragmentMain", BuiltInShader::LuminanceFragment, 1, 0, 1 },
        { &m_AdaptFragment, fragment, "AdaptFragmentMain", BuiltInShader::AdaptFragment, 2, 0, 1 },
        { &m_UpscaleFragment, fragment, "UpscaleFragmentMain", BuiltInShader::UpscaleFragment, 1, 0, 1 },
    };

    bool created = true;
    for (const Stage& stage : stages)
    {
        *stage.Shader = CreateShader(m_Device, stage.Kind, stage.EntryPoint, GetBuiltInShader(stage.Code),
                                     stage.Samplers, stage.StorageBuffers, stage.UniformBuffers);
        created = created && *stage.Shader != nullptr;
    }
    return created;
}

bool Renderer3D::Initialize(SDL_GPUDevice* device)
{
    m_Device = device;
    RegisterRenderer(this);

    if (!CreateShaders())
    {
        return false;
    }

    // D32 float is the common case; the packed format is the fallback for
    // devices that do not offer it.
    if (!SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTURETYPE_2D,
                                      SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET))
    {
        m_DepthFormat = SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
    }

    if (!SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTURETYPE_2D,
                                      SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER))
    {
        m_SampledDepthFormat = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    }

    // Half-float colour gives lighting room above 1 for the tone map to roll
    // off. Where multisampled half-float is unavailable the scene falls back
    // to 8-bit, losing the headroom but not the picture.
    if (!SDL_GPUTextureSupportsFormat(device, m_SceneFormat, SDL_GPU_TEXTURETYPE_2D,
                                      SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER))
    {
        m_SceneFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    }

    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;

    auto Build = [&](void (*builder)(std::vector<MeshVertex>&, std::vector<uint32_t>&), MeshBuffers& mesh) {
        vertices.clear();
        indices.clear();
        builder(vertices, indices);
        if (!UploadMesh(m_Device, vertices.data(), vertices.size(), indices, mesh))
        {
            return false;
        }
        KeepGeometry(mesh, vertices, indices);
        return true;
    };

    if (!Build(BuildBoxMesh, m_BoxMesh) || !Build(BuildQuadMesh, m_QuadMesh))
    {
        return false;
    }

    // A sphere at three densities, so a distant one costs a fraction of a
    // near one and looks the same at that size.
    for (int detail = 0; detail < 3; ++detail)
    {
        vertices.clear();
        indices.clear();
        BuildSphereMeshAt(vertices, indices, detail);
        if (!UploadMesh(m_Device, vertices.data(), vertices.size(), indices, m_SphereMeshes[detail]))
        {
            return false;
        }
        KeepGeometry(m_SphereMeshes[detail], vertices, indices);
    }
    m_SphereMesh = m_SphereMeshes[0];

    // Hardware percentage-closer filtering: each lookup compares against four
    // texels and blends the results.
    SDL_GPUSamplerCreateInfo shadowInfo{};
    shadowInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    shadowInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    shadowInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    shadowInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    shadowInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    shadowInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    shadowInfo.enable_compare = true;
    shadowInfo.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    m_ShadowSampler = SDL_CreateGPUSampler(m_Device, &shadowInfo);

    SDL_GPUSamplerCreateInfo pointInfo{};
    pointInfo.min_filter = SDL_GPU_FILTER_NEAREST;
    pointInfo.mag_filter = SDL_GPU_FILTER_NEAREST;
    pointInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    pointInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    pointInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    pointInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    m_PointSampler = SDL_CreateGPUSampler(m_Device, &pointInfo);

    SDL_GPUSamplerCreateInfo linearInfo = pointInfo;
    linearInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    linearInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    m_LinearClampSampler = SDL_CreateGPUSampler(m_Device, &linearInfo);

    SDL_GPUSamplerCreateInfo mipInfo = linearInfo;
    mipInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    mipInfo.max_lod = 16.0f;
    m_MipSampler = SDL_CreateGPUSampler(m_Device, &mipInfo);

    m_NoShadow = CreateTarget(m_Device, m_SampledDepthFormat, 1, 1, SDL_GPU_SAMPLECOUNT_1,
                              SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);

    const float noDeltas[8] = {};
    m_NoMorphDeltas = UploadStorage(m_Device, noDeltas, sizeof(noDeltas));

    m_White = CreateSolidTexture(m_Device, 255, 255, 255, 255);
    m_Clear = CreateSolidTexture(m_Device, 0, 0, 0, 0);

    if (!CreateEffectPipelines())
    {
        return false;
    }

    // Tracing needs compute storage in the formats it writes. Without it the
    // rasterized effects stand in, and nothing else changes.
    m_Rays = new RayScene();
    m_RaysSupported = RayScene::IsSupported(m_Device) && CreateRayPipelines();

    return m_ShadowSampler != nullptr && m_PointSampler != nullptr && m_LinearClampSampler != nullptr &&
           m_MipSampler != nullptr && m_NoShadow != nullptr && m_NoMorphDeltas != nullptr && m_White != nullptr &&
           m_Clear != nullptr;
}

void Renderer3D::KeepGeometry(MeshBuffers& mesh, std::vector<MeshVertex> vertices, std::vector<uint32_t> indices)
{
    m_PrimitiveGeometry.emplace_back(std::move(vertices), std::move(indices));
    const auto& [keptVertices, keptIndices] = m_PrimitiveGeometry.back();
    mesh.CpuVertices = reinterpret_cast<const ModelVertex*>(keptVertices.data());
    mesh.CpuVertexCount = static_cast<uint32_t>(keptVertices.size());
    mesh.CpuIndices = keptIndices.data();
    mesh.CpuIndexCount = static_cast<uint32_t>(keptIndices.size());
}

SDL_GPUGraphicsPipeline* Renderer3D::CreateSurfacePipeline(SDL_GPUShader* fragmentShader, SurfaceVariant variant,
                                                           bool alphaToCoverage) const
{
    SDL_GPUVertexBufferDescription bufferDescription{};
    bufferDescription.slot = 0;
    bufferDescription.pitch = sizeof(MeshVertex);
    bufferDescription.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexAttribute attributes[5]{};
    attributes[0] = { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0 };
    attributes[1] = { 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, sizeof(float) * 3 };
    attributes[2] = { 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, sizeof(float) * 6 };
    // Indices as plain bytes, weights as bytes the hardware divides by 255 on
    // the way in, so the shader receives them already summing to one.
    attributes[3] = { 3, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4, sizeof(float) * 8 };
    attributes[4] = { 4, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, sizeof(float) * 8 + 4 };

    const bool blended = (variant != SurfaceVariant::Opaque);

    SDL_GPUColorTargetDescription colorTarget{};
    colorTarget.format = m_SceneFormat;
    if (blended)
    {
        colorTarget.blend_state.enable_blend = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    }

    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = m_SurfaceVertex;
    info.fragment_shader = fragmentShader;
    info.vertex_input_state.vertex_buffer_descriptions = &bufferDescription;
    info.vertex_input_state.num_vertex_buffers = 1;
    info.vertex_input_state.vertex_attributes = attributes;
    info.vertex_input_state.num_vertex_attributes = 5;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    // A sprite is a single face; everything else is closed and culls its back.
    info.rasterizer_state.cull_mode =
        (variant == SurfaceVariant::Overlay) ? SDL_GPU_CULLMODE_NONE : SDL_GPU_CULLMODE_BACK;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

    info.multisample_state.sample_count = m_SampleCount;

    // Only meaningful with more than one sample, and only for opaque
    // materials: the default shader writes the actor's alpha, which is 1 for
    // anything drawn opaque.
    info.multisample_state.enable_alpha_to_coverage =
        alphaToCoverage && variant == SurfaceVariant::Opaque && m_SampleCount != SDL_GPU_SAMPLECOUNT_1;

    info.depth_stencil_state.enable_depth_test = (variant != SurfaceVariant::Overlay);
    info.depth_stencil_state.enable_depth_write = (variant == SurfaceVariant::Opaque);
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;

    info.target_info.color_target_descriptions = &colorTarget;
    info.target_info.num_color_targets = 1;
    info.target_info.has_depth_stencil_target = true;
    info.target_info.depth_stencil_format = m_DepthFormat;

    return SDL_CreateGPUGraphicsPipeline(m_Device, &info);
}

SDL_GPUGraphicsPipeline* Renderer3D::CreateFullscreenPipeline(SDL_GPUShader* fragmentShader,
                                                              SDL_GPUTextureFormat format,
                                                              SDL_GPUSampleCount samples, bool withDepth,
                                                              bool additive) const
{
    SDL_GPUColorTargetDescription colorTarget{};
    colorTarget.format = format;
    if (additive)
    {
        colorTarget.blend_state.enable_blend = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        colorTarget.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    }

    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = m_FullscreenVertex;
    info.fragment_shader = fragmentShader;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.multisample_state.sample_count = samples;
    info.target_info.color_target_descriptions = &colorTarget;
    info.target_info.num_color_targets = 1;

    if (withDepth)
    {
        info.target_info.has_depth_stencil_target = true;
        info.target_info.depth_stencil_format = m_DepthFormat;
    }

    return SDL_CreateGPUGraphicsPipeline(m_Device, &info);
}

bool Renderer3D::BuildPipelines()
{
    ReleasePipelines();

    for (size_t variant = 0; variant < m_DefaultSurface.size(); ++variant)
    {
        m_DefaultSurface[variant] = CreateSurfacePipeline(m_SurfaceFragment, static_cast<SurfaceVariant>(variant), false);
    }

    const SDL_GPUVertexAttribute meshAttributes[5] = {
        { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0 },
        { 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, sizeof(float) * 3 },
        { 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, sizeof(float) * 6 },
        // Indices as plain bytes, weights as bytes the hardware divides by 255
        // on the way in, so the shader receives them already summing to one.
        { 3, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4, sizeof(float) * 8 },
        { 4, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, sizeof(float) * 8 + 4 },
    };
    SDL_GPUVertexBufferDescription meshBuffer{};
    meshBuffer.pitch = sizeof(MeshVertex);
    meshBuffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    // The shadow map: depth only. Rendering both faces keeps thin objects
    // casting shadows; the slope-scaled bias, together with the normal offset
    // in the shader, keeps lit surfaces from shadowing themselves.
    {
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = m_ShadowVertex;
        info.fragment_shader = m_ShadowFragment;
        info.vertex_input_state.vertex_buffer_descriptions = &meshBuffer;
        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_attributes = meshAttributes;
        info.vertex_input_state.num_vertex_attributes = 5;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.rasterizer_state.enable_depth_bias = true;
        info.rasterizer_state.depth_bias_constant_factor = 1.0f;
        info.rasterizer_state.depth_bias_slope_factor = 1.5f;
        info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = true;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
        info.target_info.num_color_targets = 0;
        info.target_info.has_depth_stencil_target = true;
        info.target_info.depth_stencil_format = m_SampledDepthFormat;
        m_ShadowPipeline = SDL_CreateGPUGraphicsPipeline(m_Device, &info);
    }

    // Depth, motion, and normals, single-sampled, so all three can be sampled
    // afterwards.
    {
        SDL_GPUColorTargetDescription motionTargets[2]{};
        motionTargets[0].format = SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;
        motionTargets[1].format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;

        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = m_MotionVertex;
        info.fragment_shader = m_MotionFragment;
        info.vertex_input_state.vertex_buffer_descriptions = &meshBuffer;
        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_attributes = meshAttributes;
        info.vertex_input_state.num_vertex_attributes = 5;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
        info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = true;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
        info.target_info.color_target_descriptions = motionTargets;
        info.target_info.num_color_targets = 2;
        info.target_info.has_depth_stencil_target = true;
        info.target_info.depth_stencil_format = m_SampledDepthFormat;
        m_MotionPipeline = SDL_CreateGPUGraphicsPipeline(m_Device, &info);
    }

    // Debug lines: tested against the scene's depth but never written to it.
    {
        SDL_GPUVertexBufferDescription lineBuffer{};
        lineBuffer.pitch = sizeof(float) * 7;
        lineBuffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

        SDL_GPUVertexAttribute lineAttributes[2] = {
            { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0 },
            { 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, sizeof(float) * 3 },
        };

        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = m_SceneFormat;
        colorTarget.blend_state.enable_blend = true;
        colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = m_LineVertex;
        info.fragment_shader = m_LineFragment;
        info.vertex_input_state.vertex_buffer_descriptions = &lineBuffer;
        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_attributes = lineAttributes;
        info.vertex_input_state.num_vertex_attributes = 2;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_LINELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.multisample_state.sample_count = m_SampleCount;
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = false;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
        info.target_info.color_target_descriptions = &colorTarget;
        info.target_info.num_color_targets = 1;
        info.target_info.has_depth_stencil_target = true;
        info.target_info.depth_stencil_format = m_DepthFormat;
        m_LinePipeline = SDL_CreateGPUGraphicsPipeline(m_Device, &info);
    }

    m_BackdropPipeline = CreateFullscreenPipeline(m_BackdropFragment, m_SceneFormat, m_SampleCount, true);
    m_SkyPipeline = CreateFullscreenPipeline(m_SkyFragment, m_SceneFormat, m_SampleCount, true);
    m_ToneMapPipeline = CreateFullscreenPipeline(m_ToneMapFragment, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                                 SDL_GPU_SAMPLECOUNT_1);
    m_FxaaPipeline = CreateFullscreenPipeline(m_FxaaFragment, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                              SDL_GPU_SAMPLECOUNT_1);
    m_TaaPipeline = CreateFullscreenPipeline(m_TaaFragment, m_SceneFormat, SDL_GPU_SAMPLECOUNT_1);

    for (SDL_GPUGraphicsPipeline* pipeline : m_DefaultSurface)
    {
        if (pipeline == nullptr)
        {
            LogMessage(LogLevel::Error, "render", "Could not create the world pipeline: %s", SDL_GetError());
            return false;
        }
    }

    // Named one by one, because a single message for the group would not say
    // which pipeline failed, and the driver's own message rarely helps.
    const std::pair<SDL_GPUGraphicsPipeline*, const char*> passes[] = {
        { m_ShadowPipeline, "shadow map" },   { m_MotionPipeline, "depth and motion" },
        { m_LinePipeline, "debug lines" },    { m_BackdropPipeline, "backdrop" },
        { m_ToneMapPipeline, "tone map" },    { m_FxaaPipeline, "FXAA" },
        { m_TaaPipeline, "temporal anti-aliasing" }, { m_SkyPipeline, "sky" },
    };

    for (const auto& pass : passes)
    {
        if (pass.first == nullptr)
        {
            LogMessage(LogLevel::Error, "render", "Could not create the %s pipeline: %s", pass.second,
                       SDL_GetError());
            return false;
        }
    }

    m_PipelinesBuilt = true;
    return true;
}

void Renderer3D::ReleasePipelines()
{
    auto Release = [&](SDL_GPUGraphicsPipeline*& pipeline) {
        if (pipeline != nullptr)
        {
            SDL_ReleaseGPUGraphicsPipeline(m_Device, pipeline);
            pipeline = nullptr;
        }
    };

    for (SDL_GPUGraphicsPipeline*& pipeline : m_DefaultSurface)
    {
        Release(pipeline);
    }
    Release(m_ShadowPipeline);
    Release(m_MotionPipeline);
    Release(m_LinePipeline);
    Release(m_BackdropPipeline);
    Release(m_ToneMapPipeline);
    Release(m_FxaaPipeline);
    Release(m_TaaPipeline);
    Release(m_SkyPipeline);

    // Every material pipeline is baked for one sample count, so they go too.
    // They rebuild lazily on next use.
    ReleaseMaterialPipelines();
    m_PipelinesBuilt = false;
}

void Renderer3D::ReleaseMaterialPipelines()
{
    for (auto& variants : m_MaterialSurface)
    {
        for (CachedPipeline& cached : variants)
        {
            if (cached.Pipeline != nullptr && m_Device != nullptr)
            {
                SDL_ReleaseGPUGraphicsPipeline(m_Device, cached.Pipeline);
            }
            cached = CachedPipeline{};
        }
    }
    for (auto& variants : m_MaterialPost)
    {
        for (CachedPipeline& cached : variants)
        {
            if (cached.Pipeline != nullptr && m_Device != nullptr)
            {
                SDL_ReleaseGPUGraphicsPipeline(m_Device, cached.Pipeline);
            }
            cached = CachedPipeline{};
        }
    }
    m_MaterialSurface.clear();
    m_MaterialPost.clear();
}

SDL_GPUGraphicsPipeline* Renderer3D::SurfacePipelineFor(int materialIndex, SurfaceVariant variant)
{
    if (materialIndex < 0)
    {
        return nullptr;
    }

    WorldMaterialRecord* record = GetWorldMaterials().At(static_cast<size_t>(materialIndex));
    if (record == nullptr || !record->Alive)
    {
        return nullptr;
    }

    const size_t index = static_cast<size_t>(materialIndex);
    if (m_MaterialSurface.size() <= index)
    {
        m_MaterialSurface.resize(index + 1);
    }

    CachedPipeline& cached = m_MaterialSurface[index][static_cast<size_t>(variant)];

    // Up to date: same material, same compiled version.
    if (cached.Generation == record->Generation && cached.Version == record->Version)
    {
        return cached.Failed ? nullptr : cached.Pipeline;
    }

    if (cached.Pipeline != nullptr)
    {
        SDL_ReleaseGPUGraphicsPipeline(m_Device, cached.Pipeline);
        cached.Pipeline = nullptr;
    }

    cached.Generation = record->Generation;
    cached.Version = record->Version;
    cached.Failed = false;

    // A material reads the same resources as the built-in shader plus its own
    // uniform block, so it declares one more uniform buffer.
    SDL_GPUShader* fragmentShader = nullptr;
    const ShaderBytecode code = GetWorldMaterials().CodeFor(*record, DeviceShaderFormat(m_Device));
    if (!code.IsEmpty())
    {
        fragmentShader =
            CreateShader(m_Device, SDL_GPU_SHADERSTAGE_FRAGMENT, record->EntryPoint.c_str(), code, 9, 5, 2);
    }

    if (fragmentShader != nullptr)
    {
        cached.Pipeline = CreateSurfacePipeline(fragmentShader, variant, true);
        SDL_ReleaseGPUShader(m_Device, fragmentShader);
    }

    if (cached.Pipeline == nullptr)
    {
        // Remembered, so a broken material costs one diagnostic rather than a
        // rebuild attempt and a log line every frame. Missing code has been
        // reported already, by the store.
        cached.Failed = true;
        if (!code.IsEmpty())
        {
            LogMessage(LogLevel::Error, "material",
                       "A material's shader could not be turned into a pipeline; its actors draw with the default "
                       "shader until it is fixed. %s",
                       SDL_GetError());
        }
        return nullptr;
    }

    return cached.Pipeline;
}

SDL_GPUGraphicsPipeline* Renderer3D::PostPipelineFor(int materialIndex, bool linearLight)
{
    if (materialIndex < 0)
    {
        return nullptr;
    }

    WorldMaterialRecord* record = GetWorldMaterials().At(static_cast<size_t>(materialIndex));
    if (record == nullptr || !record->Alive)
    {
        return nullptr;
    }

    const size_t index = static_cast<size_t>(materialIndex);
    if (m_MaterialPost.size() <= index)
    {
        m_MaterialPost.resize(index + 1);
    }

    CachedPipeline& cached = m_MaterialPost[index][linearLight ? 0 : 1];
    if (cached.Generation == record->Generation && cached.Version == record->Version)
    {
        return cached.Failed ? nullptr : cached.Pipeline;
    }

    if (cached.Pipeline != nullptr)
    {
        SDL_ReleaseGPUGraphicsPipeline(m_Device, cached.Pipeline);
        cached.Pipeline = nullptr;
    }

    cached.Generation = record->Generation;
    cached.Version = record->Version;
    cached.Failed = false;

    // Scene and SceneDepth, the post uniforms, and the material's block.
    SDL_GPUShader* fragmentShader = nullptr;
    const ShaderBytecode code = GetWorldMaterials().CodeFor(*record, DeviceShaderFormat(m_Device));
    if (!code.IsEmpty())
    {
        fragmentShader =
            CreateShader(m_Device, SDL_GPU_SHADERSTAGE_FRAGMENT, record->EntryPoint.c_str(), code, 2, 0, 2);
    }
    if (fragmentShader != nullptr)
    {
        cached.Pipeline = CreateFullscreenPipeline(
            fragmentShader, linearLight ? m_SceneFormat : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_SAMPLECOUNT_1);
        SDL_ReleaseGPUShader(m_Device, fragmentShader);
    }

    if (cached.Pipeline == nullptr)
    {
        cached.Failed = true;
        if (!code.IsEmpty())
        {
            LogMessage(LogLevel::Error, "material",
                       "A post-process material could not be turned into a pipeline and is skipped until it is "
                       "fixed. Check that it includes world_post.hlsli. %s",
                       SDL_GetError());
        }
        return nullptr;
    }

    return cached.Pipeline;
}

void Renderer3D::ReleaseTargets()
{
    ReleaseSceneTargets();
    if (m_Output != nullptr)
    {
        SDL_ReleaseGPUTexture(m_Device, m_Output);
        m_Output = nullptr;
    }
}

void Renderer3D::ReleaseSceneTargets()
{
    auto Release = [&](SDL_GPUTexture*& texture) {
        if (texture != nullptr)
        {
            SDL_ReleaseGPUTexture(m_Device, texture);
            texture = nullptr;
        }
    };

    Release(m_SceneMsaa);
    Release(m_Scene);
    Release(m_Depth);
    for (SDL_GPUTexture*& texture : m_HdrWork)
    {
        Release(texture);
    }
    for (SDL_GPUTexture*& texture : m_LdrWork)
    {
        Release(texture);
    }
    for (SDL_GPUTexture*& texture : m_History)
    {
        Release(texture);
    }
    Release(m_Motion);
    Release(m_PrepassDepth);
    Release(m_Normals);

    ReleaseEffectTargets();
    ReleaseRayTargets();

    m_HistoryValid = false;
}

bool Renderer3D::SetSize(int outputWidth, int outputHeight, const RenderSettings& settings)
{
    if (outputWidth <= 0 || outputHeight <= 0 || m_Device == nullptr)
    {
        return false;
    }

    SDL_GPUSampleCount requested = SamplesFor(settings.Mode);

    // Fall back to the highest count the device actually supports rather than
    // failing outright.
    while (requested != SDL_GPU_SAMPLECOUNT_1 &&
           !SDL_GPUTextureSupportsSampleCount(m_Device, m_SceneFormat, requested))
    {
        requested = static_cast<SDL_GPUSampleCount>(static_cast<int>(requested) - 1);
    }

    // The scene is drawn at the render scale, never wider than the largest
    // texture a device is sure to take.
    const float scale = std::clamp(std::isfinite(settings.RenderScale) ? settings.RenderScale : 1.0f, 0.25f, 2.0f);
    const int width = std::clamp(static_cast<int>(std::lround(static_cast<float>(outputWidth) * scale)), 1, 16384);
    const int height = std::clamp(static_cast<int>(std::lround(static_cast<float>(outputHeight) * scale)), 1, 16384);
    m_RenderScale = scale;

    const bool outputChanged = (outputWidth != m_OutputWidth || outputHeight != m_OutputHeight || m_Output == nullptr);
    const bool sizeChanged = (width != m_Width || height != m_Height || m_Scene == nullptr);

    if (!sizeChanged && !outputChanged && requested == m_SampleCount && m_PipelinesBuilt)
    {
        return true;
    }

    const SDL_GPUTextureUsageFlags sampledTarget = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;

    // The output is replaced only when the host's size changes: a host wrapped
    // that pointer once, and a change of render scale must not break it.
    if (outputChanged)
    {
        if (m_Output != nullptr)
        {
            SDL_ReleaseGPUTexture(m_Device, m_Output);
        }
        m_OutputWidth = outputWidth;
        m_OutputHeight = outputHeight;
        m_Output = CreateTarget(m_Device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, outputWidth, outputHeight,
                                SDL_GPU_SAMPLECOUNT_1, sampledTarget);
        if (m_Output == nullptr)
        {
            return false;
        }
    }

    if (sizeChanged)
    {
        ReleaseSceneTargets();

        m_Width = width;
        m_Height = height;

        m_Scene = CreateTarget(m_Device, m_SceneFormat, width, height, SDL_GPU_SAMPLECOUNT_1, sampledTarget);
        if (m_Scene == nullptr)
        {
            return false;
        }
    }
    else if (requested != m_SampleCount || !m_PipelinesBuilt)
    {
        // Only the anti-aliasing changed. Only the multisampled targets
        // depend on the sample count.
        if (m_SceneMsaa != nullptr)
        {
            SDL_ReleaseGPUTexture(m_Device, m_SceneMsaa);
            m_SceneMsaa = nullptr;
        }
        if (m_Depth != nullptr)
        {
            SDL_ReleaseGPUTexture(m_Device, m_Depth);
            m_Depth = nullptr;
        }
        m_HistoryValid = false;
    }

    if (!sizeChanged && requested == m_SampleCount && m_PipelinesBuilt)
    {
        return true;
    }

    if (requested != SDL_GPU_SAMPLECOUNT_1)
    {
        m_SceneMsaa = CreateTarget(m_Device, m_SceneFormat, width, height, requested,
                                   SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
        if (m_SceneMsaa == nullptr)
        {
            LogMessage(LogLevel::Warning, "render",
                       "Multisampling is unavailable at this size; falling back to no multisampling.");
            requested = SDL_GPU_SAMPLECOUNT_1;
        }
    }

    m_Depth = CreateTarget(m_Device, m_DepthFormat, width, height, requested,
                           SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
    if (m_Depth == nullptr)
    {
        return false;
    }

    if (requested != m_SampleCount || !m_PipelinesBuilt)
    {
        m_SampleCount = requested;
        if (!BuildPipelines())
        {
            return false;
        }
    }

    return true;
}

void Renderer3D::Shutdown()
{
    UnregisterRenderer(this);

    if (m_Device == nullptr)
    {
        return;
    }

    ReleaseTargets();
    ReleasePipelines();
    ReleaseEffectPipelines();
    ReleaseRayPipelines();

    if (m_Rays != nullptr)
    {
        m_Rays->Shutdown(m_Device);
        delete m_Rays;
        m_Rays = nullptr;
    }

    auto ReleaseTexture = [&](SDL_GPUTexture*& texture) {
        if (texture != nullptr)
        {
            SDL_ReleaseGPUTexture(m_Device, texture);
            texture = nullptr;
        }
    };
    ReleaseTexture(m_ShadowMap);
    ReleaseTexture(m_NoShadow);
    ReleaseTexture(m_White);
    ReleaseTexture(m_Clear);
    ReleaseTexture(m_Luminance);
    for (SDL_GPUTexture*& texture : m_Exposure)
    {
        ReleaseTexture(texture);
    }
    m_ExposureValid = false;

    ReleaseMesh(m_Device, m_BoxMesh);
    for (MeshBuffers& mesh : m_SphereMeshes)
    {
        ReleaseMesh(m_Device, mesh);
    }
    m_SphereMesh = MeshBuffers{};
    for (auto& [key, mesh] : m_CapsuleMeshes)
    {
        ReleaseMesh(m_Device, mesh);
    }
    m_CapsuleMeshes.clear();
    m_PrimitiveGeometry.clear();
    ReleaseMesh(m_Device, m_QuadMesh);
    for (std::unique_ptr<MeshBuffers>& mesh : m_ModelMeshes)
    {
        ReleaseMesh(m_Device, *mesh);
    }
    m_ModelMeshes.clear();
    m_ModelRevisions.clear();

    auto ReleaseBuffer = [&](SDL_GPUBuffer*& buffer) {
        if (buffer != nullptr)
        {
            SDL_ReleaseGPUBuffer(m_Device, buffer);
            buffer = nullptr;
        }
    };
    ReleaseBuffer(m_InstanceBuffer);
    ReleaseBuffer(m_JointBuffer);
    m_JointCapacity = 0;
    ReleaseBuffer(m_MorphBuffer);
    m_MorphCapacity = 0;
    ReleaseBuffer(m_NoMorphDeltas);
    ReleaseBuffer(m_ParamBuffer);
    ReleaseBuffer(m_LightBuffer);
    ReleaseBuffer(m_ClusterRangeBuffer);
    ReleaseBuffer(m_ClusterIndexBuffer);
    ReleaseBuffer(m_LineBuffer);
    if (m_Transfer != nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(m_Device, m_Transfer);
        m_Transfer = nullptr;
    }

    for (SDL_GPUSampler* sampler : { m_ShadowSampler, m_PointSampler, m_LinearClampSampler, m_MipSampler })
    {
        if (sampler != nullptr)
        {
            SDL_ReleaseGPUSampler(m_Device, sampler);
        }
    }
    m_ShadowSampler = nullptr;
    m_PointSampler = nullptr;
    m_LinearClampSampler = nullptr;
    m_MipSampler = nullptr;

    for (SDL_GPUShader** shader :
         { &m_SurfaceVertex, &m_SurfaceFragment, &m_ShadowVertex, &m_ShadowFragment, &m_MotionVertex,
           &m_MotionFragment, &m_LineVertex, &m_LineFragment, &m_FullscreenVertex, &m_BackdropFragment,
           &m_ToneMapFragment, &m_FxaaFragment, &m_TaaFragment, &m_SkyFragment, &m_OcclusionFragment,
           &m_OcclusionBlurFragment, &m_BloomPrefilterFragment, &m_BloomDownFragment, &m_BloomUpFragment,
           &m_LuminanceFragment, &m_AdaptFragment, &m_UpscaleFragment })
    {
        if (*shader != nullptr)
        {
            SDL_ReleaseGPUShader(m_Device, *shader);
            *shader = nullptr;
        }
    }

    m_Device = nullptr;
}

// ---------------------------------------------------------------------------
// Building a frame
// ---------------------------------------------------------------------------

void Renderer3D::ResetFrame()
{
    m_Instances.clear();
    m_OpaqueOrder.clear();
    m_TransparentOrder.clear();
    m_OverlayBehind.clear();
    m_OverlayFront.clear();
    m_ShadowOrder.clear();
    m_Lights.clear();
    m_LineVertices.clear();
    m_ParamData.clear();
    m_JointMatrices.clear();
    m_MorphChannels.clear();

    m_DrawCalls = 0;
    m_InstanceCount = 0;
    m_CulledCount = 0;
    m_LargestBatch = 0;
    m_UploadsThisFrame = 0;
}

MeshBuffers* Renderer3D::MeshForShape(ShapeKind shape)
{
    switch (shape)
    {
        case ShapeKind::Sphere:  return &m_SphereMesh;
        case ShapeKind::Capsule: return MeshForCapsule(0.5f);
        case ShapeKind::Box:
        case ShapeKind::Model:   break;
    }
    return &m_BoxMesh;
}

MeshBuffers* Renderer3D::MeshForCapsule(float stretch, int detail)
{
    // The proportion and the density together name the mesh: caps survive
    // only uniform scale, so each proportion needs its own.
    const int key = static_cast<int>(std::lround(std::clamp(stretch, 0.0f, 1000.0f) * 64.0f)) * 4 +
                    std::clamp(detail, 0, 2);
    for (auto& [existing, mesh] : m_CapsuleMeshes)
    {
        if (existing == key)
        {
            return &mesh;
        }
    }

    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    BuildCapsuleMeshAt(vertices, indices, static_cast<float>(key / 4) / 64.0f,
                       std::clamp(detail, 0, 2));

    MeshBuffers mesh;
    if (!UploadMesh(m_Device, vertices.data(), vertices.size(), indices, mesh))
    {
        return &m_SphereMesh;
    }
    KeepGeometry(mesh, std::move(vertices), std::move(indices));
    m_CapsuleMeshes.push_back({ key, mesh });
    return &m_CapsuleMeshes.back().second;
}

int Renderer3D::DetailFor(const Vec3& position, float radius, const RenderSettings& settings) const
{
    if (!settings.Detail.Enabled)
    {
        return 0;
    }

    // How much of the view's height the object covers: its size on screen is
    // what decides whether a simpler shape would be noticed, not its distance
    // alone.
    const Vec3 offset{ position.X - m_FrameCamera.Position.X, position.Y - m_FrameCamera.Position.Y,
                       position.Z - m_FrameCamera.Position.Z };
    const float distance = std::sqrt(DotProduct(offset, offset));
    if (distance <= radius)
    {
        return 0;
    }

    const float halfHeightAtDistance = std::tan(m_FrameCamera.FieldOfViewRadians * 0.5f) * distance;
    const float coverage = radius / std::max(halfHeightAtDistance, 1e-4f);

    if (coverage < settings.Detail.Simplest)
    {
        return 2;
    }
    if (coverage < settings.Detail.Simpler)
    {
        return 1;
    }
    return 0;
}

MeshBuffers* Renderer3D::MeshForModel(ModelStore& models, int modelIndex)
{
    const ModelMesh* source = models.Get(modelIndex);
    if (source == nullptr)
    {
        return nullptr;
    }

    // Still being parsed in the background: nothing to upload yet, and the
    // caller draws the placeholder instead.
    if (!source->Ready || source->Indices.empty())
    {
        return nullptr;
    }

    const size_t index = static_cast<size_t>(modelIndex);
    while (m_ModelMeshes.size() <= index)
    {
        m_ModelMeshes.push_back(std::make_unique<MeshBuffers>());
        m_ModelRevisions.push_back(0);
    }

    MeshBuffers& target = *m_ModelMeshes[index];
    if (target.IndexCount != 0 && m_ModelRevisions[index] == source->Revision)
    {
        return &target;
    }

    // Two new meshes a frame: a dozen models landing together would put
    // megabytes on the bus in one frame, which is the hitch that background
    // loading exists to avoid. The rest keep their placeholder and arrive
    // over the next few frames.
    constexpr int UploadsPerFrame = 2;
    if (m_UploadsThisFrame >= UploadsPerFrame)
    {
        return target.IndexCount != 0 ? &target : nullptr;
    }
    ++m_UploadsThisFrame;

    // A model that arrived after its placeholder was drawn replaces it.
    if (target.IndexCount != 0)
    {
        ReleaseMesh(m_Device, target);
    }
    m_ModelRevisions[index] = source->Revision;

    // First draw: upload. The model store holds plain CPU data so that loading
    // never depends on a device existing, and its vertex layout is the
    // renderer's, so nothing needs converting.
    if (!UploadMesh(m_Device, source->Vertices.data(), source->Vertices.size(), source->Indices, target))
    {
        LogMessage(LogLevel::Error, "render", "Could not upload the mesh for \"%s\".", source->Path.c_str());
        return nullptr;
    }
    target.CpuVertices = source->Vertices.data();
    target.CpuVertexCount = static_cast<uint32_t>(source->Vertices.size());
    target.CpuIndices = source->Indices.data();
    target.CpuIndexCount = static_cast<uint32_t>(source->Indices.size());

    // Morph displacements go up with the geometry and stay there; the frame
    // only sends which targets are in use and how much.
    if (!source->MorphDeltas.empty())
    {
        target.MorphDeltas =
            UploadStorage(m_Device, source->MorphDeltas.data(), source->MorphDeltas.size() * sizeof(float));
        if (target.MorphDeltas == nullptr)
        {
            LogMessage(LogLevel::Error, "render", "Could not upload the morph targets of \"%s\"; it draws unmorphed.",
                       source->Path.c_str());
        }
    }

    return &target;
}

Mat4 Renderer3D::PreviousModelFor(uint32_t index, uint32_t generation, const Mat4& current)
{
    if (m_CurrentModels.size() <= index)
    {
        m_CurrentModels.resize(index + 1, { 0u, Mat4{} });
    }
    m_CurrentModels[index] = { generation, current };

    if (index < m_PreviousModels.size() && m_PreviousModels[index].first == generation)
    {
        return m_PreviousModels[index].second;
    }
    return current;
}

void Renderer3D::AddInstance(const PendingInstance& instance, SurfaceVariant variant, bool behind)
{
    if (instance.Mesh == nullptr || instance.IndexCount == 0)
    {
        return;
    }

    const uint32_t index = static_cast<uint32_t>(m_Instances.size());
    m_Instances.push_back(instance);

    switch (variant)
    {
        case SurfaceVariant::Opaque:      m_OpaqueOrder.push_back(index); break;
        case SurfaceVariant::Transparent: m_TransparentOrder.push_back(index); break;
        case SurfaceVariant::Overlay:     (behind ? m_OverlayBehind : m_OverlayFront).push_back(index); break;
        case SurfaceVariant::Count:       break;
    }
}

void Renderer3D::AddShadowCaster(const PendingInstance& instance)
{
    if (instance.Mesh == nullptr || instance.IndexCount == 0)
    {
        return;
    }

    const uint32_t index = static_cast<uint32_t>(m_Instances.size());
    m_Instances.push_back(instance);
    m_ShadowOrder.push_back(index);
}

void Renderer3D::Render(World3DState& state)
{
    LUDIFEX_PROFILE("render");
    ResetFrame();

    // Material clocks advance and changed shader files recompile here, so a
    // program never has to call anything extra to get hot reload.
    GetWorldMaterials().Update();

    if (m_Device == nullptr || m_Output == nullptr || !m_PipelinesBuilt)
    {
        return;
    }

    const Camera3D& camera = state.Camera;
    const RenderSettings& settings = state.Render;

    FrameCamera frame;
    const float aspect = static_cast<float>(m_Width) / static_cast<float>(m_Height);
    frame.FieldOfViewRadians = camera.FieldOfViewDegrees * Pi / 180.0f;
    frame.NearPlane = camera.NearPlane;
    frame.FarPlane = camera.FarPlane;
    frame.Projection = Mat4::Perspective(frame.FieldOfViewRadians, aspect, camera.NearPlane, camera.FarPlane);
    frame.View = Mat4::LookAt(camera.Position, camera.Target, camera.Up);
    frame.Position = camera.Position;
    frame.Forward = NormalizeVector(Vec3{ camera.Target.X - camera.Position.X, camera.Target.Y - camera.Position.Y,
                                          camera.Target.Z - camera.Position.Z });
    frame.Up = camera.Up;

    const Mat4 viewProjection = frame.Projection * frame.View;
    m_FrameCamera = frame;

    const Frustum frustum = Frustum::FromViewProjection(viewProjection);

    m_ShadowsThisFrame = settings.Shadows.Enabled && settings.Light.Intensity > 0.0f;
    Frustum shadowFrustum;
    if (m_ShadowsThisFrame)
    {
        PlanShadows(frame, settings);
        shadowFrustum = Frustum::FromViewProjection(m_ShadowViewProjection);
    }

    // A tracer sees the whole world, not just what is in view: a reflection
    // shows what is behind the camera, and a shadow falls from what is off
    // screen. So with tracing on, every visible actor goes to the ray scene.
    const RayTracingSettings& rays = settings.RayTracing;
    const bool collectRays = m_RaysSupported && m_Rays != nullptr &&
                             (settings.PathTracing.Enabled || rays.Shadows || rays.AmbientOcclusion || rays.Reflections);
    if (collectRays)
    {
        m_Rays->BeginFrame();
    }

    m_CurrentModels.assign(state.Actors.size(), { 0u, Mat4{} });

    for (uint32_t actorIndex = 0; actorIndex < state.Actors.size(); ++actorIndex)
    {
        const ActorRecord3& record = state.Actors[actorIndex];
        const Appearance& look = record.Look;
        if (!record.Alive || !look.Visible)
        {
            continue;
        }

        // The interpolated transform, so motion is smooth however far the frame
        // rate sits from the physics rate.
        Transform3 transform = LerpTransform(record.Previous, record.Current, state.Alpha);
        transform.Scale = record.Scale;

        // Objects outside the view are rejected here, so they never reach the
        // instance buffer or the GPU. An object off screen may still cast a
        // shadow into the view.
        const bool inView = frustum.ContainsSphere(transform.Position, record.BoundingRadius);
        const bool castsShadow =
            m_ShadowsThisFrame && !look.Unlit && shadowFrustum.ContainsSphere(transform.Position, record.BoundingRadius);

        if (!inView)
        {
            ++m_CulledCount;
            if (!castsShadow && !collectRays)
            {
                continue;
            }
        }

        const Mat4 model = Mat4::FromTransform(transform);

        PendingInstance base;
        base.Model = model;
        base.PreviousModel = PreviousModelFor(actorIndex, record.Generation, model);
        base.Tint = ToLinear(look.Tint);
        base.Roughness = look.Roughness;
        base.Metallic = look.Metallic;
        base.UVScale = look.UVScale;
        base.UVOffset = look.UVOffset;
        base.Flags = (look.Unlit ? InstanceFlagUnlit : 0u) | (look.FlipX ? InstanceFlagFlipX : 0u);
        base.Texture = look.Texture;
        base.NormalMap = look.NormalMap;
        base.NormalStrength = look.NormalMap.IsValid() ? look.NormalStrength : 0.0f;
        base.Emission = Vec3{ look.Emission.R, look.Emission.G, look.Emission.B };
        base.ParamMask = look.ParamMask;
        base.Params = look.Params;
        base.ActorKey = (static_cast<uint64_t>(actorIndex) << 32) | record.Generation;

        // Borrowed, not copied: the palette belongs to the actor and outlives
        // this frame. It is empty for everything that does not bend, and the
        // upload treats that as "not skinned".
        if (!record.Animation.Palette.empty())
        {
            base.Palette = &record.Animation.Palette;
        }
        if (!record.Animation.MorphWeights.empty())
        {
            base.MorphWeights = &record.Animation.MorphWeights;
        }

        WorldMaterialRecord* material = GetWorldMaterials().Resolve(look.Material);
        base.MaterialIndex = material != nullptr ? static_cast<int>(look.Material.Index) : -1;
        const bool materialTransparent = material != nullptr && material->Transparent;

        const Vec3 toObject{ transform.Position.X - frame.Position.X, transform.Position.Y - frame.Position.Y,
                             transform.Position.Z - frame.Position.Z };
        base.Depth = Dot(toObject, frame.Forward);

        auto Submit = [&](const PendingInstance& instance, bool transparent) {
            if (inView)
            {
                AddInstance(instance, transparent ? SurfaceVariant::Transparent : SurfaceVariant::Opaque);
            }
            // Translucent objects let light through, so they cast nothing.
            if (castsShadow && !transparent)
            {
                AddShadowCaster(instance);
            }
            if (collectRays)
            {
                m_Rays->Add(instance, transparent);
            }
        };

        if (record.Shape == ShapeKind::Model && state.Models != nullptr)
        {
            const ModelMesh* source = state.Models->Get(record.ModelIndex);
            MeshBuffers* mesh = MeshForModel(*state.Models, record.ModelIndex);
            if (source == nullptr || mesh == nullptr)
            {
                // A model still being parsed in the background draws as a
                // plain box of its own scale, so the actor is there from the
                // first frame and changes shape when the file arrives.
                if (record.AwaitingModel)
                {
                    PendingInstance placeholder = base;
                    placeholder.Mesh = &m_BoxMesh;
                    placeholder.IndexCount = m_BoxMesh.IndexCount;
                    Submit(placeholder, placeholder.Tint.A < 0.999f || materialTransparent);
                }
                continue;
            }

            if (source->HasMorphs() && mesh->MorphDeltas != nullptr)
            {
                base.MorphTargets = &source->MorphTargets;
            }
            base.SourceModel = source;

            for (const ModelPart& part : source->Parts)
            {
                PendingInstance instance = base;
                instance.Mesh = mesh;
                instance.FirstIndex = part.FirstIndex;
                instance.IndexCount = part.IndexCount;

                // The file's own look, under the actor's: a texture set on the
                // actor replaces the file's, and the actor's colour tints it.
                if (!look.Texture.IsValid())
                {
                    instance.Texture = part.BaseColorTexture;
                }
                if (!look.NormalMap.IsValid() && part.NormalTexture.IsValid())
                {
                    instance.NormalMap = part.NormalTexture;
                    instance.NormalStrength = part.NormalScale;
                }
                instance.MetallicRoughnessMap = part.MetallicRoughnessTexture;
                instance.OcclusionMap = part.OcclusionTexture;
                instance.OcclusionStrength = part.OcclusionStrength;
                if (!look.EmissionOverridden)
                {
                    instance.EmissiveMap = part.EmissiveTexture;
                    instance.Emission = part.EmissiveFactor;
                }
                instance.Tint = Color{ base.Tint.R * part.BaseColorFactor.R, base.Tint.G * part.BaseColorFactor.G,
                                       base.Tint.B * part.BaseColorFactor.B, base.Tint.A * part.BaseColorFactor.A };
                if (part.HasMaterial && !look.SurfaceOverridden)
                {
                    instance.Roughness = part.Roughness;
                    instance.Metallic = part.Metallic;
                }

                Submit(instance, instance.Tint.A < 0.999f || part.Transparent || materialTransparent);
            }
        }
        else if (record.Shape == ShapeKind::Capsule)
        {
            // The actor's extents are diameter by height by diameter; the mesh
            // is built for that proportion and scaled uniformly, which keeps
            // the caps round.
            const float diameter = std::max(record.Scale.X, 1e-4f);
            const float stretch = std::max(0.0f, 0.5f * (record.Scale.Y / diameter - 1.0f));

            Transform3 uniform = transform;
            uniform.Scale = Vec3{ diameter, diameter, record.Scale.Z };

            PendingInstance instance = base;
            instance.Model = Mat4::FromTransform(uniform);
            instance.PreviousModel = PreviousModelFor(actorIndex, record.Generation, instance.Model);
            instance.Mesh =
                MeshForCapsule(stretch, DetailFor(transform.Position, record.BoundingRadius, settings));
            instance.IndexCount = instance.Mesh->IndexCount;
            Submit(instance, instance.Tint.A < 0.999f || materialTransparent);
        }
        else if (record.Shape == ShapeKind::Sphere)
        {
            // A sphere far enough away to cover a few pixels is drawn from a
            // simpler mesh; at that size the difference is arithmetic rather
            // than anything anyone can see.
            PendingInstance instance = base;
            instance.Mesh = &m_SphereMeshes[DetailFor(transform.Position, record.BoundingRadius, settings)];
            instance.IndexCount = instance.Mesh->IndexCount;
            Submit(instance, instance.Tint.A < 0.999f || materialTransparent);
        }
        else
        {
            PendingInstance instance = base;
            instance.Mesh = MeshForShape(record.Shape);
            instance.IndexCount = instance.Mesh->IndexCount;
            Submit(instance, instance.Tint.A < 0.999f || materialTransparent);
        }
    }

    // Point lights: an attached light follows its actor's interpolated pose.
    struct Candidate
    {
        GpuPointLight Light;
        float Importance;
    };
    std::vector<Candidate> candidates;

    for (LightRecord3& light : state.Lights)
    {
        if (!light.Alive)
        {
            continue;
        }

        if (light.AttachedTo.IsValid())
        {
            const uint32_t index = light.AttachedTo.Index;
            if (index < state.Actors.size() && state.Actors[index].Alive &&
                state.Actors[index].Generation == light.AttachedTo.Generation)
            {
                const ActorRecord3& actor = state.Actors[index];
                const Transform3 pose = LerpTransform(actor.Previous, actor.Current, state.Alpha);
                const Vec3 offset = Rotate(pose.Rotation, light.Offset);
                light.Desc.Position = Vec3{ pose.Position.X + offset.X, pose.Position.Y + offset.Y,
                                            pose.Position.Z + offset.Z };
            }
            else
            {
                // The actor is gone; the light stays where it last was.
                light.AttachedTo = ActorId{};
            }
        }

        const PointLightDesc& desc = light.Desc;
        if (desc.Intensity <= 0.0f || desc.Range <= 0.0f ||
            !frustum.ContainsSphere(desc.Position, desc.Range))
        {
            continue;
        }

        const Color linear = ToLinear(desc.Tint);
        Candidate candidate{};
        candidate.Light.PositionRange[0] = desc.Position.X;
        candidate.Light.PositionRange[1] = desc.Position.Y;
        candidate.Light.PositionRange[2] = desc.Position.Z;
        candidate.Light.PositionRange[3] = desc.Range;
        candidate.Light.ColorIntensity[0] = linear.R;
        candidate.Light.ColorIntensity[1] = linear.G;
        candidate.Light.ColorIntensity[2] = linear.B;
        candidate.Light.ColorIntensity[3] = desc.Intensity;

        const Vec3 toLight{ desc.Position.X - frame.Position.X, desc.Position.Y - frame.Position.Y,
                            desc.Position.Z - frame.Position.Z };
        candidate.Importance = desc.Intensity * desc.Range * desc.Range / (1.0f + Dot(toLight, toLight));
        candidates.push_back(candidate);
    }

    // With more lights than the shader takes, the ones contributing most to
    // what the camera sees win.
    if (candidates.size() > MaxPointLights)
    {
        std::partial_sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(MaxPointLights),
                          candidates.end(),
                          [](const Candidate& a, const Candidate& b) { return a.Importance > b.Importance; });
        candidates.resize(MaxPointLights);
    }
    for (const Candidate& candidate : candidates)
    {
        m_Lights.push_back(candidate.Light);
    }
    if (collectRays)
    {
        m_Rays->SetLights(m_Lights);
    }

    // Collider bounds and joint anchors, when asked for.
    if (state.PhysicsDebugDraw)
    {
        for (const ActorRecord3& record : state.Actors)
        {
            if (!record.Alive)
            {
                continue;
            }
            const Transform3 pose = LerpTransform(record.Previous, record.Current, state.Alpha);
            const Vec3 half{ record.Scale.X * 0.5f, record.Scale.Y * 0.5f, record.Scale.Z * 0.5f };
            const Color color = record.IsSensor ? Color{ 0.3f, 0.9f, 1.0f, 1.0f } : Color{ 0.4f, 1.0f, 0.4f, 1.0f };

            Vec3 corners[8];
            for (int corner = 0; corner < 8; ++corner)
            {
                const Vec3 local{ (corner & 1) ? half.X : -half.X, (corner & 2) ? half.Y : -half.Y,
                                  (corner & 4) ? half.Z : -half.Z };
                const Vec3 rotated = Rotate(pose.Rotation, local);
                corners[corner] = Vec3{ pose.Position.X + rotated.X, pose.Position.Y + rotated.Y,
                                        pose.Position.Z + rotated.Z };
            }
            const int edges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 },
                                       { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
            for (const auto& edge : edges)
            {
                state.Extras.DebugLines.push_back(DebugLine{ corners[edge[0]], corners[edge[1]], color });
            }
        }

        for (const JointRecord3& joint : state.Joints)
        {
            if (!joint.Alive || !b3Joint_IsValid(joint.Joint))
            {
                continue;
            }
            const b3BodyId bodyA = b3Joint_GetBodyA(joint.Joint);
            const b3BodyId bodyB = b3Joint_GetBodyB(joint.Joint);
            const Vec3 a = FromB3Pos(b3Body_GetPosition(bodyA));
            const Vec3 b = FromB3Pos(b3Body_GetPosition(bodyB));
            state.Extras.DebugLines.push_back(DebugLine{ a, b, Color{ 1.0f, 0.5f, 0.2f, 1.0f } });
        }
    }

    RenderFrame(frame, settings, state.Extras, false);

    m_PreviousModels.swap(m_CurrentModels);
}

void Renderer3D::Render2D(World2DState& state)
{
    ResetFrame();
    GetWorldMaterials().Update();

    if (m_Device == nullptr || m_Output == nullptr || !m_PipelinesBuilt)
    {
        return;
    }

    const Camera2D& camera = state.Camera;

    // The viewer sits on the +Z axis looking back at the plane, which is where
    // the lighting and the rim term are measured from.
    FrameCamera frame;
    frame.Position = Vec3{ camera.Center.X, camera.Center.Y, 20.0f };
    frame.Forward = Vec3{ 0.0f, 0.0f, -1.0f };
    frame.NearPlane = 0.1f;
    frame.FarPlane = 100.0f;
    frame.Orthographic = true;

    const float aspect = static_cast<float>(m_Width) / static_cast<float>(m_Height);
    const float halfHeight = std::max(0.01f, camera.Height * 0.5f);
    const float halfWidth = halfHeight * aspect;

    // Height is in world units, so widening the window shows more of the world
    // rather than stretching what is already there. The bounds are relative to
    // the eye: the view matrix has already moved the camera centre to the
    // origin, so centring them on it again would shift the scene by twice the
    // camera's offset.
    frame.Projection = Mat4::Orthographic(-halfWidth, halfWidth, -halfHeight, halfHeight, frame.NearPlane,
                                          frame.FarPlane);
    frame.View = Mat4::LookAt(frame.Position, Vec3{ camera.Center.X, camera.Center.Y, 0.0f },
                              Vec3{ 0.0f, 1.0f, 0.0f });

    const Frustum frustum = Frustum::FromViewProjection(frame.Projection * frame.View);

    // A 2D world has no directional shadows: the light comes from in front of
    // a flat scene.
    m_ShadowsThisFrame = false;
    m_CurrentModels.assign(state.Actors.size(), { 0u, Mat4{} });

    for (uint32_t actorIndex = 0; actorIndex < state.Actors.size(); ++actorIndex)
    {
        const ActorRecord2& record = state.Actors[actorIndex];
        const Appearance& look = record.Look;
        if (!record.Alive || !look.Visible)
        {
            continue;
        }

        const Transform2 flat = LerpTransform(record.Previous, record.Current, state.Alpha);
        const float halfAngle = flat.Rotation * 0.5f;

        Transform3 transform;
        transform.Position = Vec3{ flat.Position.X, flat.Position.Y, 0.0f };
        transform.Rotation = Quat{ 0.0f, 0.0f, std::sin(halfAngle), std::cos(halfAngle) };

        const float radius = 0.5f * std::sqrt(record.Scale.X * record.Scale.X + record.Scale.Y * record.Scale.Y);
        if (!frustum.ContainsSphere(transform.Position, radius))
        {
            ++m_CulledCount;
            continue;
        }

        PendingInstance instance;
        instance.Tint = ToLinear(look.Tint);
        instance.Roughness = look.Roughness;
        instance.Metallic = look.Metallic;
        instance.UVScale = look.UVScale;
        instance.UVOffset = look.UVOffset;
        instance.Flags = (look.Unlit ? InstanceFlagUnlit : 0u) | (look.FlipX ? InstanceFlagFlipX : 0u);
        instance.Texture = look.Texture;
        instance.NormalMap = look.NormalMap;
        instance.NormalStrength = look.NormalMap.IsValid() ? look.NormalStrength : 0.0f;
        instance.Emission = Vec3{ look.Emission.R, look.Emission.G, look.Emission.B };
        instance.ParamMask = look.ParamMask;
        instance.Params = look.Params;
        instance.Layer = look.Layer;
        instance.Sequence = record.Sequence;

        WorldMaterialRecord* material = GetWorldMaterials().Resolve(look.Material);
        instance.MaterialIndex = material != nullptr ? static_cast<int>(look.Material.Index) : -1;
        const bool materialTransparent = material != nullptr && material->Transparent;

        bool overlay;
        if (record.IsSprite)
        {
            // A flat quad showing its image, drawn in layer order over (or,
            // on a negative layer, under) the shapes.
            transform.Scale = Vec3{ record.Scale.X, record.Scale.Y, 1.0f };
            instance.Mesh = &m_QuadMesh;
            instance.ClampSampler = true;
            overlay = true;
        }
        else
        {
            // Shapes are given a little depth so the directional light still
            // models them. Flat fill reads worse and costs the same.
            const float thickness = std::max(0.05f, std::min(record.Scale.X, record.Scale.Y) * 0.5f);
            if (record.Shape == ShapeKind2::Capsule)
            {
                // Scaled uniformly in the plane, as a 3D capsule is, so the
                // caps stay round.
                const float diameter = std::max(record.Scale.X, 1e-4f);
                transform.Scale = Vec3{ diameter, diameter, thickness };
                instance.Mesh = MeshForCapsule(std::max(0.0f, 0.5f * (record.Scale.Y / diameter - 1.0f)));
            }
            else
            {
                transform.Scale = Vec3{ record.Scale.X, record.Scale.Y, thickness };
                instance.Mesh = (record.Shape == ShapeKind2::Circle) ? &m_SphereMesh : &m_BoxMesh;
            }
            overlay = instance.Tint.A < 0.999f || materialTransparent;
        }

        instance.IndexCount = instance.Mesh->IndexCount;
        instance.Model = Mat4::FromTransform(transform);
        instance.PreviousModel = PreviousModelFor(actorIndex, record.Generation, instance.Model);

        if (overlay)
        {
            AddInstance(instance, SurfaceVariant::Overlay, look.Layer < 0);
        }
        else
        {
            AddInstance(instance, SurfaceVariant::Opaque);
        }
    }

    RenderFrame(frame, state.Render, state.Extras, true);

    m_PreviousModels.swap(m_CurrentModels);
}

// ---------------------------------------------------------------------------
// The frame
// ---------------------------------------------------------------------------

bool Renderer3D::UploadFrameData(SDL_GPUCommandBuffer* commandBuffer)
{
    // Opaque objects sort by material, then texture, then mesh: a material
    // change is a pipeline change and the costliest switch, and every run of
    // objects sharing all three becomes one instanced draw.
    std::stable_sort(m_OpaqueOrder.begin(), m_OpaqueOrder.end(), [&](uint32_t a, uint32_t b) {
        const PendingInstance& x = m_Instances[a];
        const PendingInstance& y = m_Instances[b];
        if (x.MaterialIndex != y.MaterialIndex)
        {
            return x.MaterialIndex < y.MaterialIndex;
        }
        if (TextureKey(x.Texture) != TextureKey(y.Texture))
        {
            return TextureKey(x.Texture) < TextureKey(y.Texture);
        }
        if (TextureKey(x.NormalMap) != TextureKey(y.NormalMap))
        {
            return TextureKey(x.NormalMap) < TextureKey(y.NormalMap);
        }
        if (TextureKey(x.MetallicRoughnessMap) != TextureKey(y.MetallicRoughnessMap))
        {
            return TextureKey(x.MetallicRoughnessMap) < TextureKey(y.MetallicRoughnessMap);
        }
        if (TextureKey(x.EmissiveMap) != TextureKey(y.EmissiveMap))
        {
            return TextureKey(x.EmissiveMap) < TextureKey(y.EmissiveMap);
        }
        if (TextureKey(x.OcclusionMap) != TextureKey(y.OcclusionMap))
        {
            return TextureKey(x.OcclusionMap) < TextureKey(y.OcclusionMap);
        }
        if (x.Mesh != y.Mesh)
        {
            return x.Mesh < y.Mesh;
        }
        return x.FirstIndex < y.FirstIndex;
    });

    // Translucent objects blend correctly only drawn far to near.
    std::stable_sort(m_TransparentOrder.begin(), m_TransparentOrder.end(),
                     [&](uint32_t a, uint32_t b) { return m_Instances[a].Depth > m_Instances[b].Depth; });

    // 2D overlays follow their layer, then the order they were created in.
    auto LayerOrder = [&](uint32_t a, uint32_t b) {
        const PendingInstance& x = m_Instances[a];
        const PendingInstance& y = m_Instances[b];
        if (x.Layer != y.Layer)
        {
            return x.Layer < y.Layer;
        }
        return x.Sequence < y.Sequence;
    };
    std::stable_sort(m_OverlayBehind.begin(), m_OverlayBehind.end(), LayerOrder);
    std::stable_sort(m_OverlayFront.begin(), m_OverlayFront.end(), LayerOrder);

    // Shadow casters only need grouping by geometry.
    std::stable_sort(m_ShadowOrder.begin(), m_ShadowOrder.end(), [&](uint32_t a, uint32_t b) {
        const PendingInstance& x = m_Instances[a];
        const PendingInstance& y = m_Instances[b];
        if (x.Mesh != y.Mesh)
        {
            return x.Mesh < y.Mesh;
        }
        return x.FirstIndex < y.FirstIndex;
    });

    // Every list goes into one buffer, each contiguous in its own draw order.
    const std::vector<uint32_t>* lists[] = { &m_OpaqueOrder, &m_TransparentOrder, &m_OverlayBehind,
                                             &m_OverlayFront, &m_ShadowOrder };
    uint32_t* bases[] = { &m_OpaqueBase, &m_TransparentBase, &m_BehindBase, &m_FrontBase, &m_ShadowBase };

    std::vector<GpuInstance> gpuInstances;
    gpuInstances.reserve(m_Instances.size());

    for (size_t list = 0; list < 5; ++list)
    {
        *bases[list] = static_cast<uint32_t>(gpuInstances.size());
        for (uint32_t index : *lists[list])
        {
            PendingInstance& instance = m_Instances[index];

            if (instance.ParamMask != 0 && instance.Params != nullptr)
            {
                instance.ParamOffset = static_cast<uint32_t>(m_ParamData.size() / 4);
                for (int slot = 0; slot < 8; ++slot)
                {
                    m_ParamData.insert(m_ParamData.end(), instance.Params[slot], instance.Params[slot] + 4);
                }
            }

            // A skinned actor's joints are appended to the frame's shared
            // palette and the instance records where its own block begins, so
            // a crowd in different poses is still one buffer.
            if (instance.Palette != nullptr && !instance.Palette->empty())
            {
                instance.PaletteOffset = static_cast<uint32_t>(m_JointMatrices.size());
                instance.Flags |= InstanceFlagSkinned;
                m_JointMatrices.insert(m_JointMatrices.end(), instance.Palette->begin(),
                                       instance.Palette->end());
            }

            // A morphing actor's targets in use go into the frame's table the
            // same way: the instance says where its run starts and how long it
            // is, and a target at zero is left out altogether.
            instance.MorphCount = 0;
            if (instance.MorphTargets != nullptr)
            {
                const std::vector<MorphTarget>& targets = *instance.MorphTargets;
                const std::vector<float>* weights =
                    instance.MorphWeights != nullptr && instance.MorphWeights->size() == targets.size()
                        ? instance.MorphWeights
                        : nullptr;

                instance.MorphOffset = static_cast<uint32_t>(m_MorphChannels.size() / 4);
                for (size_t target = 0; target < targets.size(); ++target)
                {
                    const float weight = weights != nullptr ? (*weights)[target] : targets[target].DefaultWeight;
                    if (std::abs(weight) < 1e-4f || targets[target].VertexCount == 0)
                    {
                        continue;
                    }

                    uint32_t bits = 0;
                    std::memcpy(&bits, &weight, sizeof(bits));
                    m_MorphChannels.insert(m_MorphChannels.end(), { targets[target].DeltaOffset,
                                                                    targets[target].VertexStart,
                                                                    targets[target].VertexCount, bits });
                    ++instance.MorphCount;
                }
            }

            GpuInstance gpu{};
            CopyMatrix(gpu.Model, instance.Model);
            CopyMatrix(gpu.PreviousModel, instance.PreviousModel);
            gpu.BaseColor[0] = instance.Tint.R;
            gpu.BaseColor[1] = instance.Tint.G;
            gpu.BaseColor[2] = instance.Tint.B;
            gpu.BaseColor[3] = instance.Tint.A;
            gpu.Surface[0] = instance.Roughness;
            gpu.Surface[1] = instance.Metallic;
            gpu.Surface[2] = instance.OcclusionStrength;
            gpu.Surface[3] = instance.NormalStrength;
            gpu.Emission[0] = instance.Emission.X;
            gpu.Emission[1] = instance.Emission.Y;
            gpu.Emission[2] = instance.Emission.Z;
            gpu.Emission[3] = 0.0f;
            gpu.UVTransform[0] = instance.UVScale.X;
            gpu.UVTransform[1] = instance.UVScale.Y;
            gpu.UVTransform[2] = instance.UVOffset.X;
            gpu.UVTransform[3] = instance.UVOffset.Y;
            gpu.Extra[0] = instance.ParamOffset;
            gpu.Extra[1] = instance.ParamMask;
            gpu.Extra[2] = instance.Flags;
            gpu.Extra[3] = instance.PaletteOffset;
            gpu.Morph[0] = instance.MorphOffset;
            gpu.Morph[1] = instance.MorphCount;
            gpuInstances.push_back(gpu);
        }
    }

    m_InstanceCount = static_cast<uint32_t>(m_OpaqueOrder.size() + m_TransparentOrder.size() +
                                            m_OverlayBehind.size() + m_OverlayFront.size());

    // Storage buffers are bound whether or not anything reads them, so each
    // gets at least one element.
    if (m_ParamData.empty())
    {
        m_ParamData.assign(4, 0.0f);
    }
    if (gpuInstances.empty())
    {
        gpuInstances.push_back(GpuInstance{});
    }
    if (m_JointMatrices.empty())
    {
        m_JointMatrices.push_back(Mat4::Identity());
    }
    if (m_MorphChannels.empty())
    {
        m_MorphChannels.assign(4, 0u);
    }
    std::vector<GpuPointLight> lights = m_Lights;
    if (lights.empty())
    {
        lights.push_back(GpuPointLight{});
    }

    if (m_ClusterRanges.empty())
    {
        m_ClusterRanges.assign(static_cast<size_t>(ClusterCount) * 2, 0u);
    }
    if (m_ClusterIndices.empty())
    {
        m_ClusterIndices.push_back(0u);
    }

    const size_t instanceBytes = gpuInstances.size() * sizeof(GpuInstance);
    const size_t jointBytes = m_JointMatrices.size() * sizeof(Mat4);
    const size_t morphBytes = m_MorphChannels.size() * sizeof(uint32_t);
    const size_t paramBytes = m_ParamData.size() * sizeof(float);
    const size_t lightBytes = lights.size() * sizeof(GpuPointLight);
    const size_t clusterRangeBytes = m_ClusterRanges.size() * sizeof(uint32_t);
    const size_t clusterIndexBytes = m_ClusterIndices.size() * sizeof(uint32_t);
    const size_t lineBytes = m_LineVertices.size() * sizeof(float);

    const SDL_GPUBufferUsageFlags storage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    if (!EnsureBuffer(m_Device, m_InstanceBuffer, m_InstanceCapacity, instanceBytes, storage) ||
        !EnsureBuffer(m_Device, m_JointBuffer, m_JointCapacity, jointBytes, storage) ||
        !EnsureBuffer(m_Device, m_MorphBuffer, m_MorphCapacity, morphBytes, storage) ||
        !EnsureBuffer(m_Device, m_ParamBuffer, m_ParamCapacity, paramBytes, storage) ||
        !EnsureBuffer(m_Device, m_LightBuffer, m_LightCapacity, lightBytes, storage) ||
        !EnsureBuffer(m_Device, m_ClusterRangeBuffer, m_ClusterRangeCapacity, clusterRangeBytes,
                      storage) ||
        !EnsureBuffer(m_Device, m_ClusterIndexBuffer, m_ClusterIndexCapacity, clusterIndexBytes,
                      storage))
    {
        return false;
    }
    if (lineBytes > 0 &&
        !EnsureBuffer(m_Device, m_LineBuffer, m_LineCapacity, lineBytes, SDL_GPU_BUFFERUSAGE_VERTEX))
    {
        return false;
    }

    const size_t totalBytes = instanceBytes + jointBytes + morphBytes + paramBytes + lightBytes +
                              clusterRangeBytes + clusterIndexBytes + lineBytes;
    if (m_Transfer == nullptr || m_TransferCapacity < totalBytes)
    {
        if (m_Transfer != nullptr)
        {
            SDL_ReleaseGPUTransferBuffer(m_Device, m_Transfer);
        }
        size_t size = std::max<size_t>(m_TransferCapacity, 65536);
        while (size < totalBytes)
        {
            size *= 2;
        }
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = static_cast<uint32_t>(size);
        m_Transfer = SDL_CreateGPUTransferBuffer(m_Device, &info);
        m_TransferCapacity = m_Transfer != nullptr ? size : 0;
        if (m_Transfer == nullptr)
        {
            return false;
        }
    }

    // Cycling lets this frame's copy start while the GPU may still be reading
    // the previous frame's, so the CPU never waits here.
    auto* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(m_Device, m_Transfer, true));
    if (mapped == nullptr)
    {
        return false;
    }
    size_t offset = 0;
    std::memcpy(mapped + offset, gpuInstances.data(), instanceBytes);
    offset += instanceBytes;
    std::memcpy(mapped + offset, m_JointMatrices.data(), jointBytes);
    offset += jointBytes;
    std::memcpy(mapped + offset, m_MorphChannels.data(), morphBytes);
    offset += morphBytes;
    std::memcpy(mapped + offset, m_ParamData.data(), paramBytes);
    offset += paramBytes;
    std::memcpy(mapped + offset, lights.data(), lightBytes);
    offset += lightBytes;
    std::memcpy(mapped + offset, m_ClusterRanges.data(), clusterRangeBytes);
    offset += clusterRangeBytes;
    std::memcpy(mapped + offset, m_ClusterIndices.data(), clusterIndexBytes);
    offset += clusterIndexBytes;
    if (lineBytes > 0)
    {
        std::memcpy(mapped + offset, m_LineVertices.data(), lineBytes);
    }
    SDL_UnmapGPUTransferBuffer(m_Device, m_Transfer);

    // The copy has to happen outside any render pass, so it is issued before
    // the first pass opens.
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);

    auto Upload = [&](SDL_GPUBuffer* buffer, size_t sourceOffset, size_t bytes) {
        SDL_GPUTransferBufferLocation source{};
        source.transfer_buffer = m_Transfer;
        source.offset = static_cast<uint32_t>(sourceOffset);
        SDL_GPUBufferRegion region{};
        region.buffer = buffer;
        region.size = static_cast<uint32_t>(bytes);
        SDL_UploadToGPUBuffer(copyPass, &source, &region, true);
    };

    offset = 0;
    Upload(m_InstanceBuffer, offset, instanceBytes);
    offset += instanceBytes;
    Upload(m_JointBuffer, offset, jointBytes);
    offset += jointBytes;
    Upload(m_MorphBuffer, offset, morphBytes);
    offset += morphBytes;
    Upload(m_ParamBuffer, offset, paramBytes);
    offset += paramBytes;
    Upload(m_LightBuffer, offset, lightBytes);
    offset += lightBytes;
    Upload(m_ClusterRangeBuffer, offset, clusterRangeBytes);
    offset += clusterRangeBytes;
    Upload(m_ClusterIndexBuffer, offset, clusterIndexBytes);
    offset += clusterIndexBytes;
    if (lineBytes > 0)
    {
        Upload(m_LineBuffer, offset, lineBytes);
    }

    SDL_EndGPUCopyPass(copyPass);
    return true;
}

void Renderer3D::BindSurfaceResources(SDL_GPUCommandBuffer* commandBuffer, SDL_GPURenderPass* pass,
                                      const PendingInstance& maps, SDL_GPUSampler* baseSampler)
{
    using Fallback = WorldTextureStore::Fallback;
    WorldTextureStore& textures = GetWorldTextures();

    // The order is the shader's: base colour, the shadow map, then the maps a
    // surface may or may not have, each falling back to a texel that changes
    // nothing when it has none, then the frame's screen-space lighting.
    SDL_GPUTextureSamplerBinding samplers[9]{};
    samplers[0].texture = textures.GetGpuTexture(m_Device, maps.Texture);
    samplers[0].sampler = baseSampler;
    samplers[1].texture = m_ShadowsThisFrame && m_ShadowMap != nullptr ? m_ShadowMap : m_NoShadow;
    samplers[1].sampler = m_ShadowSampler;
    samplers[2].texture = textures.GetGpuTexture(m_Device, maps.NormalMap, Fallback::FlatNormal);
    samplers[2].sampler = baseSampler;
    samplers[3].texture = textures.GetGpuTexture(m_Device, maps.MetallicRoughnessMap);
    samplers[3].sampler = baseSampler;
    samplers[4].texture = textures.GetGpuTexture(m_Device, maps.EmissiveMap, Fallback::Black);
    samplers[4].sampler = baseSampler;
    samplers[5].texture = textures.GetGpuTexture(m_Device, maps.OcclusionMap);
    samplers[5].sampler = baseSampler;
    samplers[6].texture = m_ScreenOcclusion != nullptr ? m_ScreenOcclusion : m_White;
    samplers[6].sampler = m_LinearClampSampler;
    samplers[7].texture = m_ScreenSunlight != nullptr ? m_ScreenSunlight : m_White;
    samplers[7].sampler = m_LinearClampSampler;
    samplers[8].texture = m_ScreenReflections != nullptr ? m_ScreenReflections : m_Clear;
    samplers[8].sampler = m_LinearClampSampler;
    SDL_BindGPUFragmentSamplers(pass, 0, samplers, 9);

    BindVertexStorage(pass);

    SDL_GPUBuffer* fragmentBuffers[5] = { m_InstanceBuffer, m_ParamBuffer, m_LightBuffer,
                                          m_ClusterRangeBuffer, m_ClusterIndexBuffer };
    SDL_BindGPUFragmentStorageBuffers(pass, 0, fragmentBuffers, 5);

    SDL_PushGPUFragmentUniformData(commandBuffer, 0, m_SceneUniforms.data(),
                                   static_cast<uint32_t>(m_SceneUniforms.size()));
}

void Renderer3D::BindVertexStorage(SDL_GPURenderPass* pass)
{
    // The displacement slot is filled too, so nothing is ever left unbound;
    // BindMorphDeltas swaps in each mesh's own as the draws need them.
    SDL_GPUBuffer* vertexBuffers[4] = { m_InstanceBuffer, m_JointBuffer, m_MorphBuffer, m_NoMorphDeltas };
    SDL_BindGPUVertexStorageBuffers(pass, 0, vertexBuffers, 4);
}

void Renderer3D::BindMorphDeltas(SDL_GPURenderPass* pass, const MeshBuffers& mesh, SDL_GPUBuffer*& bound)
{
    SDL_GPUBuffer* deltas = mesh.MorphDeltas != nullptr ? mesh.MorphDeltas : m_NoMorphDeltas;
    if (deltas != bound)
    {
        SDL_BindGPUVertexStorageBuffers(pass, 3, &deltas, 1);
        bound = deltas;
    }
}

void Renderer3D::DrawRuns(SDL_GPUCommandBuffer* commandBuffer, SDL_GPURenderPass* pass,
                          const std::vector<uint32_t>& order, uint32_t base, SurfaceVariant variant,
                          const Mat4& viewProjection, const float jitter[2])
{
    if (order.empty())
    {
        return;
    }

    WorldTextureStore& textures = GetWorldTextures();

    SDL_GPUGraphicsPipeline* bound = nullptr;
    const PendingInstance* boundMaps = nullptr;
    SDL_GPUSampler* boundSampler = nullptr;
    SDL_GPUBuffer* boundDeltas = nullptr;

    size_t runStart = 0;
    while (runStart < order.size())
    {
        const PendingInstance& first = m_Instances[order[runStart]];

        size_t runEnd = runStart + 1;
        while (runEnd < order.size())
        {
            const PendingInstance& next = m_Instances[order[runEnd]];
            if (next.Mesh != first.Mesh || next.FirstIndex != first.FirstIndex ||
                next.IndexCount != first.IndexCount || next.MaterialIndex != first.MaterialIndex ||
                !SameMaps(next, first) || next.ClampSampler != first.ClampSampler)
            {
                break;
            }
            ++runEnd;
        }

        SDL_GPUGraphicsPipeline* materialPipeline = SurfacePipelineFor(first.MaterialIndex, variant);
        SDL_GPUGraphicsPipeline* pipeline =
            materialPipeline != nullptr ? materialPipeline : m_DefaultSurface[static_cast<size_t>(variant)];

        SDL_GPUSampler* sampler =
            first.ClampSampler ? textures.GetClampSampler(m_Device) : textures.GetRepeatSampler(m_Device);

        if (pipeline != bound)
        {
            SDL_BindGPUGraphicsPipeline(pass, pipeline);
            bound = pipeline;

            // Bindings are re-supplied after a pipeline change rather than
            // trusted to carry over.
            BindSurfaceResources(commandBuffer, pass, first, sampler);
            boundMaps = &first;
            boundSampler = sampler;
            boundDeltas = m_NoMorphDeltas;
        }
        else if (boundMaps == nullptr || !SameMaps(*boundMaps, first) || sampler != boundSampler)
        {
            BindSurfaceResources(commandBuffer, pass, first, sampler);
            boundMaps = &first;
            boundSampler = sampler;
            boundDeltas = m_NoMorphDeltas;
        }

        BindMorphDeltas(pass, *first.Mesh, boundDeltas);

        if (materialPipeline != nullptr)
        {
            if (WorldMaterialRecord* record = GetWorldMaterials().At(static_cast<size_t>(first.MaterialIndex)))
            {
                SDL_PushGPUFragmentUniformData(commandBuffer, 1, &record->Uniforms, sizeof(MaterialUniformBlock));
            }
        }

        SDL_GPUBufferBinding vertexBinding{};
        vertexBinding.buffer = first.Mesh->Vertices;
        SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);

        SDL_GPUBufferBinding indexBinding{};
        indexBinding.buffer = first.Mesh->Indices;
        SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

        // SV_InstanceID restarts at zero for each draw, so the run's base index
        // into the shared buffer travels in the uniform instead.
        DrawUniforms uniforms{};
        CopyMatrix(uniforms.ViewProjection, viewProjection);
        CopyMatrix(uniforms.PreviousViewProjection, m_HasPreviousViewProjection ? m_PreviousViewProjection : viewProjection);
        uniforms.Jitter[0] = jitter[0];
        uniforms.Jitter[1] = jitter[1];
        uniforms.InstanceOffset = base + static_cast<uint32_t>(runStart);
        SDL_PushGPUVertexUniformData(commandBuffer, 0, &uniforms, sizeof(uniforms));

        SDL_DrawGPUIndexedPrimitives(pass, first.IndexCount, static_cast<uint32_t>(runEnd - runStart),
                                     first.FirstIndex, 0, 0);
        ++m_DrawCalls;

        // The largest run is the measure of instancing: objects sharing a
        // mesh, a material, and a texture go out together however many of
        // them there are, and this says how many that was.
        m_LargestBatch = std::max(m_LargestBatch, static_cast<uint32_t>(runEnd - runStart));

        runStart = runEnd;
    }
}

void Renderer3D::DrawDepthRuns(SDL_GPUCommandBuffer* commandBuffer, SDL_GPURenderPass* pass,
                               const std::vector<uint32_t>& order, uint32_t base, const Mat4& viewProjection,
                               const float jitter[2])
{
    BindVertexStorage(pass);
    SDL_GPUBuffer* boundDeltas = m_NoMorphDeltas;

    size_t runStart = 0;
    while (runStart < order.size())
    {
        const PendingInstance& first = m_Instances[order[runStart]];

        // Only geometry matters here, so runs are as long as the geometry is
        // shared, whatever the materials and textures.
        size_t runEnd = runStart + 1;
        while (runEnd < order.size())
        {
            const PendingInstance& next = m_Instances[order[runEnd]];
            if (next.Mesh != first.Mesh || next.FirstIndex != first.FirstIndex || next.IndexCount != first.IndexCount)
            {
                break;
            }
            ++runEnd;
        }

        BindMorphDeltas(pass, *first.Mesh, boundDeltas);

        SDL_GPUBufferBinding vertexBinding{};
        vertexBinding.buffer = first.Mesh->Vertices;
        SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);

        SDL_GPUBufferBinding indexBinding{};
        indexBinding.buffer = first.Mesh->Indices;
        SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

        DrawUniforms uniforms{};
        CopyMatrix(uniforms.ViewProjection, viewProjection);
        CopyMatrix(uniforms.PreviousViewProjection, m_HasPreviousViewProjection ? m_PreviousViewProjection : viewProjection);
        uniforms.Jitter[0] = jitter[0];
        uniforms.Jitter[1] = jitter[1];
        uniforms.InstanceOffset = base + static_cast<uint32_t>(runStart);
        SDL_PushGPUVertexUniformData(commandBuffer, 0, &uniforms, sizeof(uniforms));

        SDL_DrawGPUIndexedPrimitives(pass, first.IndexCount, static_cast<uint32_t>(runEnd - runStart),
                                     first.FirstIndex, 0, 0);
        ++m_DrawCalls;

        runStart = runEnd;
    }
}

void Renderer3D::DrawShadowMap(SDL_GPUCommandBuffer* commandBuffer)
{
    SDL_GPUDepthStencilTargetInfo depth{};
    depth.texture = m_ShadowMap;
    depth.clear_depth = 1.0f;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_STORE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commandBuffer, nullptr, 0, &depth);
    if (pass == nullptr)
    {
        return;
    }

    SDL_BindGPUGraphicsPipeline(pass, m_ShadowPipeline);

    // Each cascade draws into its own square of the shared map, two a row.
    const int perRow = m_Cascades > 1 ? 2 : 1;
    const float noJitter[2] = { 0.0f, 0.0f };
    for (int cascade = 0; cascade < m_Cascades; ++cascade)
    {
        const int x = (cascade % perRow) * m_CascadeResolution;
        const int y = (cascade / perRow) * m_CascadeResolution;

        SDL_GPUViewport viewport{};
        viewport.x = static_cast<float>(x);
        viewport.y = static_cast<float>(y);
        viewport.w = static_cast<float>(m_CascadeResolution);
        viewport.h = static_cast<float>(m_CascadeResolution);
        viewport.min_depth = 0.0f;
        viewport.max_depth = 1.0f;
        SDL_SetGPUViewport(pass, &viewport);

        const SDL_Rect scissor{ x, y, m_CascadeResolution, m_CascadeResolution };
        SDL_SetGPUScissor(pass, &scissor);

        DrawDepthRuns(commandBuffer, pass, m_ShadowOrder, m_ShadowBase, m_CascadeMatrices[cascade], noJitter);
    }

    SDL_EndGPURenderPass(pass);
}

void Renderer3D::DrawMotionPrepass(SDL_GPUCommandBuffer* commandBuffer, const Mat4& viewProjection,
                                   const float jitter[2])
{
    SDL_GPUColorTargetInfo color[2]{};
    color[0].texture = m_Motion;
    color[0].clear_color = SDL_FColor{ 0.0f, 0.0f, 0.0f, 0.0f };
    color[0].load_op = SDL_GPU_LOADOP_CLEAR;
    color[0].store_op = SDL_GPU_STOREOP_STORE;
    color[1] = color[0];
    color[1].texture = m_Normals;
    color[1].clear_color = SDL_FColor{ 0.0f, 1.0f, 0.0f, 1.0f };

    SDL_GPUDepthStencilTargetInfo depth{};
    depth.texture = m_PrepassDepth;
    depth.clear_depth = 1.0f;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_STORE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commandBuffer, color, 2, &depth);
    if (pass == nullptr)
    {
        return;
    }

    SDL_BindGPUGraphicsPipeline(pass, m_MotionPipeline);
    DrawDepthRuns(commandBuffer, pass, m_OpaqueOrder, m_OpaqueBase, viewProjection, jitter);

    SDL_EndGPURenderPass(pass);
}

void Renderer3D::DrawDebugLines(SDL_GPUCommandBuffer* commandBuffer, SDL_GPURenderPass* pass,
                                const Mat4& viewProjection, const float jitter[2])
{
    if (m_LineVertices.empty() || m_LineBuffer == nullptr)
    {
        return;
    }

    SDL_BindGPUGraphicsPipeline(pass, m_LinePipeline);

    SDL_GPUBufferBinding binding{};
    binding.buffer = m_LineBuffer;
    SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);

    DrawUniforms uniforms{};
    CopyMatrix(uniforms.ViewProjection, viewProjection);
    CopyMatrix(uniforms.PreviousViewProjection, viewProjection);
    uniforms.Jitter[0] = jitter[0];
    uniforms.Jitter[1] = jitter[1];
    SDL_PushGPUVertexUniformData(commandBuffer, 0, &uniforms, sizeof(uniforms));

    SDL_DrawGPUPrimitives(pass, static_cast<uint32_t>(m_LineVertices.size() / 7), 1, 0, 0);
    ++m_DrawCalls;
}

void Renderer3D::SetScreenLighting(SDL_GPUCommandBuffer* commandBuffer, bool enabled)
{
    if (m_SceneUniforms.size() < sizeof(SceneUniforms))
    {
        return;
    }

    float screen[4];
    std::memcpy(screen, m_SceneUniforms.data() + offsetof(SceneUniforms, ScreenParams), sizeof(screen));
    screen[2] = enabled && m_ScreenLightingThisFrame ? 1.0f : 0.0f;
    screen[3] = enabled && m_ScreenSunThisFrame ? 1.0f : 0.0f;
    std::memcpy(m_SceneUniforms.data() + offsetof(SceneUniforms, ScreenParams), screen, sizeof(screen));

    SDL_PushGPUFragmentUniformData(commandBuffer, 0, m_SceneUniforms.data(),
                                   static_cast<uint32_t>(m_SceneUniforms.size()));
}

void Renderer3D::DrawSky(SDL_GPUCommandBuffer* commandBuffer, SDL_GPURenderPass* pass, const FrameCamera& camera,
                         const RenderSettings& settings)
{
    EffectUniforms uniforms{};
    CopyMatrix(uniforms.InverseViewProjection, InvertMatrix(camera.Projection * camera.View));

    const Vec3 direction = NormalizeVector(settings.Light.Direction);
    const Color light = ToLinear(settings.Light.Tint);
    const Color zenith = ToLinear(settings.Sky.Zenith);
    const Color horizon = ToLinear(settings.Sky.Horizon);
    const Color ground = ToLinear(settings.Sky.Ground);
    const float sunRadius = std::max(settings.Sky.SunSize, 0.0f) * SunDiscDegrees * 0.5f * Pi / 180.0f;
    const float intensity = settings.Light.Intensity;

    const float extra[5][4] = {
        { direction.X, direction.Y, direction.Z, 0.0f },
        { light.R * intensity, light.G * intensity, light.B * intensity, 0.0f },
        { zenith.R, zenith.G, zenith.B, std::max(settings.Sky.Brightness, 0.0f) },
        { horizon.R, horizon.G, horizon.B, std::cos(sunRadius) },
        { ground.R, ground.G, ground.B, settings.Sky.SunSize > 0.0f ? SunDiscBrightness : 0.0f },
    };
    std::memcpy(uniforms.Extra, extra, sizeof(extra));

    SDL_BindGPUGraphicsPipeline(pass, m_SkyPipeline);
    SDL_PushGPUFragmentUniformData(commandBuffer, 0, &uniforms, sizeof(uniforms));
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    ++m_DrawCalls;
}

void Renderer3D::DrawScene(SDL_GPUCommandBuffer* commandBuffer, const RenderSettings& settings,
                           const RenderExtras& extras, const Mat4& viewProjection, const float jitter[2],
                           const FrameCamera& camera, bool flat)
{
    const Color sky = ToLinear(settings.SkyColor);

    SDL_GPUColorTargetInfo color{};
    color.clear_color = SDL_FColor{ sky.R, sky.G, sky.B, 1.0f };
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    if (m_SceneMsaa != nullptr)
    {
        // Resolving as part of the store is cheaper than a separate pass, and
        // it leaves the single-sample texture ready for the passes after.
        color.texture = m_SceneMsaa;
        color.store_op = SDL_GPU_STOREOP_RESOLVE;
        color.resolve_texture = m_Scene;
    }
    else
    {
        color.texture = m_Scene;
        color.store_op = SDL_GPU_STOREOP_STORE;
    }

    SDL_GPUDepthStencilTargetInfo depth{};
    depth.texture = m_Depth;
    depth.clear_depth = 1.0f;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.cycle = true;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commandBuffer, &color, 1, &depth);
    if (pass == nullptr)
    {
        return;
    }

    if (extras.Background.IsValid())
    {
        DrawBackdrop(commandBuffer, pass, extras.Background);
    }
    else if (settings.Sky.Enabled && !flat)
    {
        DrawSky(commandBuffer, pass, camera, settings);
    }

    SetScreenLighting(commandBuffer, false);
    DrawRuns(commandBuffer, pass, m_OverlayBehind, m_BehindBase, SurfaceVariant::Overlay, viewProjection, jitter);
    SetScreenLighting(commandBuffer, true);
    DrawRuns(commandBuffer, pass, m_OpaqueOrder, m_OpaqueBase, SurfaceVariant::Opaque, viewProjection, jitter);
    SetScreenLighting(commandBuffer, false);
    DrawRuns(commandBuffer, pass, m_TransparentOrder, m_TransparentBase, SurfaceVariant::Transparent, viewProjection,
             jitter);
    DrawRuns(commandBuffer, pass, m_OverlayFront, m_FrontBase, SurfaceVariant::Overlay, viewProjection, jitter);
    DrawDebugLines(commandBuffer, pass, viewProjection, jitter);

    SDL_EndGPURenderPass(pass);
}

void Renderer3D::RenderFrame(const FrameCamera& camera, const RenderSettings& settings, RenderExtras& extras,
                             bool flat)
{
    // Debug lines become vertices: position then linear colour.
    for (const DebugLine& line : extras.DebugLines)
    {
        const Color linear = ToLinear(line.Tint);
        for (const Vec3& point : { line.From, line.To })
        {
            m_LineVertices.insert(m_LineVertices.end(),
                                  { point.X, point.Y, point.Z, linear.R, linear.G, linear.B, linear.A });
        }
    }
    extras.DebugLines.clear();

    LUDIFEX_PROFILE("render frame");

    // How long since the last frame, for effects that ease over time. Capped,
    // so a stall does not make the exposure jump.
    const uint64_t now = SDL_GetTicksNS();
    m_FrameSeconds = m_LastFrameTicks != 0
                         ? std::min(static_cast<float>(static_cast<double>(now - m_LastFrameTicks) * 1e-9), 0.25f)
                         : 0.0f;
    m_LastFrameTicks = now;

    // Which lights reach which cell, before anything is uploaded or drawn.
    const float aspect =
        static_cast<float>(std::max(1, m_Width)) / static_cast<float>(std::max(1, m_Height));
    BuildClusters(camera, aspect);

    const Mat4 viewProjection = camera.Projection * camera.View;

    // What this frame traces. A flat world has nothing to trace, and a device
    // without the compute storage tracing needs says so once and draws the
    // rasterized effects instead.
    const RayTracingSettings& rays = settings.RayTracing;
    const bool wantsRays = !flat && (settings.PathTracing.Enabled || rays.Shadows || rays.AmbientOcclusion ||
                                     rays.Reflections);
    if (wantsRays && !m_RaysSupported && !m_RaysUnsupportedReported)
    {
        LogMessage(LogLevel::Warning, "render",
                   "Ray tracing and path tracing need compute shaders writing half- and full-float textures, which "
                   "this device does not offer; the rasterized effects stand in.");
        m_RaysUnsupportedReported = true;
    }
    const bool pathTracing = wantsRays && m_RaysSupported && settings.PathTracing.Enabled;
    const bool rayEffects = wantsRays && m_RaysSupported && !pathTracing;
    const bool tracedShadows = rayEffects && rays.Shadows;
    const bool occlusion = !flat && !pathTracing && settings.AmbientOcclusion.Enabled &&
                           !(rayEffects && rays.AmbientOcclusion);

    // A different sub-pixel offset each frame is what lets TAA gather many
    // samples per pixel over time. Eight positions, then the cycle repeats.
    // The path tracer jitters its own rays, and does not need it.
    float jitter[2] = { 0.0f, 0.0f };
    const bool taa = UsesTaa(settings.Mode) && !pathTracing;
    if (taa)
    {
        const uint32_t sample = (m_FrameIndex % 8) + 1;
        jitter[0] = (Halton(sample, 2) - 0.5f) * 2.0f / static_cast<float>(m_Width);
        jitter[1] = (Halton(sample, 3) - 0.5f) * 2.0f / static_cast<float>(m_Height);
    }

    bool anyPost = false;
    bool anyLinearPost = false;
    size_t displayPasses = 1 + (UsesFxaa(settings.Mode) ? 1 : 0);
    for (const PostProcessEntry& entry : extras.PostProcess)
    {
        if (GetWorldMaterials().Resolve(entry.Material) == nullptr)
        {
            continue;
        }
        anyPost = true;
        if (entry.Point == PassPoint::BeforeToneMap)
        {
            anyLinearPost = true;
        }
        else
        {
            ++displayPasses;
        }
    }

    const bool scaled = m_Width != m_OutputWidth || m_Height != m_OutputHeight;
    const bool prepass = anyPost || (!pathTracing && (taa || occlusion || rayEffects));
    if (!EnsurePostTargets(anyLinearPost, displayPasses > 1 || scaled, taa, prepass))
    {
        return;
    }

    // Traced shadows replace the maps, except for translucent surfaces, which
    // the prepass never saw and so the tracer cannot shade. A path-traced
    // frame needs no maps at all.
    if (m_ShadowsThisFrame && (pathTracing || (tracedShadows && m_TransparentOrder.empty())))
    {
        m_ShadowsThisFrame = false;
    }
    const int perRow = m_Cascades > 1 ? 2 : 1;
    if (m_ShadowsThisFrame && !EnsureShadowMap(m_CascadeResolution * perRow))
    {
        m_ShadowsThisFrame = false;
    }

    // The scene block every surface shader reads.
    SceneUniforms scene{};
    const Vec3 lightDirection = NormalizeVector(settings.Light.Direction);
    const Color lightColor = ToLinear(settings.Light.Tint);
    const Color ambient = ToLinear(settings.AmbientColor);
    const Color fog = ToLinear(settings.Fog.Tint);
    const bool sky = settings.Sky.Enabled && !flat;

    scene.LightDirection[0] = lightDirection.X;
    scene.LightDirection[1] = lightDirection.Y;
    scene.LightDirection[2] = lightDirection.Z;
    scene.LightColor[0] = lightColor.R;
    scene.LightColor[1] = lightColor.G;
    scene.LightColor[2] = lightColor.B;
    scene.LightColor[3] = settings.Light.Intensity;
    scene.AmbientColor[0] = ambient.R;
    scene.AmbientColor[1] = ambient.G;
    scene.AmbientColor[2] = ambient.B;
    scene.AmbientColor[3] = sky ? 1.0f : 0.0f;
    scene.CameraPosition[0] = camera.Position.X;
    scene.CameraPosition[1] = camera.Position.Y;
    scene.CameraPosition[2] = camera.Position.Z;

    // Which way the camera looks, so a pixel can find its own depth and with
    // it the cell whose lights it should shade against, and its cascade.
    const Vec3 forward = NormalizeVector(camera.Forward);
    scene.CameraForward[0] = forward.X;
    scene.CameraForward[1] = forward.Y;
    scene.CameraForward[2] = forward.Z;
    scene.CameraForward[3] = camera.NearPlane;

    if (m_ShadowsThisFrame)
    {
        const float distance = std::min(settings.Shadows.Distance, camera.FarPlane);
        for (int cascade = 0; cascade < m_Cascades; ++cascade)
        {
            CopyMatrix(scene.ShadowViewProjection[cascade], m_CascadeMatrices[static_cast<size_t>(cascade)]);
            scene.ShadowSplits[cascade] = m_CascadeEnds[static_cast<size_t>(cascade)];
            scene.ShadowOffsets[cascade] = m_CascadeOffsets[static_cast<size_t>(cascade)];
        }
        scene.ShadowParams[0] = 1.0f / static_cast<float>(m_ShadowResolution);
        scene.ShadowParams[1] = std::max(0.0f, settings.Shadows.Softness);
        scene.ShadowParams[2] = static_cast<float>(m_Cascades);
        scene.ShadowParams[3] = static_cast<float>(perRow);
        scene.ShadowFade[0] = distance * 0.85f;
        scene.ShadowFade[1] = distance * 0.15f;
    }

    scene.FogColor[0] = fog.R;
    scene.FogColor[1] = fog.G;
    scene.FogColor[2] = fog.B;
    scene.FogColor[3] = settings.Fog.Enabled ? 1.0f : 0.0f;
    scene.FogParams[0] = settings.Fog.Start;
    scene.FogParams[1] = settings.Fog.End;
    scene.ClusterParams[0] = m_ClusterScale;
    scene.ClusterParams[1] = m_ClusterBias;
    scene.ClusterParams[2] = static_cast<float>(ClusterColumns);
    scene.ClusterParams[3] = static_cast<float>(ClusterRows);
    scene.ClusterViewport[0] = static_cast<float>(std::max(1, m_Width));
    scene.ClusterViewport[1] = static_cast<float>(std::max(1, m_Height));

    scene.Counts[0] = static_cast<uint32_t>(m_Lights.size());
    scene.Counts[1] = ClusterSlices;

    m_ScreenLightingThisFrame = occlusion || rayEffects;
    m_ScreenSunThisFrame = tracedShadows;
    scene.ScreenParams[0] = 1.0f / static_cast<float>(std::max(1, m_Width));
    scene.ScreenParams[1] = 1.0f / static_cast<float>(std::max(1, m_Height));
    scene.ScreenParams[2] = m_ScreenLightingThisFrame ? 1.0f : 0.0f;
    scene.ScreenParams[3] = m_ScreenSunThisFrame ? 1.0f : 0.0f;

    const Color zenith = ToLinear(settings.Sky.Zenith);
    const Color horizon = ToLinear(settings.Sky.Horizon);
    const Color ground = ToLinear(settings.Sky.Ground);
    const float sunRadius = std::max(settings.Sky.SunSize, 0.0f) * SunDiscDegrees * 0.5f * Pi / 180.0f;
    const float skyZenith[4] = { zenith.R, zenith.G, zenith.B, std::max(settings.Sky.Brightness, 0.0f) };
    const float skyHorizon[4] = { horizon.R, horizon.G, horizon.B, std::cos(sunRadius) };
    const float skyGround[4] = { ground.R, ground.G, ground.B, settings.Sky.SunSize > 0.0f ? SunDiscBrightness : 0.0f };
    std::memcpy(scene.SkyZenith, skyZenith, sizeof(skyZenith));
    std::memcpy(scene.SkyHorizon, skyHorizon, sizeof(skyHorizon));
    std::memcpy(scene.SkyGround, skyGround, sizeof(skyGround));

    m_SceneUniforms.resize(sizeof(scene));
    std::memcpy(m_SceneUniforms.data(), &scene, sizeof(scene));

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(m_Device);
    if (commandBuffer == nullptr)
    {
        return;
    }

    if (!UploadFrameData(commandBuffer))
    {
        SDL_SubmitGPUCommandBuffer(commandBuffer);
        return;
    }

    if (m_ShadowsThisFrame)
    {
        DrawShadowMap(commandBuffer);
    }

    if (prepass)
    {
        DrawMotionPrepass(commandBuffer, viewProjection, jitter);
    }

    // The screen-space lighting surfaces read: nothing, until an effect below
    // fills it in.
    m_ScreenOcclusion = m_White;
    m_ScreenSunlight = m_White;
    m_ScreenReflections = m_Clear;

    if (occlusion)
    {
        RunOcclusion(commandBuffer, camera, settings);
        m_ScreenOcclusion = m_Occlusion[0];
    }

    m_RaysActive = false;
    if (pathTracing || rayEffects)
    {
        m_RaysActive = TraceRays(commandBuffer, camera, settings, extras, pathTracing);
    }
    if (!pathTracing)
    {
        m_PathSamples = 0;
        m_PathFingerprint = 0;
    }

    // A path-traced frame is the traced image; anything else is rasterized,
    // including a path-traced frame the device failed to trace.
    SDL_GPUTexture* image = m_Scene;
    if (pathTracing && m_RaysActive)
    {
        image = m_PathImage;
    }
    else
    {
        DrawScene(commandBuffer, settings, extras, viewProjection, jitter, camera, flat);
    }

    RunPostChain(commandBuffer, camera, settings, extras, image, taa);

    SDL_SubmitGPUCommandBuffer(commandBuffer);

    m_PreviousViewProjection = viewProjection;
    m_HasPreviousViewProjection = true;
    ++m_FrameIndex;
}

} // namespace ludifex::detail
