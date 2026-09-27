#include "RayScene.h"

#include "Models.h"
#include "WorldTextures.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

namespace ludifex::detail
{
namespace
{

// Deeper than this, a node becomes a leaf however many items it holds. The
// tracing shaders keep a stack of 32 entries, and walking one path down a tree
// never needs more than one entry past its depth.
constexpr uint32_t MaxDepth = 28;
constexpr int BinCount = 12;

// A node with more items than this is split even when the heuristic says a
// split costs more than it saves. Boxes that cannot be told apart at all are
// the only way to get a larger leaf.
constexpr uint32_t LargestLeaf = 16;

// Frames a piece of geometry may go undrawn before its tree is let go.
constexpr uint32_t StaleFrames = 600;

// The renderer's flag for an actor lit by nothing, as in Render3D.cpp.
constexpr uint32_t InstanceFlagUnlit = 1;

BuildBox EmptyBox()
{
    const float infinity = std::numeric_limits<float>::infinity();
    return BuildBox{ { infinity, infinity, infinity }, { -infinity, -infinity, -infinity } };
}

void Grow(BuildBox& box, const BuildBox& other)
{
    for (int axis = 0; axis < 3; ++axis)
    {
        box.Min[axis] = std::min(box.Min[axis], other.Min[axis]);
        box.Max[axis] = std::max(box.Max[axis], other.Max[axis]);
    }
}

void GrowPoint(BuildBox& box, const float* point)
{
    for (int axis = 0; axis < 3; ++axis)
    {
        box.Min[axis] = std::min(box.Min[axis], point[axis]);
        box.Max[axis] = std::max(box.Max[axis], point[axis]);
    }
}

// Half the surface area, which is all a comparison of areas needs.
float HalfArea(const BuildBox& box)
{
    const float x = std::max(0.0f, box.Max[0] - box.Min[0]);
    const float y = std::max(0.0f, box.Max[1] - box.Min[1]);
    const float z = std::max(0.0f, box.Max[2] - box.Min[2]);
    return x * y + y * z + z * x;
}

float Centre(const BuildBox& box, int axis)
{
    return 0.5f * (box.Min[axis] + box.Max[axis]);
}

uint64_t Combine(uint64_t hash, uint64_t value)
{
    hash ^= value + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2);
    return hash;
}

// FNV-1a over raw bytes.
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

// A unit normal as two signed sixteen-bit numbers: the sphere folded onto an
// octahedron and flattened (Cigolle et al.), which spends the bits evenly
// over every direction.
uint32_t PackNormal(const float* normal)
{
    const float x = normal[0];
    const float y = normal[1];
    const float z = normal[2];
    const float length = std::fabs(x) + std::fabs(y) + std::fabs(z);
    if (length <= 0.0f)
    {
        return 0;
    }

    float u = x / length;
    float v = y / length;
    if (z < 0.0f)
    {
        const float foldedU = (1.0f - std::fabs(v)) * (u >= 0.0f ? 1.0f : -1.0f);
        const float foldedV = (1.0f - std::fabs(u)) * (v >= 0.0f ? 1.0f : -1.0f);
        u = foldedU;
        v = foldedV;
    }

    const auto Quantize = [](float value) {
        const long rounded = std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f);
        return static_cast<uint32_t>(static_cast<uint16_t>(static_cast<int16_t>(rounded)));
    };
    return Quantize(u) | (Quantize(v) << 16);
}

// Each corner is eight floats: position, normal, and texture coordinates.
// These are the first eight floats of a model vertex, and the layout a pose is
// kept in.
void AppendTriangle(std::vector<RayTriangleGpu>& triangles, std::vector<RayShadingGpu>& shading, const float* a,
                    const float* b, const float* c)
{
    RayTriangleGpu triangle{};
    for (int axis = 0; axis < 3; ++axis)
    {
        triangle.Vertex[axis] = a[axis];
        triangle.Edge1[axis] = b[axis] - a[axis];
        triangle.Edge2[axis] = c[axis] - a[axis];
    }
    triangles.push_back(triangle);

    RayShadingGpu corners{};
    corners.Normals[0] = PackNormal(a + 3);
    corners.Normals[1] = PackNormal(b + 3);
    corners.Normals[2] = PackNormal(c + 3);
    corners.UV01[0] = a[6];
    corners.UV01[1] = a[7];
    corners.UV01[2] = b[6];
    corners.UV01[3] = b[7];
    corners.UV2[0] = c[6];
    corners.UV2[1] = c[7];
    shading.push_back(corners);
}

// The inverse of a model matrix, as the three rows of a 3x4: what carries a
// world-space ray into the mesh's own space.
void WorldToObjectRows(const Mat4& model, float out[12])
{
    const float* m = model.M; // column-major: row r, column c is m[c * 4 + r]
    const float a00 = m[0], a01 = m[4], a02 = m[8];
    const float a10 = m[1], a11 = m[5], a12 = m[9];
    const float a20 = m[2], a21 = m[6], a22 = m[10];

    const float c00 = a11 * a22 - a12 * a21;
    const float c01 = a02 * a21 - a01 * a22;
    const float c02 = a01 * a12 - a02 * a11;
    const float c10 = a12 * a20 - a10 * a22;
    const float c11 = a00 * a22 - a02 * a20;
    const float c12 = a02 * a10 - a00 * a12;
    const float c20 = a10 * a21 - a11 * a20;
    const float c21 = a01 * a20 - a00 * a21;
    const float c22 = a00 * a11 - a01 * a10;

    const float determinant = a00 * c00 + a01 * c10 + a02 * c20;
    const float inverse = std::fabs(determinant) > 1e-20f ? 1.0f / determinant : 0.0f;

    const float rows[3][3] = { { c00 * inverse, c01 * inverse, c02 * inverse },
                               { c10 * inverse, c11 * inverse, c12 * inverse },
                               { c20 * inverse, c21 * inverse, c22 * inverse } };
    const float translation[3] = { m[12], m[13], m[14] };

    for (int row = 0; row < 3; ++row)
    {
        out[row * 4 + 0] = rows[row][0];
        out[row * 4 + 1] = rows[row][1];
        out[row * 4 + 2] = rows[row][2];
        out[row * 4 + 3] = -(rows[row][0] * translation[0] + rows[row][1] * translation[1] +
                             rows[row][2] * translation[2]);
    }
}

// A local box carried into the world: the box around its eight corners.
BuildBox TransformBox(const BuildBox& box, const Mat4& model)
{
    BuildBox result = EmptyBox();
    const float* m = model.M;
    for (int corner = 0; corner < 8; ++corner)
    {
        const float x = (corner & 1) ? box.Max[0] : box.Min[0];
        const float y = (corner & 2) ? box.Max[1] : box.Min[1];
        const float z = (corner & 4) ? box.Max[2] : box.Min[2];
        const float point[3] = { m[0] * x + m[4] * y + m[8] * z + m[12], m[1] * x + m[5] * y + m[9] * z + m[13],
                                 m[2] * x + m[6] * y + m[10] * z + m[14] };
        GrowPoint(result, point);
    }
    return result;
}

bool EnsureStorage(SDL_GPUDevice* device, SDL_GPUBuffer*& buffer, size_t& capacity, size_t needed, bool& grew)
{
    needed = std::max<size_t>(needed, 256);
    if (buffer != nullptr && capacity >= needed)
    {
        return true;
    }

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
    info.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
    info.size = static_cast<uint32_t>(size);
    buffer = SDL_CreateGPUBuffer(device, &info);
    capacity = buffer != nullptr ? size : 0;
    grew = true;

    if (buffer == nullptr)
    {
        LogMessage(LogLevel::Error, "render", "Could not allocate the ray tracer's %zu-byte buffer: %s", size,
                   SDL_GetError());
        return false;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// The tree builder
// ---------------------------------------------------------------------------

void BuildTree(const std::vector<BuildBox>& boxes, uint32_t maxLeaf, std::vector<BvhNodeGpu>& outNodes,
               std::vector<uint32_t>& outOrder)
{
    const uint32_t count = static_cast<uint32_t>(boxes.size());
    outNodes.clear();
    outOrder.resize(count);
    std::iota(outOrder.begin(), outOrder.end(), 0u);
    if (count == 0)
    {
        return;
    }

    std::vector<float> centres(static_cast<size_t>(count) * 3);
    for (uint32_t index = 0; index < count; ++index)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            centres[index * 3 + axis] = Centre(boxes[index], axis);
        }
    }

    struct Task
    {
        uint32_t Node;
        uint32_t Begin;
        uint32_t End;
        uint32_t Depth;
    };

    outNodes.reserve(static_cast<size_t>(count) * 2);
    outNodes.push_back(BvhNodeGpu{});
    std::vector<Task> tasks{ { 0, 0, count, 0 } };

    while (!tasks.empty())
    {
        const Task task = tasks.back();
        tasks.pop_back();

        BuildBox bounds = EmptyBox();
        BuildBox centreBounds = EmptyBox();
        for (uint32_t index = task.Begin; index < task.End; ++index)
        {
            const uint32_t item = outOrder[index];
            Grow(bounds, boxes[item]);
            GrowPoint(centreBounds, &centres[item * 3]);
        }

        const uint32_t items = task.End - task.Begin;
        BvhNodeGpu& node = outNodes[task.Node];
        std::memcpy(node.Min, bounds.Min, sizeof(node.Min));
        std::memcpy(node.Max, bounds.Max, sizeof(node.Max));
        node.First = task.Begin;
        node.Count = items;

        if (items <= maxLeaf || task.Depth >= MaxDepth)
        {
            continue;
        }

        // The cheapest of the candidate splits: each child costs its item
        // count, weighted by the chance a ray crossing the parent crosses it
        // too, which is the ratio of their surface areas.
        float bestCost = std::numeric_limits<float>::infinity();
        int bestAxis = -1;
        int bestSplit = 0;

        for (int axis = 0; axis < 3; ++axis)
        {
            const float extent = centreBounds.Max[axis] - centreBounds.Min[axis];
            if (!(extent > 1e-12f))
            {
                continue;
            }
            const float scale = static_cast<float>(BinCount) / extent;

            BuildBox binBoxes[BinCount];
            uint32_t binCounts[BinCount] = {};
            for (BuildBox& box : binBoxes)
            {
                box = EmptyBox();
            }
            for (uint32_t index = task.Begin; index < task.End; ++index)
            {
                const uint32_t item = outOrder[index];
                const int bin = std::min(BinCount - 1,
                                         static_cast<int>((centres[item * 3 + axis] - centreBounds.Min[axis]) * scale));
                ++binCounts[bin];
                Grow(binBoxes[bin], boxes[item]);
            }

            float leftArea[BinCount - 1];
            uint32_t leftCount[BinCount - 1];
            BuildBox running = EmptyBox();
            uint32_t runningCount = 0;
            for (int bin = 0; bin < BinCount - 1; ++bin)
            {
                Grow(running, binBoxes[bin]);
                runningCount += binCounts[bin];
                leftArea[bin] = runningCount > 0 ? HalfArea(running) : 0.0f;
                leftCount[bin] = runningCount;
            }

            running = EmptyBox();
            runningCount = 0;
            for (int bin = BinCount - 1; bin > 0; --bin)
            {
                Grow(running, binBoxes[bin]);
                runningCount += binCounts[bin];
                if (leftCount[bin - 1] == 0 || runningCount == 0)
                {
                    continue;
                }
                const float cost = leftArea[bin - 1] * static_cast<float>(leftCount[bin - 1]) +
                                   HalfArea(running) * static_cast<float>(runningCount);
                if (cost < bestCost)
                {
                    bestCost = cost;
                    bestAxis = axis;
                    bestSplit = bin - 1;
                }
            }
        }

        const float parentArea = HalfArea(bounds);
        const float leafCost = static_cast<float>(items) * parentArea;
        const float splitCost = parentArea + bestCost;
        if ((bestAxis < 0 || splitCost >= leafCost) && items <= LargestLeaf)
        {
            continue;
        }

        uint32_t middle = task.Begin;
        if (bestAxis >= 0)
        {
            const float extent = centreBounds.Max[bestAxis] - centreBounds.Min[bestAxis];
            const float scale = static_cast<float>(BinCount) / extent;
            const float low = centreBounds.Min[bestAxis];
            auto* split = std::partition(outOrder.data() + task.Begin, outOrder.data() + task.End, [&](uint32_t item) {
                const int bin = std::min(BinCount - 1, static_cast<int>((centres[item * 3 + bestAxis] - low) * scale));
                return bin <= bestSplit;
            });
            middle = static_cast<uint32_t>(split - outOrder.data());
        }

        // Boxes whose centres coincide cannot be split by position; halving
        // the list still keeps every leaf small.
        if (middle == task.Begin || middle == task.End)
        {
            middle = task.Begin + items / 2;
            const int axis = bestAxis >= 0 ? bestAxis : 0;
            std::nth_element(outOrder.begin() + task.Begin, outOrder.begin() + middle, outOrder.begin() + task.End,
                             [&](uint32_t a, uint32_t b) { return centres[a * 3 + axis] < centres[b * 3 + axis]; });
        }

        const uint32_t left = static_cast<uint32_t>(outNodes.size());
        outNodes.push_back(BvhNodeGpu{});
        outNodes.push_back(BvhNodeGpu{});
        outNodes[task.Node].First = left;
        outNodes[task.Node].Count = 0;

        tasks.push_back({ left + 1, middle, task.End, task.Depth + 1 });
        tasks.push_back({ left, task.Begin, middle, task.Depth + 1 });
    }
}

// ---------------------------------------------------------------------------
// The scene
// ---------------------------------------------------------------------------

bool RayScene::IsSupported(SDL_GPUDevice* device)
{
    const SDL_GPUTextureUsageFlags written = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
    return device != nullptr &&
           SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, SDL_GPU_TEXTURETYPE_2D,
                                        written) &&
           SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT, SDL_GPU_TEXTURETYPE_2D,
                                        written) &&
           SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB, SDL_GPU_TEXTURETYPE_2D,
                                        SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
}

void RayScene::Shutdown(SDL_GPUDevice* device)
{
    if (device != nullptr)
    {
        for (SDL_GPUBuffer** buffer : { &m_TopNodeBuffer, &m_NodeBuffer, &m_TriangleBuffer, &m_ShadingBuffer,
                                        &m_InstanceBuffer, &m_LightBuffer })
        {
            if (*buffer != nullptr)
            {
                SDL_ReleaseGPUBuffer(device, *buffer);
            }
        }
        if (m_Atlas != nullptr)
        {
            SDL_ReleaseGPUTexture(device, m_Atlas);
        }
        if (m_Transfer != nullptr)
        {
            SDL_ReleaseGPUTransferBuffer(device, m_Transfer);
        }
    }

    m_TopNodeBuffer = m_NodeBuffer = m_TriangleBuffer = m_ShadingBuffer = m_InstanceBuffer = m_LightBuffer = nullptr;
    m_TopNodeCapacity = m_NodeCapacity = m_TriangleCapacity = m_ShadingCapacity = m_InstanceCapacity =
        m_LightCapacity = 0;
    m_Atlas = nullptr;
    m_Transfer = nullptr;
    m_TransferCapacity = 0;

    m_Trees.clear();
    m_StaticNodes.clear();
    m_StaticTriangles.clear();
    m_StaticShading.clear();
    m_PosedTrees.clear();
    m_Poses.clear();
    m_Cells.clear();
    m_CellByKey.clear();
    m_PendingCells.clear();
    m_StaticChanged = true;
}

void RayScene::BeginFrame()
{
    ++m_Frame;
    m_Placed.clear();
    m_PosedNodes.clear();
    m_PosedTriangles.clear();
    m_PosedShading.clear();
    m_Lights.clear();

    // Every so often, geometry not drawn for a while is released. The
    // static buffers are rebuilt from scratch as things are drawn again, which
    // is simpler than compacting them and happens rarely.
    if (m_Frame % StaleFrames == 0)
    {
        bool stale = false;
        for (const auto& [key, tree] : m_Trees)
        {
            stale = stale || tree.LastUsed + StaleFrames < m_Frame;
        }
        if (stale)
        {
            m_Trees.clear();
            m_StaticNodes.clear();
            m_StaticTriangles.clear();
            m_StaticShading.clear();
            m_StaticChanged = true;
        }

        for (auto tree = m_PosedTrees.begin(); tree != m_PosedTrees.end();)
        {
            tree = tree->second.LastUsed + StaleFrames < m_Frame ? m_PosedTrees.erase(tree) : std::next(tree);
        }
        for (auto pose = m_Poses.begin(); pose != m_Poses.end();)
        {
            pose = pose->second.Frame + StaleFrames < m_Frame ? m_Poses.erase(pose) : std::next(pose);
        }
    }
}

bool RayScene::TreeFor(const PendingInstance& instance, uint32_t& outRoot, BuildBox& outBounds)
{
    const MeshBuffers& mesh = *instance.Mesh;
    if (mesh.CpuVertices == nullptr || mesh.CpuIndices == nullptr || instance.IndexCount < 3 ||
        static_cast<uint64_t>(instance.FirstIndex) + instance.IndexCount > mesh.CpuIndexCount)
    {
        return false;
    }

    const uint64_t key = Combine(Combine(mesh.Serial, instance.FirstIndex), instance.IndexCount);
    const auto found = m_Trees.find(key);
    if (found != m_Trees.end())
    {
        found->second.LastUsed = m_Frame;
        outRoot = found->second.Root;
        outBounds = found->second.Bounds;
        return true;
    }

    const uint32_t* indices = mesh.CpuIndices + instance.FirstIndex;
    const uint32_t triangleCount = instance.IndexCount / 3;
    const auto Corner = [&](uint32_t triangle, int corner) {
        const uint32_t vertex = std::min(indices[triangle * 3 + corner], mesh.CpuVertexCount - 1);
        return mesh.CpuVertices[vertex].Position;
    };

    std::vector<BuildBox> boxes(triangleCount);
    for (uint32_t triangle = 0; triangle < triangleCount; ++triangle)
    {
        boxes[triangle] = EmptyBox();
        for (int corner = 0; corner < 3; ++corner)
        {
            GrowPoint(boxes[triangle], Corner(triangle, corner));
        }
    }

    std::vector<BvhNodeGpu> nodes;
    std::vector<uint32_t> order;
    BuildTree(boxes, 4, nodes, order);

    const uint32_t nodeBase = static_cast<uint32_t>(m_StaticNodes.size());
    const uint32_t triangleBase = static_cast<uint32_t>(m_StaticTriangles.size());
    for (BvhNodeGpu node : nodes)
    {
        node.First += node.Count > 0 ? triangleBase : nodeBase;
        m_StaticNodes.push_back(node);
    }
    for (uint32_t triangle : order)
    {
        AppendTriangle(m_StaticTriangles, m_StaticShading, Corner(triangle, 0), Corner(triangle, 1),
                       Corner(triangle, 2));
    }

    MeshTree tree;
    tree.Root = nodeBase;
    std::memcpy(tree.Bounds.Min, nodes[0].Min, sizeof(tree.Bounds.Min));
    std::memcpy(tree.Bounds.Max, nodes[0].Max, sizeof(tree.Bounds.Max));
    tree.LastUsed = m_Frame;
    m_Trees.emplace(key, tree);
    m_StaticChanged = true;

    outRoot = tree.Root;
    outBounds = tree.Bounds;
    return true;
}

const std::vector<float>& RayScene::PoseOf(const PendingInstance& instance)
{
    const MeshBuffers& mesh = *instance.Mesh;
    Pose& pose = m_Poses[Combine(instance.ActorKey, mesh.Serial)];
    if (pose.Frame == m_Frame && !pose.Vertices.empty())
    {
        return pose.Vertices;
    }
    pose.Frame = m_Frame;

    const uint32_t vertexCount = mesh.CpuVertexCount;
    pose.Vertices.resize(static_cast<size_t>(vertexCount) * 8);
    for (uint32_t vertex = 0; vertex < vertexCount; ++vertex)
    {
        std::memcpy(&pose.Vertices[vertex * 8], mesh.CpuVertices[vertex].Position, sizeof(float) * 8);
    }

    // Blend shapes first, then the skeleton, as glTF orders them and as the
    // vertex shader does.
    if (instance.MorphTargets != nullptr && instance.SourceModel != nullptr)
    {
        const std::vector<MorphTarget>& targets = *instance.MorphTargets;
        const std::vector<float>* weights =
            instance.MorphWeights != nullptr && instance.MorphWeights->size() == targets.size() ? instance.MorphWeights
                                                                                                 : nullptr;
        const std::vector<float>& deltas = instance.SourceModel->MorphDeltas;

        for (size_t target = 0; target < targets.size(); ++target)
        {
            const float weight = weights != nullptr ? (*weights)[target] : targets[target].DefaultWeight;
            if (std::fabs(weight) < 1e-4f)
            {
                continue;
            }
            for (uint32_t local = 0; local < targets[target].VertexCount; ++local)
            {
                const uint32_t vertex = targets[target].VertexStart + local;
                const size_t at = (static_cast<size_t>(targets[target].DeltaOffset) + local * 2) * 4;
                if (vertex >= vertexCount || at + 7 >= deltas.size())
                {
                    continue;
                }
                float* posed = &pose.Vertices[vertex * 8];
                for (int axis = 0; axis < 3; ++axis)
                {
                    posed[axis] += deltas[at + axis] * weight;
                    posed[3 + axis] += deltas[at + 4 + axis] * weight;
                }
            }
        }
    }

    if (instance.Palette != nullptr && !instance.Palette->empty())
    {
        const std::vector<Mat4>& palette = *instance.Palette;
        for (uint32_t vertex = 0; vertex < vertexCount; ++vertex)
        {
            const ModelVertex& source = mesh.CpuVertices[vertex];
            float blend[12] = {};
            float total = 0.0f;
            for (int slot = 0; slot < 4; ++slot)
            {
                const float weight = static_cast<float>(source.Weights[slot]) / 255.0f;
                if (weight <= 0.0f || source.Joints[slot] >= palette.size())
                {
                    continue;
                }
                const float* joint = palette[source.Joints[slot]].M;
                for (int row = 0; row < 3; ++row)
                {
                    for (int column = 0; column < 4; ++column)
                    {
                        blend[row * 4 + column] += joint[column * 4 + row] * weight;
                    }
                }
                total += weight;
            }

            // A part no bone moves keeps the model's own placement, exactly as
            // the vertex shader leaves it.
            if (total <= 0.0f)
            {
                continue;
            }

            float* posed = &pose.Vertices[vertex * 8];
            const float position[3] = { posed[0], posed[1], posed[2] };
            const float normal[3] = { posed[3], posed[4], posed[5] };
            for (int row = 0; row < 3; ++row)
            {
                posed[row] = blend[row * 4 + 0] * position[0] + blend[row * 4 + 1] * position[1] +
                             blend[row * 4 + 2] * position[2] + blend[row * 4 + 3];
                posed[3 + row] =
                    blend[row * 4 + 0] * normal[0] + blend[row * 4 + 1] * normal[1] + blend[row * 4 + 2] * normal[2];
            }
        }
    }

    return pose.Vertices;
}

bool RayScene::PosedTreeFor(const PendingInstance& instance, uint32_t& outRoot, BuildBox& outBounds)
{
    const MeshBuffers& mesh = *instance.Mesh;
    if (mesh.CpuVertices == nullptr || mesh.CpuIndices == nullptr || instance.IndexCount < 3 ||
        static_cast<uint64_t>(instance.FirstIndex) + instance.IndexCount > mesh.CpuIndexCount)
    {
        return false;
    }

    const std::vector<float>& pose = PoseOf(instance);
    const uint32_t* indices = mesh.CpuIndices + instance.FirstIndex;
    const uint32_t triangleCount = instance.IndexCount / 3;
    const auto Corner = [&](uint32_t triangle, int corner) {
        const uint32_t vertex = std::min(indices[triangle * 3 + corner], mesh.CpuVertexCount - 1);
        return &pose[static_cast<size_t>(vertex) * 8];
    };

    const uint64_t key = Combine(Combine(Combine(instance.ActorKey, mesh.Serial), instance.FirstIndex),
                                 instance.IndexCount);
    PosedTree& tree = m_PosedTrees[key];
    tree.LastUsed = m_Frame;

    // The tree's shape comes from the first pose it is seen in, and every
    // later pose refits the same shape: a limb moving a little leaves the
    // grouping of its triangles about as good as it was.
    if (tree.Nodes.empty())
    {
        std::vector<BuildBox> boxes(triangleCount);
        for (uint32_t triangle = 0; triangle < triangleCount; ++triangle)
        {
            boxes[triangle] = EmptyBox();
            for (int corner = 0; corner < 3; ++corner)
            {
                GrowPoint(boxes[triangle], Corner(triangle, corner));
            }
        }
        BuildTree(boxes, 4, tree.Nodes, tree.Order);
    }

    const uint32_t nodeBase = static_cast<uint32_t>(m_PosedNodes.size());
    const uint32_t triangleBase = static_cast<uint32_t>(m_PosedTriangles.size());
    for (uint32_t triangle : tree.Order)
    {
        AppendTriangle(m_PosedTriangles, m_PosedShading, Corner(triangle, 0), Corner(triangle, 1),
                       Corner(triangle, 2));
    }

    // Children always come after their parent, so walking backwards refits
    // every child before the parent that encloses it.
    m_PosedNodes.resize(nodeBase + tree.Nodes.size());
    for (size_t index = tree.Nodes.size(); index-- > 0;)
    {
        const BvhNodeGpu& shape = tree.Nodes[index];
        BuildBox box = EmptyBox();
        if (shape.Count > 0)
        {
            for (uint32_t item = shape.First; item < shape.First + shape.Count; ++item)
            {
                for (int corner = 0; corner < 3; ++corner)
                {
                    GrowPoint(box, Corner(tree.Order[item], corner));
                }
            }
        }
        else
        {
            for (uint32_t child = shape.First; child <= shape.First + 1; ++child)
            {
                const BvhNodeGpu& fitted = m_PosedNodes[nodeBase + child];
                BuildBox childBox;
                std::memcpy(childBox.Min, fitted.Min, sizeof(childBox.Min));
                std::memcpy(childBox.Max, fitted.Max, sizeof(childBox.Max));
                Grow(box, childBox);
            }
        }

        BvhNodeGpu& node = m_PosedNodes[nodeBase + index];
        node = shape;
        std::memcpy(node.Min, box.Min, sizeof(node.Min));
        std::memcpy(node.Max, box.Max, sizeof(node.Max));
        node.First += shape.Count > 0 ? triangleBase : nodeBase;
    }

    outRoot = nodeBase;
    std::memcpy(outBounds.Min, m_PosedNodes[nodeBase].Min, sizeof(outBounds.Min));
    std::memcpy(outBounds.Max, m_PosedNodes[nodeBase].Max, sizeof(outBounds.Max));
    return true;
}

uint32_t RayScene::CellFor(const TextureId& texture)
{
    if (!texture.IsValid() || GetWorldTextures().Resolve(texture) == nullptr)
    {
        return 0;
    }

    const uint64_t key = (static_cast<uint64_t>(texture.Index) << 32) | texture.Generation;
    const auto found = m_CellByKey.find(key);
    if (found != m_CellByKey.end())
    {
        m_Cells[found->second].LastUsed = m_Frame;
        return found->second + 1;
    }

    if (m_Cells.empty())
    {
        m_Cells.resize(AtlasCellCount);
    }

    // A free cell, or else the least recently used one, but never one this
    // frame has already requested.
    uint32_t chosen = AtlasCellCount;
    uint32_t oldest = m_Frame;
    for (uint32_t cell = 0; cell < AtlasCellCount; ++cell)
    {
        if (!m_Cells[cell].Used)
        {
            chosen = cell;
            break;
        }
        if (m_Cells[cell].LastUsed < oldest)
        {
            oldest = m_Cells[cell].LastUsed;
            chosen = cell;
        }
    }
    if (chosen == AtlasCellCount)
    {
        if (!m_AtlasFullReported)
        {
            LogMessage(LogLevel::Warning, "render",
                       "More than %u textures are in view of the ray tracer at once; the rest are traced as their "
                       "colour alone.",
                       AtlasCellCount);
            m_AtlasFullReported = true;
        }
        return 0;
    }

    if (m_Cells[chosen].Used)
    {
        m_CellByKey.erase(m_Cells[chosen].Key);
    }
    m_Cells[chosen] = AtlasCell{ key, m_Frame, true };
    m_CellByKey[key] = chosen;
    m_PendingCells.push_back({ chosen, texture });
    return chosen + 1;
}

void RayScene::Add(const PendingInstance& instance, bool transparent)
{
    if (instance.Mesh == nullptr)
    {
        return;
    }

    const bool posed = (instance.Palette != nullptr && !instance.Palette->empty()) ||
                       (instance.MorphTargets != nullptr && !instance.MorphTargets->empty());

    uint32_t root = 0;
    BuildBox local{};
    if (!(posed ? PosedTreeFor(instance, root, local) : TreeFor(instance, root, local)))
    {
        return;
    }

    Placed placed{};
    placed.Posed = posed;
    WorldToObjectRows(instance.Model, placed.Gpu.WorldToObject);

    uint32_t flags = 0;
    if ((instance.Flags & InstanceFlagUnlit) != 0)
    {
        flags |= RayFlagUnlit;
    }
    if (transparent)
    {
        flags |= RayFlagTransparent;
    }

    placed.Gpu.Geometry[0] = root;
    placed.Gpu.Geometry[1] = flags;
    placed.Gpu.Geometry[2] = CellFor(instance.Texture);
    placed.Gpu.Geometry[3] = instance.EmissiveMap.IsValid() ? CellFor(instance.EmissiveMap) : 0;

    placed.Gpu.BaseColor[0] = instance.Tint.R;
    placed.Gpu.BaseColor[1] = instance.Tint.G;
    placed.Gpu.BaseColor[2] = instance.Tint.B;
    placed.Gpu.BaseColor[3] = instance.Tint.A;
    placed.Gpu.Surface[0] = instance.Roughness;
    placed.Gpu.Surface[1] = instance.Metallic;
    placed.Gpu.Emission[0] = instance.Emission.X;
    placed.Gpu.Emission[1] = instance.Emission.Y;
    placed.Gpu.Emission[2] = instance.Emission.Z;
    placed.Gpu.UVTransform[0] = instance.UVScale.X;
    placed.Gpu.UVTransform[1] = instance.UVScale.Y;
    placed.Gpu.UVTransform[2] = instance.UVOffset.X;
    placed.Gpu.UVTransform[3] = instance.UVOffset.Y;

    placed.Bounds = TransformBox(local, instance.Model);
    m_Placed.push_back(placed);
}

void RayScene::SetLights(const std::vector<GpuPointLight>& lights)
{
    m_Lights = lights;
}

bool RayScene::Upload(SDL_GPUDevice* device, SDL_GPUCommandBuffer* commandBuffer)
{
    // --- the atlas -----------------------------------------------------------
    if (m_Atlas == nullptr)
    {
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        info.width = AtlasSize;
        info.height = AtlasSize;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        m_Atlas = SDL_CreateGPUTexture(device, &info);
        if (m_Atlas == nullptr)
        {
            LogMessage(LogLevel::Error, "render", "Could not create the ray tracer's texture atlas: %s",
                       SDL_GetError());
            return false;
        }
    }

    // Each new texture is copied into its cell from the level of its mip
    // chain nearest the cell's size, which the copy's filtering then shrinks
    // without skipping texels.
    WorldTextureStore& textures = GetWorldTextures();
    for (const auto& [cell, texture] : m_PendingCells)
    {
        SDL_GPUTexture* source = textures.GetGpuTexture(device, texture);
        const WorldTextureRecord* record = textures.Resolve(texture);
        if (source == nullptr || record == nullptr || record->Width <= 0 || record->Height <= 0)
        {
            continue;
        }

        const int largest = std::max(record->Width, record->Height);
        int level = 0;
        while ((largest >> (level + 1)) >= static_cast<int>(AtlasCellSize) && level + 1 < record->LevelCount)
        {
            ++level;
        }

        SDL_GPUBlitInfo blit{};
        blit.source.texture = source;
        blit.source.mip_level = static_cast<Uint32>(level);
        blit.source.w = static_cast<Uint32>(std::max(1, record->Width >> level));
        blit.source.h = static_cast<Uint32>(std::max(1, record->Height >> level));
        blit.destination.texture = m_Atlas;
        blit.destination.x = (cell % AtlasCellsPerRow) * AtlasCellSize;
        blit.destination.y = (cell / AtlasCellsPerRow) * AtlasCellSize;
        blit.destination.w = AtlasCellSize;
        blit.destination.h = AtlasCellSize;
        blit.load_op = SDL_GPU_LOADOP_LOAD;
        blit.filter = SDL_GPU_FILTER_LINEAR;
        SDL_BlitGPUTexture(commandBuffer, &blit);
    }
    m_PendingCells.clear();

    // --- the tree over instances ----------------------------------------------
    //
    // Posed geometry goes after the static, so everything posed moves up by
    // the static geometry's size now that it is known for this frame.
    const uint32_t staticNodes = static_cast<uint32_t>(m_StaticNodes.size());
    const uint32_t staticTriangles = static_cast<uint32_t>(m_StaticTriangles.size());
    for (BvhNodeGpu& node : m_PosedNodes)
    {
        node.First += node.Count > 0 ? staticTriangles : staticNodes;
    }

    std::vector<BvhNodeGpu> topNodes;
    std::vector<RayInstanceGpu> instances;
    if (m_Placed.empty())
    {
        // A tree with one leaf and nothing in its box, which no ray crosses.
        BvhNodeGpu empty{};
        const float infinity = std::numeric_limits<float>::infinity();
        empty.Min[0] = empty.Min[1] = empty.Min[2] = infinity;
        empty.Max[0] = empty.Max[1] = empty.Max[2] = -infinity;
        empty.Count = 1;
        topNodes.push_back(empty);
        instances.push_back(RayInstanceGpu{});
    }
    else
    {
        std::vector<BuildBox> boxes;
        boxes.reserve(m_Placed.size());
        for (const Placed& placed : m_Placed)
        {
            boxes.push_back(placed.Bounds);
        }
        std::vector<uint32_t> order;
        BuildTree(boxes, 2, topNodes, order);

        instances.reserve(order.size());
        for (uint32_t index : order)
        {
            RayInstanceGpu gpu = m_Placed[index].Gpu;
            if (m_Placed[index].Posed)
            {
                gpu.Geometry[0] += staticNodes;
            }
            instances.push_back(gpu);
        }
    }

    std::vector<GpuPointLight> lights = m_Lights;
    if (lights.empty())
    {
        lights.push_back(GpuPointLight{});
    }

    // What a traced image depends on, so a path tracer can tell when to start
    // again: every instance, every light, and every posed triangle.
    uint64_t fingerprint = 0xCBF29CE484222325ull;
    fingerprint = HashBytes(fingerprint, instances.data(), instances.size() * sizeof(RayInstanceGpu));
    fingerprint = HashBytes(fingerprint, m_Lights.data(), m_Lights.size() * sizeof(GpuPointLight));
    fingerprint = HashBytes(fingerprint, m_PosedTriangles.data(), m_PosedTriangles.size() * sizeof(RayTriangleGpu));
    m_Fingerprint = fingerprint;

    // --- buffers ---------------------------------------------------------------
    const size_t nodeCount = std::max<size_t>(m_StaticNodes.size() + m_PosedNodes.size(), 1);
    const size_t triangleCount = std::max<size_t>(m_StaticTriangles.size() + m_PosedTriangles.size(), 1);

    bool grew = false;
    if (!EnsureStorage(device, m_TopNodeBuffer, m_TopNodeCapacity, topNodes.size() * sizeof(BvhNodeGpu), grew) ||
        !EnsureStorage(device, m_InstanceBuffer, m_InstanceCapacity, instances.size() * sizeof(RayInstanceGpu), grew) ||
        !EnsureStorage(device, m_LightBuffer, m_LightCapacity, lights.size() * sizeof(GpuPointLight), grew))
    {
        return false;
    }
    grew = false;
    if (!EnsureStorage(device, m_NodeBuffer, m_NodeCapacity, nodeCount * sizeof(BvhNodeGpu), grew) ||
        !EnsureStorage(device, m_TriangleBuffer, m_TriangleCapacity, triangleCount * sizeof(RayTriangleGpu), grew) ||
        !EnsureStorage(device, m_ShadingBuffer, m_ShadingCapacity, triangleCount * sizeof(RayShadingGpu), grew))
    {
        return false;
    }
    const bool uploadStatic = m_StaticChanged || grew;

    // Everything goes through one transfer buffer, in the order it is copied.
    struct Piece
    {
        SDL_GPUBuffer* Buffer;
        size_t Offset;
        const void* Data;
        size_t Size;
        bool Whole;
    };
    std::vector<Piece> pieces;
    pieces.push_back({ m_TopNodeBuffer, 0, topNodes.data(), topNodes.size() * sizeof(BvhNodeGpu), true });
    pieces.push_back({ m_InstanceBuffer, 0, instances.data(), instances.size() * sizeof(RayInstanceGpu), true });
    pieces.push_back({ m_LightBuffer, 0, lights.data(), lights.size() * sizeof(GpuPointLight), true });

    // A dummy entry keeps a buffer with no geometry in it valid to bind.
    static const BvhNodeGpu noNode{};
    static const RayTriangleGpu noTriangle{};
    static const RayShadingGpu noShading{};
    if (uploadStatic)
    {
        pieces.push_back({ m_NodeBuffer, 0, m_StaticNodes.empty() ? &noNode : m_StaticNodes.data(),
                           std::max<size_t>(m_StaticNodes.size(), 1) * sizeof(BvhNodeGpu), false });
        pieces.push_back({ m_TriangleBuffer, 0, m_StaticTriangles.empty() ? &noTriangle : m_StaticTriangles.data(),
                           std::max<size_t>(m_StaticTriangles.size(), 1) * sizeof(RayTriangleGpu), false });
        pieces.push_back({ m_ShadingBuffer, 0, m_StaticShading.empty() ? &noShading : m_StaticShading.data(),
                           std::max<size_t>(m_StaticShading.size(), 1) * sizeof(RayShadingGpu), false });
    }
    if (!m_PosedNodes.empty())
    {
        pieces.push_back({ m_NodeBuffer, m_StaticNodes.size() * sizeof(BvhNodeGpu), m_PosedNodes.data(),
                           m_PosedNodes.size() * sizeof(BvhNodeGpu), false });
        pieces.push_back({ m_TriangleBuffer, m_StaticTriangles.size() * sizeof(RayTriangleGpu),
                           m_PosedTriangles.data(), m_PosedTriangles.size() * sizeof(RayTriangleGpu), false });
        pieces.push_back({ m_ShadingBuffer, m_StaticShading.size() * sizeof(RayShadingGpu), m_PosedShading.data(),
                           m_PosedShading.size() * sizeof(RayShadingGpu), false });
    }

    size_t total = 0;
    for (const Piece& piece : pieces)
    {
        total += piece.Size;
    }
    if (m_Transfer == nullptr || m_TransferCapacity < total)
    {
        if (m_Transfer != nullptr)
        {
            SDL_ReleaseGPUTransferBuffer(device, m_Transfer);
        }
        size_t size = std::max<size_t>(m_TransferCapacity, 65536);
        while (size < total)
        {
            size *= 2;
        }
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = static_cast<uint32_t>(size);
        m_Transfer = SDL_CreateGPUTransferBuffer(device, &info);
        m_TransferCapacity = m_Transfer != nullptr ? size : 0;
        if (m_Transfer == nullptr)
        {
            return false;
        }
    }

    auto* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device, m_Transfer, true));
    if (mapped == nullptr)
    {
        return false;
    }
    size_t offset = 0;
    for (const Piece& piece : pieces)
    {
        std::memcpy(mapped + offset, piece.Data, piece.Size);
        offset += piece.Size;
    }
    SDL_UnmapGPUTransferBuffer(device, m_Transfer);

    // Buffers rewritten whole are cycled, so this frame's copy need not wait
    // for the last frame's tracing to finish reading. The geometry buffers are
    // written in parts (posed geometry after the static geometry that stays),
    // so they are not cycled.
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);
    offset = 0;
    for (const Piece& piece : pieces)
    {
        SDL_GPUTransferBufferLocation source{};
        source.transfer_buffer = m_Transfer;
        source.offset = static_cast<uint32_t>(offset);
        SDL_GPUBufferRegion region{};
        region.buffer = piece.Buffer;
        region.offset = static_cast<uint32_t>(piece.Offset);
        region.size = static_cast<uint32_t>(piece.Size);
        SDL_UploadToGPUBuffer(copyPass, &source, &region, piece.Whole);
        offset += piece.Size;
    }
    SDL_EndGPUCopyPass(copyPass);

    m_StaticChanged = false;
    return true;
}

} // namespace ludifex::detail
