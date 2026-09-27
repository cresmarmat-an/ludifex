// The world renderer.
// Not installed and not part of the public API.
//
// The world draws into its own textures on its own command buffer instead of
// into someone else's render pass. So ludifex can render without knowing
// whether an interface library exists, and sharing a device with opane needs
// only a pointer.
//
// A frame is a fixed sequence of passes:
//
//   shadow maps     depth from the sun, one map per cascade, each fitted
//                   around a slice of what the camera sees
//   prepass         depth, screen motion, and normals, when TAA, ambient
//                   occlusion, ray tracing, or a post-process material needs
//                   them
//   occlusion       screen-space ambient occlusion, blurred along the surfaces
//   rays            traced sun shadows, occlusion, and reflections, smoothed
//                   over time and space (RenderRays.cpp)
//   scene           linear-light HDR, multisampled: sky or backdrop, opaque
//                   objects sorted by material, texture, and mesh, then
//                   translucent objects sorted back to front, then debug
//                   lines; with path tracing, the whole image traced instead
//   before tone map TAA, then the user's BeforeToneMap materials
//   bloom, exposure the glow of the brightest light, and the eye's adaptation
//   tone map        into display-encoded 8-bit colour, graded
//   after tone map  FXAA, then the user's AfterToneMap materials
//   scale           to the target's size, when the render scale is not 1
//
// The scene is drawn at the render scale; the last pass always writes the same
// output texture at the target's size, so the pointer a host wrapped once
// stays valid however many passes run.

#pragma once

#include "Internal.h"
#include "Models.h"
#include "WorldMaterials.h"
#include "WorldTextures.h"

#include <SDL3/SDL.h>

#include <array>
#include <deque>
#include <utility>
#include <memory>
#include <vector>

namespace ludifex::detail
{

// The one GPU device this process uses, whether adopted from another library
// or created here on first use.
extern SDL_GPUDevice* g_Device;

SDL_GPUDevice* EnsureDevice();

// The shader format of the device in use or, before there is one, of the
// device most likely to be made, without making it.
SDL_GPUShaderFormat ExpectedShaderFormat();
void ReleaseDeviceIfOwned();

// Renderers register themselves so a host about to destroy the device can have
// every one of them let go of its GPU objects first.
class Renderer3D;
void RegisterRenderer(Renderer3D* renderer);
void UnregisterRenderer(Renderer3D* renderer);

// Mirrors VertexInput in world.hlsl, and matches ModelVertex so a loaded model
// uploads without conversion.
struct MeshVertex
{
    float Position[3];
    float Normal[3];
    float UV[2];

    // Zero for every shape the world builds itself; the shader reads that as
    // "does not bend". See ModelVertex for why they are here at all.
    uint8_t Joints[4];
    uint8_t Weights[4];
};

static_assert(sizeof(MeshVertex) == sizeof(ModelVertex), "model and mesh vertices must share a layout");

struct MeshBuffers
{
    SDL_GPUBuffer* Vertices = nullptr;
    SDL_GPUBuffer* Indices = nullptr;
    uint32_t IndexCount = 0;

    // A model's morph displacements, read by the vertex stage; null for a
    // mesh without morph targets, which is almost every mesh.
    SDL_GPUBuffer* MorphDeltas = nullptr;

    // A number no other upload shares, so anything kept per mesh (such as the
    // ray tracer's tree over it) can tell a mesh rebuilt in place from the one
    // it replaced.
    uint64_t Serial = 0;

    // The geometry as it was uploaded, which the ray tracer builds from.
    // Borrowed: the renderer keeps the primitive shapes', and the model store
    // its models'.
    const ModelVertex* CpuVertices = nullptr;
    uint32_t CpuVertexCount = 0;
    const uint32_t* CpuIndices = nullptr;
    uint32_t CpuIndexCount = 0;
};

// --- helpers shared by the renderer's source files (RenderMeshes.cpp) --------

void BuildBoxMesh(std::vector<MeshVertex>& vertices, std::vector<uint32_t>& indices);
void BuildSphereMesh(std::vector<MeshVertex>& vertices, std::vector<uint32_t>& indices);
void BuildCapsuleMesh(std::vector<MeshVertex>& vertices, std::vector<uint32_t>& indices, float stretch);

// The same shapes at a chosen density: 0 is full, 2 is the simplest.
void BuildSphereMeshAt(std::vector<MeshVertex>& vertices, std::vector<uint32_t>& indices, int detail);
void BuildCapsuleMeshAt(std::vector<MeshVertex>& vertices, std::vector<uint32_t>& indices,
                        float stretch, int detail);
void BuildQuadMesh(std::vector<MeshVertex>& vertices, std::vector<uint32_t>& indices);

// A storage buffer the shaders read, filled once. Null on failure.
SDL_GPUBuffer* UploadStorage(SDL_GPUDevice* device, const void* data, size_t bytes);

bool UploadMesh(SDL_GPUDevice* device, const void* vertices, size_t vertexCount,
                const std::vector<uint32_t>& indices, MeshBuffers& outMesh);
void ReleaseMesh(SDL_GPUDevice* device, MeshBuffers& mesh);

// The shader in whichever format the device takes: DXIL, SPIR-V, or MSL.
SDL_GPUShader* CreateShader(SDL_GPUDevice* device, SDL_GPUShaderStage stage, const char* entryPoint,
                            const ShaderBytecode& bytecode, uint32_t samplers, uint32_t storageBuffers,
                            uint32_t uniformBuffers);

// A compute pipeline, eight by eight threads a group, in whichever format the
// device takes.
SDL_GPUComputePipeline* CreateComputePipeline(SDL_GPUDevice* device, const char* entryPoint,
                                              const ShaderBytecode& bytecode, uint32_t samplers,
                                              uint32_t readOnlyBuffers, uint32_t writtenTextures,
                                              uint32_t uniformBuffers);

// levels above 1 make a mip chain, for a texture whose smaller levels are
// generated from its largest.
SDL_GPUTexture* CreateTarget(SDL_GPUDevice* device, SDL_GPUTextureFormat format, int width, int height,
                             SDL_GPUSampleCount samples, SDL_GPUTextureUsageFlags usage, uint32_t levels = 1);

// The inverse of a matrix that has one.
Mat4 InvertMatrix(const Mat4& matrix);

// --- the frame ----------------------------------------------------------------

// Everything about the viewpoint a frame needs, filled in by the world.
struct FrameCamera
{
    Mat4 View;
    Mat4 Projection; // unjittered

    Vec3 Position;
    Vec3 Forward{ 0.0f, 0.0f, -1.0f };
    Vec3 Up{ 0.0f, 1.0f, 0.0f };

    float NearPlane = 0.1f;
    float FarPlane = 500.0f;
    float FieldOfViewRadians = 1.0f;
    bool Orthographic = false;
};

// Mirrors EffectUniforms in effects.hlsl: one block for every image effect,
// each giving the fields its own meaning.
struct EffectUniforms
{
    float ViewProjection[16];
    float InverseViewProjection[16];
    float TexelSize[4];
    float Params[4];
    float Extra[6][4];
};

// Mirrors PointLight in world_material.hlsli.
struct GpuPointLight
{
    float PositionRange[4];
    float ColorIntensity[4];
};

// Clustered forward lighting: the view is cut into a grid of cells (a tile
// across the screen, a slice in depth), and each cell keeps the lights that
// reach it. A pixel then shades against the few lights in its own cell
// instead of every light in the scene, so many lights stay affordable with
// multisampling on.
constexpr uint32_t ClusterColumns = 16;
constexpr uint32_t ClusterRows = 9;
constexpr uint32_t ClusterSlices = 24;
constexpr uint32_t ClusterCount = ClusterColumns * ClusterRows * ClusterSlices;

// More lights than this in one cell is a scene that wants rethinking rather
// than a renderer that wants a bigger number.
constexpr size_t MaxLightsPerCluster = 32;

// One object to draw: a range of a mesh and everything its instance record
// holds. Several per actor when a model has several parts.
struct PendingInstance
{
    const MeshBuffers* Mesh = nullptr;
    uint32_t FirstIndex = 0;
    uint32_t IndexCount = 0;

    Mat4 Model;
    Mat4 PreviousModel;

    Color Tint;       // linear
    float Roughness = 0.6f;
    float Metallic = 0.0f;
    Vec2 UVScale{ 1.0f, 1.0f };
    Vec2 UVOffset{ 0.0f, 0.0f };
    uint32_t Flags = 0;

    TextureId Texture;
    bool ClampSampler = false;

    // The rest of the surface's maps. Invalid means "none", and the shader
    // then samples a texel that changes nothing.
    TextureId NormalMap;
    TextureId MetallicRoughnessMap;
    TextureId EmissiveMap;
    TextureId OcclusionMap;
    float NormalStrength = 0.0f;
    float OcclusionStrength = 1.0f;
    Vec3 Emission{ 0.0f, 0.0f, 0.0f };   // linear

    // -1 draws with the built-in shader.
    int MaterialIndex = -1;

    // For the ray tracer, which poses skinned and morphing models on the CPU:
    // the model this came from, and which actor it is, whose tree is refitted
    // every frame. Null and zero for everything else.
    const ModelMesh* SourceModel = nullptr;
    uint64_t ActorKey = 0;

    // Per-actor uniform values, copied into the parameter buffer at upload.
    uint32_t ParamMask = 0;
    const float (*Params)[4] = nullptr;

    // This actor's joint matrices, or null for anything that does not bend.
    // Borrowed rather than copied: it points into the actor's own pose, which
    // outlives the frame that reads it.
    const std::vector<Mat4>* Palette = nullptr;

    // This actor's morph weights and its model's targets, borrowed the same
    // way; null for anything without morph targets. Weights may be null with
    // targets present, and the targets' own defaults are drawn then.
    const std::vector<float>* MorphWeights = nullptr;
    const std::vector<MorphTarget>* MorphTargets = nullptr;

    // Back-to-front order for translucent objects; layer and insertion order
    // for 2D overlays.
    float Depth = 0.0f;
    int Layer = 0;
    uint64_t Sequence = 0;

    // Filled in at upload.
    uint32_t ParamOffset = 0;

    // Where this actor's joints start in the frame's shared palette, filled in
    // at upload rather than by the caller.
    uint32_t PaletteOffset = 0;

    // Where this actor's active morph targets start in the frame's table, and
    // how many there are. Filled in at upload.
    uint32_t MorphOffset = 0;
    uint32_t MorphCount = 0;
};

// How a surface pipeline treats depth and alpha.
enum class SurfaceVariant
{
    Opaque,       // depth tested and written; alpha becomes coverage for materials
    Transparent,  // depth tested, not written, blended
    Overlay,      // not depth tested, blended: 2D sprites, in layer order
    Count
};

class RayScene;

class Renderer3D
{
public:
    bool Initialize(SDL_GPUDevice* device);
    void Shutdown();

    // Recreates the targets when the size, the render scale, or the
    // anti-aliasing changes. The size is the output's; the scene is drawn at
    // it times the render scale.
    bool SetSize(int width, int height, const RenderSettings& settings);

    void Render(World3DState& state);

    // A 2D world draws through the same pipeline with an orthographic camera,
    // so there is one renderer rather than two that drift apart.
    void Render2D(World2DState& state);

    // The final, display-encoded image.
    SDL_GPUTexture* GetResolvedTexture() const { return m_Output; }

    uint32_t GetDrawCallCount() const { return m_DrawCalls; }
    uint32_t GetInstanceCount() const { return m_InstanceCount; }
    uint32_t GetCulledCount() const { return m_CulledCount; }
    uint32_t GetLargestBatch() const { return m_LargestBatch; }

    bool IsRayTracingActive() const { return m_RaysActive; }
    uint32_t GetPathTracedSamples() const { return m_PathSamples; }

private:
    // --- setup ------------------------------------------------------------

    bool CreateShaders();
    bool BuildPipelines();
    void ReleasePipelines();
    void ReleaseTargets();
    void ReleaseSceneTargets();
    bool EnsurePostTargets(bool needHdrWork, bool needLdrWork, bool needHistory, bool needMotion);
    bool EnsureShadowMap(int resolution);

    SDL_GPUGraphicsPipeline* CreateSurfacePipeline(SDL_GPUShader* fragmentShader, SurfaceVariant variant,
                                                   bool alphaToCoverage) const;
    // withDepth: drawn inside the scene pass, which has a depth attachment the
    // pipeline must declare even though it neither tests nor writes it.
    // additive: adds to what the target holds instead of replacing it.
    SDL_GPUGraphicsPipeline* CreateFullscreenPipeline(SDL_GPUShader* fragmentShader, SDL_GPUTextureFormat format,
                                                      SDL_GPUSampleCount samples, bool withDepth = false,
                                                      bool additive = false) const;

    // Material pipelines build on first use and rebuild when the material is
    // reloaded or the sample count changes. nullptr means "draw with the
    // default", which is what a broken material falls back to.
    SDL_GPUGraphicsPipeline* SurfacePipelineFor(int materialIndex, SurfaceVariant variant);
    SDL_GPUGraphicsPipeline* PostPipelineFor(int materialIndex, bool linearLight);
    void ReleaseMaterialPipelines();

    // --- building a frame ---------------------------------------------------

    void ResetFrame();
    MeshBuffers* MeshForShape(ShapeKind shape);

    // A capsule mesh for one proportion of cylinder to radius, built on first
    // use. Proportions are rounded to a sixty-fourth, so a scene of capsules
    // of a few sizes shares a few meshes.
    MeshBuffers* MeshForCapsule(float stretch, int detail = 0);

    // How simple a mesh this object is worth, from how much of the view it
    // covers: 0 while it is worth its full shape, 2 once it is a speck.
    int DetailFor(const Vec3& position, float radius, const RenderSettings& settings) const;
    MeshBuffers* MeshForModel(ModelStore& models, int modelIndex);

    // The model matrix an actor slot was drawn with last frame, for motion
    // vectors. A new actor in a reused slot reports no motion.
    Mat4 PreviousModelFor(uint32_t index, uint32_t generation, const Mat4& current);

    void AddInstance(const PendingInstance& instance, SurfaceVariant variant, bool behind = false);
    void AddShadowCaster(const PendingInstance& instance);

    // Keeps the CPU copy of a primitive shape's geometry for the ray tracer.
    void KeepGeometry(MeshBuffers& mesh, std::vector<MeshVertex> vertices, std::vector<uint32_t> indices);

    // flat: a 2D world, which has no shadows, occlusion, sky, or rays.
    void RenderFrame(const FrameCamera& camera, const RenderSettings& settings, RenderExtras& extras, bool flat);

    // --- passes -------------------------------------------------------------

    bool UploadFrameData(SDL_GPUCommandBuffer* commandBuffer);
    void DrawShadowMap(SDL_GPUCommandBuffer* commandBuffer);
    void DrawMotionPrepass(SDL_GPUCommandBuffer* commandBuffer, const Mat4& viewProjection, const float jitter[2]);
    void DrawScene(SDL_GPUCommandBuffer* commandBuffer, const RenderSettings& settings, const RenderExtras& extras,
                   const Mat4& viewProjection, const float jitter[2], const FrameCamera& camera, bool flat);
    // `base` is where this list starts in the instance buffer: each list is
    // uploaded contiguously in its own draw order.
    void DrawRuns(SDL_GPUCommandBuffer* commandBuffer, SDL_GPURenderPass* pass, const std::vector<uint32_t>& order,
                  uint32_t base, SurfaceVariant variant, const Mat4& viewProjection, const float jitter[2]);
    void DrawDepthRuns(SDL_GPUCommandBuffer* commandBuffer, SDL_GPURenderPass* pass,
                       const std::vector<uint32_t>& order, uint32_t base, const Mat4& viewProjection,
                       const float jitter[2]);
    void DrawDebugLines(SDL_GPUCommandBuffer* commandBuffer, SDL_GPURenderPass* pass, const Mat4& viewProjection,
                        const float jitter[2]);
    void DrawBackdrop(SDL_GPUCommandBuffer* commandBuffer, SDL_GPURenderPass* pass, TextureId background);
    void DrawSky(SDL_GPUCommandBuffer* commandBuffer, SDL_GPURenderPass* pass, const FrameCamera& camera,
                 const RenderSettings& settings);

    // Whether surfaces read the frame's screen-space lighting: the opaque
    // ones do, and the translucent ones, which the prepass never saw, do not.
    void SetScreenLighting(SDL_GPUCommandBuffer* commandBuffer, bool enabled);

    // Binds everything the surface fragment stage declares, so a pipeline
    // change never leaves a slot empty.
    void BindSurfaceResources(SDL_GPUCommandBuffer* commandBuffer, SDL_GPURenderPass* pass,
                              const PendingInstance& textures, SDL_GPUSampler* baseSampler);

    // Runs one full-screen pass reading `sources` and writing `target`.
    // keep: draws over what the target holds rather than discarding it, for
    // passes that add to it.
    void FullscreenPass(SDL_GPUCommandBuffer* commandBuffer, SDL_GPUGraphicsPipeline* pipeline,
                        SDL_GPUTexture* target, const std::vector<SDL_GPUTexture*>& sources,
                        const std::vector<SDL_GPUSampler*>& samplers, const void* uniforms, size_t uniformSize,
                        const void* materialUniforms, size_t materialSize, bool keep = false);

    // source: the HDR image the chain starts from, either the rasterized scene
    // or the path-traced one.
    void RunPostChain(SDL_GPUCommandBuffer* commandBuffer, const FrameCamera& camera,
                      const RenderSettings& settings, const RenderExtras& extras, SDL_GPUTexture* source,
                      bool temporal);

    // The light's view and projection fitted around the slice of the view
    // from nearDepth to farDepth, snapped to whole texels of a map of the given
    // resolution. outTexelWorld is one texel's size in the world.
    Mat4 FitShadow(const FrameCamera& camera, const Vec3& lightDirection, float nearDepth, float farDepth,
                   int resolution, float* outTexelWorld = nullptr);

    // Works out the cascades for this frame: their slices, their matrices,
    // and the map they share.
    void PlanShadows(const FrameCamera& camera, const RenderSettings& settings);

    // --- effects (RenderEffects.cpp) ----------------------------------------

    bool CreateEffectPipelines();
    void ReleaseEffectPipelines();
    void ReleaseEffectTargets();

    void RunOcclusion(SDL_GPUCommandBuffer* commandBuffer, const FrameCamera& camera,
                      const RenderSettings& settings);

    // The bloom chain from the HDR image; the result is m_Bloom[0].
    void RunBloom(SDL_GPUCommandBuffer* commandBuffer, SDL_GPUTexture* source, const RenderSettings& settings);

    // Measures the image and eases the exposure toward it; the result is
    // m_Exposure[m_ExposureIndex].
    void RunExposure(SDL_GPUCommandBuffer* commandBuffer, SDL_GPUTexture* source, const RenderSettings& settings);

    // --- rays (RenderRays.cpp) ------------------------------------------------

    bool CreateRayPipelines();
    void ReleaseRayPipelines();
    void ReleaseRayTargets();

    // Uploads the ray scene, and traces this frame's effects or path. False
    // when nothing was traced.
    bool TraceRays(SDL_GPUCommandBuffer* commandBuffer, const FrameCamera& camera, const RenderSettings& settings,
                   const RenderExtras& extras, bool pathTracing);
    bool EnsureRayTargets(int width, int height);
    bool EnsurePathTargets();
    void Dispatch(SDL_GPUCommandBuffer* commandBuffer, SDL_GPUComputePipeline* pipeline,
                  const std::vector<SDL_GPUTexture*>& sources, const std::vector<SDL_GPUSampler*>& samplers,
                  const std::vector<SDL_GPUBuffer*>& buffers, const std::vector<SDL_GPUTexture*>& outputs,
                  const void* uniforms, size_t uniformSize, int width, int height);

    // --- state --------------------------------------------------------------

    SDL_GPUDevice* m_Device = nullptr;

    // Shaders.
    SDL_GPUShader* m_SurfaceVertex = nullptr;
    SDL_GPUShader* m_SurfaceFragment = nullptr;
    SDL_GPUShader* m_ShadowVertex = nullptr;
    SDL_GPUShader* m_ShadowFragment = nullptr;
    SDL_GPUShader* m_MotionVertex = nullptr;
    SDL_GPUShader* m_MotionFragment = nullptr;
    SDL_GPUShader* m_LineVertex = nullptr;
    SDL_GPUShader* m_LineFragment = nullptr;
    SDL_GPUShader* m_FullscreenVertex = nullptr;
    SDL_GPUShader* m_BackdropFragment = nullptr;
    SDL_GPUShader* m_ToneMapFragment = nullptr;
    SDL_GPUShader* m_FxaaFragment = nullptr;
    SDL_GPUShader* m_TaaFragment = nullptr;
    SDL_GPUShader* m_SkyFragment = nullptr;
    SDL_GPUShader* m_OcclusionFragment = nullptr;
    SDL_GPUShader* m_OcclusionBlurFragment = nullptr;
    SDL_GPUShader* m_BloomPrefilterFragment = nullptr;
    SDL_GPUShader* m_BloomDownFragment = nullptr;
    SDL_GPUShader* m_BloomUpFragment = nullptr;
    SDL_GPUShader* m_LuminanceFragment = nullptr;
    SDL_GPUShader* m_AdaptFragment = nullptr;
    SDL_GPUShader* m_UpscaleFragment = nullptr;

    // Pipelines baked for the current sample count.
    std::array<SDL_GPUGraphicsPipeline*, static_cast<size_t>(SurfaceVariant::Count)> m_DefaultSurface{};
    SDL_GPUGraphicsPipeline* m_ShadowPipeline = nullptr;
    SDL_GPUGraphicsPipeline* m_MotionPipeline = nullptr;
    SDL_GPUGraphicsPipeline* m_LinePipeline = nullptr;
    SDL_GPUGraphicsPipeline* m_BackdropPipeline = nullptr;
    SDL_GPUGraphicsPipeline* m_ToneMapPipeline = nullptr;
    SDL_GPUGraphicsPipeline* m_FxaaPipeline = nullptr;
    SDL_GPUGraphicsPipeline* m_TaaPipeline = nullptr;
    SDL_GPUGraphicsPipeline* m_SkyPipeline = nullptr;

    // Effects that do not depend on the sample count, built once.
    SDL_GPUGraphicsPipeline* m_OcclusionPipeline = nullptr;
    SDL_GPUGraphicsPipeline* m_OcclusionBlurPipeline = nullptr;
    SDL_GPUGraphicsPipeline* m_BloomPrefilterPipeline = nullptr;
    SDL_GPUGraphicsPipeline* m_BloomDownPipeline = nullptr;
    SDL_GPUGraphicsPipeline* m_BloomUpPipeline = nullptr;
    SDL_GPUGraphicsPipeline* m_LuminancePipeline = nullptr;
    SDL_GPUGraphicsPipeline* m_AdaptPipeline = nullptr;
    SDL_GPUGraphicsPipeline* m_UpscalePipeline = nullptr;

    // The tracing passes; null where the device cannot run them.
    SDL_GPUComputePipeline* m_RayHybridPipeline = nullptr;
    SDL_GPUComputePipeline* m_RayTemporalPipeline = nullptr;
    SDL_GPUComputePipeline* m_RayFilterPipeline = nullptr;
    SDL_GPUComputePipeline* m_PathTracePipeline = nullptr;
    bool m_RaysSupported = false;
    bool m_RaysUnsupportedReported = false;

    struct CachedPipeline
    {
        uint32_t Generation = 0;
        uint32_t Version = 0;
        SDL_GPUGraphicsPipeline* Pipeline = nullptr;
        bool Failed = false;
    };

    // Indexed by material slot, then by variant (surface) or by linear light
    // versus display-encoded (post-process).
    std::vector<std::array<CachedPipeline, static_cast<size_t>(SurfaceVariant::Count)>> m_MaterialSurface;
    std::vector<std::array<CachedPipeline, 2>> m_MaterialPost;

    SDL_GPUSampler* m_ShadowSampler = nullptr;
    SDL_GPUSampler* m_PointSampler = nullptr;
    SDL_GPUSampler* m_LinearClampSampler = nullptr;

    // Linear, and able to read any mip level: the exposure reads the smallest
    // level of the brightness it measured.
    SDL_GPUSampler* m_MipSampler = nullptr;

    // Targets. m_Width and m_Height are the scene's, at the render scale;
    // the output is the host's size.
    int m_Width = 0;
    int m_Height = 0;
    int m_OutputWidth = 0;
    int m_OutputHeight = 0;
    float m_RenderScale = 1.0f;
    SDL_GPUSampleCount m_SampleCount = SDL_GPU_SAMPLECOUNT_1;
    bool m_PipelinesBuilt = false;

    SDL_GPUTexture* m_SceneMsaa = nullptr;     // multisampled HDR colour, when multisampling
    SDL_GPUTexture* m_Scene = nullptr;         // resolved HDR colour
    SDL_GPUTexture* m_Depth = nullptr;         // matches the scene's sample count
    SDL_GPUTexture* m_Output = nullptr;        // the final 8-bit image
    std::array<SDL_GPUTexture*, 2> m_HdrWork{};
    std::array<SDL_GPUTexture*, 2> m_LdrWork{};
    std::array<SDL_GPUTexture*, 2> m_History{};
    SDL_GPUTexture* m_Motion = nullptr;
    SDL_GPUTexture* m_PrepassDepth = nullptr;  // single-sample and sampleable
    SDL_GPUTexture* m_Normals = nullptr;       // the prepass's normals and roughness
    int m_HistoryIndex = 0;
    bool m_HistoryValid = false;

    // The cascades share one map, two a row when there is more than one.
    SDL_GPUTexture* m_ShadowMap = nullptr;
    int m_ShadowResolution = 0;
    int m_Cascades = 0;
    int m_CascadeResolution = 0;
    std::array<Mat4, 4> m_CascadeMatrices{};
    std::array<float, 4> m_CascadeEnds{};
    std::array<float, 4> m_CascadeOffsets{};

    // Screen-space ambient occlusion, and its blur's other half.
    std::array<SDL_GPUTexture*, 2> m_Occlusion{};

    // The bloom chain, each level half the one before.
    std::vector<SDL_GPUTexture*> m_Bloom;
    std::vector<std::pair<int, int>> m_BloomSizes;

    // The measured brightness, and the exposure it eased to, this frame's and
    // last frame's.
    SDL_GPUTexture* m_Luminance = nullptr;
    uint32_t m_LuminanceLevels = 0;
    std::array<SDL_GPUTexture*, 2> m_Exposure{};
    int m_ExposureIndex = 0;
    bool m_ExposureValid = false;
    uint64_t m_LastFrameTicks = 0;
    float m_FrameSeconds = 0.0f;

    // A texel for each kind of nothing: full light, and no reflection.
    SDL_GPUTexture* m_White = nullptr;
    SDL_GPUTexture* m_Clear = nullptr;

    // What the surfaces read this frame in their screen-space slots.
    SDL_GPUTexture* m_ScreenOcclusion = nullptr;
    SDL_GPUTexture* m_ScreenSunlight = nullptr;
    SDL_GPUTexture* m_ScreenReflections = nullptr;
    bool m_ScreenLightingThisFrame = false;
    bool m_ScreenSunThisFrame = false;

    // Ray tracing: the scene, its raw and smoothed results at the tracing
    // resolution, and their histories.
    RayScene* m_Rays = nullptr;
    bool m_RaysActive = false;
    int m_RayWidth = 0;
    int m_RayHeight = 0;
    SDL_GPUTexture* m_RayLighting = nullptr;
    SDL_GPUTexture* m_RayReflection = nullptr;
    std::array<SDL_GPUTexture*, 2> m_RayHistory{};
    std::array<SDL_GPUTexture*, 2> m_RayHistoryReflection{};
    std::array<SDL_GPUTexture*, 2> m_RayFiltered{};
    std::array<SDL_GPUTexture*, 2> m_RayFilteredReflection{};
    int m_RayHistoryIndex = 0;
    bool m_RayHistoryValid = false;
    uint32_t m_RayEffects = 0;

    // Path tracing: the running sum, read and written in turn, the image, and
    // what the sum was gathered for.
    std::array<SDL_GPUTexture*, 2> m_PathSum{};
    SDL_GPUTexture* m_PathImage = nullptr;
    int m_PathSumIndex = 0;
    uint32_t m_PathSamples = 0;
    uint64_t m_PathFingerprint = 0;

    // Bound in the shadow slot when shadows are off, so the slot is never
    // empty. The shader does not read it then.
    SDL_GPUTexture* m_NoShadow = nullptr;

    SDL_GPUTextureFormat m_DepthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

    // A depth format that can also be sampled, for the shadow map and the
    // prepass depth post-process materials read.
    SDL_GPUTextureFormat m_SampledDepthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

    SDL_GPUTextureFormat m_SceneFormat = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;

    // The CPU copies of the primitive shapes, which the ray tracer builds from.
    // A deque, because meshes point into it.
    std::deque<std::pair<std::vector<MeshVertex>, std::vector<uint32_t>>> m_PrimitiveGeometry;

    // Primitive meshes and uploaded models.
    MeshBuffers m_BoxMesh;
    MeshBuffers m_SphereMesh;
    MeshBuffers m_SphereMeshes[3];

    // The camera this frame is being drawn from, so anything deciding how
    // much detail an object is worth can ask where it is being looked at from.
    FrameCamera m_FrameCamera;
    // A deque, because draw records point into it and adding a mesh must not
    // move the ones already there.
    std::deque<std::pair<int, MeshBuffers>> m_CapsuleMeshes;
    MeshBuffers m_QuadMesh;
    // Held by pointer, not by value: an instance recorded earlier in the
    // frame holds a MeshBuffers*, and a scene with several models grows this
    // while that frame is still being collected. A vector of values would
    // move its contents out from under those pointers.
    std::vector<std::unique_ptr<MeshBuffers>> m_ModelMeshes;

    // Which revision of each model is on the GPU, so a model that arrives
    // from the loading thread replaces the buffers built from its placeholder.
    std::vector<uint32_t> m_ModelRevisions;

    // Meshes sent to the GPU this frame. Several models arriving together
    // would otherwise upload megabytes in one frame, which is the hitch
    // background loading exists to avoid; the rest wait a frame and draw
    // their placeholder until then.
    int m_UploadsThisFrame = 0;

    // The frame being built.
    std::vector<PendingInstance> m_Instances;
    std::vector<uint32_t> m_OpaqueOrder;
    std::vector<uint32_t> m_TransparentOrder;
    std::vector<uint32_t> m_OverlayBehind;
    std::vector<uint32_t> m_OverlayFront;
    std::vector<uint32_t> m_ShadowOrder;

    // Where each list starts in the uploaded instance buffer.
    uint32_t m_OpaqueBase = 0;
    uint32_t m_TransparentBase = 0;
    uint32_t m_BehindBase = 0;
    uint32_t m_FrontBase = 0;
    uint32_t m_ShadowBase = 0;
    std::vector<GpuPointLight> m_Lights;

    // Which lights reach which cell: two values per cell naming where its
    // slice of the index list starts and how long it is.
    std::vector<uint32_t> m_ClusterRanges;
    std::vector<uint32_t> m_ClusterIndices;
    float m_ClusterScale = 0.0f;
    float m_ClusterBias = 0.0f;
    SDL_GPUBuffer* m_ClusterRangeBuffer = nullptr;
    SDL_GPUBuffer* m_ClusterIndexBuffer = nullptr;
    size_t m_ClusterRangeCapacity = 0;
    size_t m_ClusterIndexCapacity = 0;

    // Works out which lights reach which cell, once a frame.
    void BuildClusters(const FrameCamera& camera, float aspect);
    std::vector<float> m_LineVertices;
    std::vector<float> m_ParamData;

    // GPU copies of the above.
    SDL_GPUBuffer* m_InstanceBuffer = nullptr;
    SDL_GPUBuffer* m_JointBuffer = nullptr;
    size_t m_JointCapacity = 0;

    // Every skinned actor's palette this frame, one block after another.
    std::vector<Mat4> m_JointMatrices;

    // Every morphing actor's active targets this frame, four words each: where
    // the target's displacements start, the first vertex it moves, how many,
    // and the weight's bits. Only targets with a non-zero weight are listed.
    std::vector<uint32_t> m_MorphChannels;
    SDL_GPUBuffer* m_MorphBuffer = nullptr;
    size_t m_MorphCapacity = 0;

    // Bound in the displacement slot for a mesh without morph targets, whose
    // vertices never read it but whose draw still needs something there.
    SDL_GPUBuffer* m_NoMorphDeltas = nullptr;

    // Binds the per-frame vertex-stage buffers, and the mesh's displacements
    // when they differ from what is bound.
    void BindVertexStorage(SDL_GPURenderPass* pass);
    void BindMorphDeltas(SDL_GPURenderPass* pass, const MeshBuffers& mesh, SDL_GPUBuffer*& bound);
    SDL_GPUBuffer* m_ParamBuffer = nullptr;
    SDL_GPUBuffer* m_LightBuffer = nullptr;
    SDL_GPUBuffer* m_LineBuffer = nullptr;
    SDL_GPUTransferBuffer* m_Transfer = nullptr;
    size_t m_InstanceCapacity = 0;
    size_t m_ParamCapacity = 0;
    size_t m_LightCapacity = 0;
    size_t m_LineCapacity = 0;
    size_t m_TransferCapacity = 0;

    // Previous-frame state for motion vectors and TAA.
    std::vector<std::pair<uint32_t, Mat4>> m_PreviousModels;
    std::vector<std::pair<uint32_t, Mat4>> m_CurrentModels;
    Mat4 m_PreviousViewProjection;
    bool m_HasPreviousViewProjection = false;
    uint32_t m_FrameIndex = 0;

    // Kept so they can be pushed again after a pipeline change.
    std::vector<uint8_t> m_SceneUniforms;

    bool m_ShadowsThisFrame = false;

    // Around every cascade at once: what decides whether an object can cast
    // a shadow anyone sees.
    Mat4 m_ShadowViewProjection;

    uint32_t m_DrawCalls = 0;
    uint32_t m_InstanceCount = 0;
    uint32_t m_CulledCount = 0;
    uint32_t m_LargestBatch = 0;
};

// Six planes in the order left, right, bottom, top, near, far. Built from a
// view-projection matrix and used to reject objects before they reach the GPU.
struct Frustum
{
    float Planes[6][4] = {};

    static Frustum FromViewProjection(const Mat4& viewProjection);
    bool ContainsSphere(const Vec3& centre, float radius) const;
};

} // namespace ludifex::detail
