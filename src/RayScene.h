// The world as the ray tracer sees it: trees of boxes over every mesh's
// triangles, a tree over the placed instances, a texture atlas, and the GPU
// buffers the tracing shaders read. ray_common.hlsli describes the same
// layouts from the other side.
// Not installed and not part of the public API.
//
// Each distinct piece of geometry (a mesh, or one part of a model) gets a
// tree of its own the first time it is drawn, built on the CPU in its own
// space and kept. Skinned and morphing models are the exception: their
// triangles move every frame, so each actor showing one is posed on the CPU
// and its tree refitted around the new positions. The tree over instances is
// rebuilt every frame.

#pragma once

#include "Render3D.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ludifex::detail
{

// Mirrors BvhNode in ray_common.hlsli: for an inner node First is its first
// child, the second following it, and Count is zero; for a leaf, First is its
// first item and Count how many.
struct BvhNodeGpu
{
    float Min[3];
    uint32_t First;
    float Max[3];
    uint32_t Count;
};

static_assert(sizeof(BvhNodeGpu) == 32, "node layout drifted from the shader");

struct RayTriangleGpu
{
    float Vertex[4];
    float Edge1[4];
    float Edge2[4];
};

struct RayShadingGpu
{
    uint32_t Normals[4];
    float UV01[4];
    float UV2[4];
};

struct RayInstanceGpu
{
    float WorldToObject[12];
    uint32_t Geometry[4];   // root node, flags, colour cell + 1, emission cell + 1
    float BaseColor[4];
    float Surface[4];
    float Emission[4];
    float UVTransform[4];
};

static_assert(sizeof(RayInstanceGpu) == 128, "instance layout drifted from the shader");

// Instance flags, mirroring ray_common.hlsli.
constexpr uint32_t RayFlagUnlit = 1;
constexpr uint32_t RayFlagTransparent = 2;

// The atlas: 4096 texels a side, cut into cells of 256.
constexpr uint32_t AtlasSize = 4096;
constexpr uint32_t AtlasCellSize = 256;
constexpr uint32_t AtlasCellsPerRow = AtlasSize / AtlasCellSize;
constexpr uint32_t AtlasCellCount = AtlasCellsPerRow * AtlasCellsPerRow;

// A box for the tree builder.
struct BuildBox
{
    float Min[3];
    float Max[3];
};

// Builds a tree over boxes by the surface area heuristic, binned: at every
// node, the split among a dozen candidate planes on each axis that makes the
// children cheapest to test. outNodes[0] is the root; a leaf's items are a
// range of outOrder, which lists the boxes in the order the leaves need them.
// No leaf holds more than maxLeaf items unless the boxes cannot be told apart,
// and no path is deeper than the tracing shaders' stack allows. Needs at
// least one box.
void BuildTree(const std::vector<BuildBox>& boxes, uint32_t maxLeaf, std::vector<BvhNodeGpu>& outNodes,
               std::vector<uint32_t>& outOrder);

class RayScene
{
public:
    // Whether this device can trace: it needs compute storage for the formats
    // the tracing passes write.
    static bool IsSupported(SDL_GPUDevice* device);

    void Shutdown(SDL_GPUDevice* device);

    void BeginFrame();

    // Adds one drawn object, building its geometry's tree the first time the
    // geometry is seen.
    void Add(const PendingInstance& instance, bool transparent);

    // The point lights path tracing aims at.
    void SetLights(const std::vector<GpuPointLight>& lights);

    // Builds the tree over this frame's instances, copies any new textures
    // into the atlas, and uploads everything that changed. Outside any pass.
    bool Upload(SDL_GPUDevice* device, SDL_GPUCommandBuffer* commandBuffer);

    SDL_GPUBuffer* GetTopNodes() const { return m_TopNodeBuffer; }
    SDL_GPUBuffer* GetNodes() const { return m_NodeBuffer; }
    SDL_GPUBuffer* GetTriangles() const { return m_TriangleBuffer; }
    SDL_GPUBuffer* GetShading() const { return m_ShadingBuffer; }
    SDL_GPUBuffer* GetInstances() const { return m_InstanceBuffer; }
    SDL_GPUBuffer* GetLights() const { return m_LightBuffer; }
    SDL_GPUTexture* GetAtlas() const { return m_Atlas; }

    uint32_t GetInstanceCount() const { return static_cast<uint32_t>(m_Placed.size()); }
    uint32_t GetLightCount() const { return static_cast<uint32_t>(m_Lights.size()); }

    // A hash of everything in the scene that changes what a ray finds, so a
    // path-traced image knows when it has to start again.
    uint64_t GetFingerprint() const { return m_Fingerprint; }

private:
    // One piece of geometry's tree, in the static buffers.
    struct MeshTree
    {
        uint32_t Root = 0;
        BuildBox Bounds{};
        uint32_t LastUsed = 0;
    };

    // One posed actor's part: the shape of its tree is kept, and its boxes and
    // triangles are refreshed from the pose every frame. Indices inside are
    // relative to the part itself.
    struct PosedTree
    {
        std::vector<BvhNodeGpu> Nodes;
        std::vector<uint32_t> Order;
        uint32_t LastUsed = 0;
    };

    // An instance of this frame. A posed instance's root counts from the
    // start of the posed geometry, which follows the static geometry once the
    // frame's trees are all known.
    struct Placed
    {
        RayInstanceGpu Gpu;
        BuildBox Bounds;
        bool Posed = false;
    };

    // The frame's pose of one actor: eight floats a vertex, position, normal,
    // and texture coordinates.
    struct Pose
    {
        std::vector<float> Vertices;
        uint32_t Frame = 0;
    };

    bool TreeFor(const PendingInstance& instance, uint32_t& outRoot, BuildBox& outBounds);
    bool PosedTreeFor(const PendingInstance& instance, uint32_t& outRoot, BuildBox& outBounds);
    const std::vector<float>& PoseOf(const PendingInstance& instance);
    uint32_t CellFor(const TextureId& texture);

    // Static geometry: every mesh tree, one after another.
    std::vector<BvhNodeGpu> m_StaticNodes;
    std::vector<RayTriangleGpu> m_StaticTriangles;
    std::vector<RayShadingGpu> m_StaticShading;
    std::unordered_map<uint64_t, MeshTree> m_Trees;
    bool m_StaticChanged = true;

    // Posed geometry, rebuilt every frame.
    std::unordered_map<uint64_t, PosedTree> m_PosedTrees;
    std::unordered_map<uint64_t, Pose> m_Poses;
    std::vector<BvhNodeGpu> m_PosedNodes;
    std::vector<RayTriangleGpu> m_PosedTriangles;
    std::vector<RayShadingGpu> m_PosedShading;

    std::vector<Placed> m_Placed;
    std::vector<GpuPointLight> m_Lights;
    uint32_t m_Frame = 0;
    uint64_t m_Fingerprint = 0;

    // The atlas, and which texture sits in each of its cells.
    struct AtlasCell
    {
        uint64_t Key = 0;
        uint32_t LastUsed = 0;
        bool Used = false;
    };
    std::vector<AtlasCell> m_Cells;
    std::unordered_map<uint64_t, uint32_t> m_CellByKey;
    std::vector<std::pair<uint32_t, TextureId>> m_PendingCells;
    bool m_AtlasFullReported = false;

    SDL_GPUTexture* m_Atlas = nullptr;
    SDL_GPUBuffer* m_TopNodeBuffer = nullptr;
    SDL_GPUBuffer* m_NodeBuffer = nullptr;
    SDL_GPUBuffer* m_TriangleBuffer = nullptr;
    SDL_GPUBuffer* m_ShadingBuffer = nullptr;
    SDL_GPUBuffer* m_InstanceBuffer = nullptr;
    SDL_GPUBuffer* m_LightBuffer = nullptr;
    size_t m_TopNodeCapacity = 0;
    size_t m_NodeCapacity = 0;
    size_t m_TriangleCapacity = 0;
    size_t m_ShadingCapacity = 0;
    size_t m_InstanceCapacity = 0;
    size_t m_LightCapacity = 0;
    SDL_GPUTransferBuffer* m_Transfer = nullptr;
    size_t m_TransferCapacity = 0;
};

} // namespace ludifex::detail
