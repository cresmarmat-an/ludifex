// Primitive geometry and the small GPU helpers the renderer's passes share.

#include "Render3D.h"

#include "Backend.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace ludifex::detail
{
namespace
{

constexpr float Pi = 3.14159265358979323846f;

// Every mesh uploaded in this process gets the next number.
std::atomic<uint64_t> g_NextMeshSerial{ 1 };

void AddQuad(std::vector<MeshVertex>& vertices, std::vector<uint32_t>& indices, const float corners[4][3],
             const float normal[3])
{
    const uint32_t base = static_cast<uint32_t>(vertices.size());

    // Each face shows the whole texture, upright when seen from outside.
    const float uvs[4][2] = { { 0.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 0.0f }, { 0.0f, 0.0f } };

    for (int corner = 0; corner < 4; ++corner)
    {
        MeshVertex vertex{};
        std::memcpy(vertex.Position, corners[corner], sizeof(vertex.Position));
        std::memcpy(vertex.Normal, normal, sizeof(vertex.Normal));
        vertex.UV[0] = uvs[corner][0];
        vertex.UV[1] = uvs[corner][1];
        vertices.push_back(vertex);
    }

    const uint32_t quad[6] = { base, base + 1, base + 2, base, base + 2, base + 3 };
    indices.insert(indices.end(), quad, quad + 6);
}

// A sphere split at its equator and pulled apart by `stretch`, which is a
// capsule when stretch is positive and a sphere when it is zero. The texture
// wraps once around the middle and pinches at the poles.
void BuildRoundMesh(std::vector<MeshVertex>& vertices, std::vector<uint32_t>& indices, int rings, int segments,
                    float stretch)
{
    constexpr float Radius = 0.5f;

    for (int ring = 0; ring <= rings; ++ring)
    {
        const float v = static_cast<float>(ring) / static_cast<float>(rings);
        const float phi = v * Pi;
        const float offset = (phi <= Pi * 0.5f) ? stretch : -stretch;

        for (int segment = 0; segment <= segments; ++segment)
        {
            const float u = static_cast<float>(segment) / static_cast<float>(segments);
            const float theta = u * 2.0f * Pi;

            const float nx = std::sin(phi) * std::cos(theta);
            const float ny = std::cos(phi);
            const float nz = std::sin(phi) * std::sin(theta);

            MeshVertex vertex{};
            vertex.Position[0] = nx * Radius;
            vertex.Position[1] = ny * Radius + offset;
            vertex.Position[2] = nz * Radius;
            vertex.Normal[0] = nx;
            vertex.Normal[1] = ny;
            vertex.Normal[2] = nz;
            vertex.UV[0] = 1.0f - u;
            vertex.UV[1] = v;
            vertices.push_back(vertex);
        }
    }

    // Counter-clockwise seen from outside, which the pipelines treat as the
    // front face. Rings run down from the north pole and segments run
    // from +X toward +Z, so the next segment along a ring comes before the
    // ring below.
    const int stride = segments + 1;
    for (int ring = 0; ring < rings; ++ring)
    {
        for (int segment = 0; segment < segments; ++segment)
        {
            const uint32_t a = static_cast<uint32_t>(ring * stride + segment);
            const uint32_t b = static_cast<uint32_t>(a + stride);

            indices.push_back(a);
            indices.push_back(a + 1);
            indices.push_back(b);

            indices.push_back(a + 1);
            indices.push_back(b + 1);
            indices.push_back(b);
        }
    }
}

} // namespace

// A unit cube centered on the origin, with one normal per face so the edges
// stay sharp.
void BuildBoxMesh(std::vector<MeshVertex>& vertices, std::vector<uint32_t>& indices)
{
    const float h = 0.5f;

    const float front[4][3] = { { -h, -h, h }, { h, -h, h }, { h, h, h }, { -h, h, h } };
    const float back[4][3] = { { h, -h, -h }, { -h, -h, -h }, { -h, h, -h }, { h, h, -h } };
    const float right[4][3] = { { h, -h, h }, { h, -h, -h }, { h, h, -h }, { h, h, h } };
    const float left[4][3] = { { -h, -h, -h }, { -h, -h, h }, { -h, h, h }, { -h, h, -h } };
    const float top[4][3] = { { -h, h, h }, { h, h, h }, { h, h, -h }, { -h, h, -h } };
    const float bottom[4][3] = { { -h, -h, -h }, { h, -h, -h }, { h, -h, h }, { -h, -h, h } };

    const float normalFront[3] = { 0, 0, 1 };
    const float normalBack[3] = { 0, 0, -1 };
    const float normalRight[3] = { 1, 0, 0 };
    const float normalLeft[3] = { -1, 0, 0 };
    const float normalTop[3] = { 0, 1, 0 };
    const float normalBottom[3] = { 0, -1, 0 };

    AddQuad(vertices, indices, front, normalFront);
    AddQuad(vertices, indices, back, normalBack);
    AddQuad(vertices, indices, right, normalRight);
    AddQuad(vertices, indices, left, normalLeft);
    AddQuad(vertices, indices, top, normalTop);
    AddQuad(vertices, indices, bottom, normalBottom);
}

// A sphere of radius 0.5, so scaling by the diameter gives the right size.
// Three densities: the full one for close objects and two simpler ones for
// distant ones.
void BuildSphereMesh(std::vector<MeshVertex>& vertices, std::vector<uint32_t>& indices)
{
    BuildSphereMeshAt(vertices, indices, 0);
}

void BuildSphereMeshAt(std::vector<MeshVertex>& vertices, std::vector<uint32_t>& indices, int detail)
{
    switch (detail)
    {
        case 2: BuildRoundMesh(vertices, indices, 6, 8, 0.0f); break;
        case 1: BuildRoundMesh(vertices, indices, 10, 14, 0.0f); break;
        default: BuildRoundMesh(vertices, indices, 18, 28, 0.0f); break;
    }
}

// A capsule of radius 0.5 whose caps sit `stretch` above and below the
// centre. Scaled uniformly by the capsule's diameter it is exactly the capsule
// the physics sees: hemispherical caps survive only uniform scale, so each
// proportion of cylinder to radius gets a mesh of its own.
void BuildCapsuleMesh(std::vector<MeshVertex>& vertices, std::vector<uint32_t>& indices, float stretch)
{
    BuildCapsuleMeshAt(vertices, indices, stretch, 0);
}

void BuildCapsuleMeshAt(std::vector<MeshVertex>& vertices, std::vector<uint32_t>& indices,
                        float stretch, int detail)
{
    switch (detail)
    {
        case 2: BuildRoundMesh(vertices, indices, 6, 8, stretch); break;
        case 1: BuildRoundMesh(vertices, indices, 10, 12, stretch); break;
        default: BuildRoundMesh(vertices, indices, 14, 24, stretch); break;
    }
}

// A unit square facing +Z, the shape a sprite is drawn with. (0, 0) in texture
// space is the image's top-left corner, as image files store it.
void BuildQuadMesh(std::vector<MeshVertex>& vertices, std::vector<uint32_t>& indices)
{
    const float h = 0.5f;
    const float corners[4][3] = { { -h, -h, 0.0f }, { h, -h, 0.0f }, { h, h, 0.0f }, { -h, h, 0.0f } };
    const float normal[3] = { 0, 0, 1 };
    AddQuad(vertices, indices, corners, normal);
}

bool UploadMesh(SDL_GPUDevice* device, const void* vertices, size_t vertexCount,
                const std::vector<uint32_t>& indices, MeshBuffers& outMesh)
{
    const size_t vertexBytes = vertexCount * sizeof(MeshVertex);
    const size_t indexBytes = indices.size() * sizeof(uint32_t);

    if (vertexBytes == 0 || indexBytes == 0)
    {
        return false;
    }

    SDL_GPUBufferCreateInfo vertexInfo{};
    vertexInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vertexInfo.size = static_cast<uint32_t>(vertexBytes);
    outMesh.Vertices = SDL_CreateGPUBuffer(device, &vertexInfo);

    SDL_GPUBufferCreateInfo indexInfo{};
    indexInfo.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    indexInfo.size = static_cast<uint32_t>(indexBytes);
    outMesh.Indices = SDL_CreateGPUBuffer(device, &indexInfo);

    if (outMesh.Vertices == nullptr || outMesh.Indices == nullptr)
    {
        LogMessage(LogLevel::Error, "render", "Could not allocate mesh buffers: %s", SDL_GetError());
        ReleaseMesh(device, outMesh);
        return false;
    }

    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = static_cast<uint32_t>(vertexBytes + indexBytes);

    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device, &transferInfo);
    if (transfer == nullptr)
    {
        ReleaseMesh(device, outMesh);
        return false;
    }

    void* mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
    if (mapped == nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        ReleaseMesh(device, outMesh);
        return false;
    }

    std::memcpy(mapped, vertices, vertexBytes);
    std::memcpy(static_cast<unsigned char*>(mapped) + vertexBytes, indices.data(), indexBytes);
    SDL_UnmapGPUTransferBuffer(device, transfer);

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);

    SDL_GPUTransferBufferLocation source{};
    source.transfer_buffer = transfer;

    SDL_GPUBufferRegion vertexRegion{};
    vertexRegion.buffer = outMesh.Vertices;
    vertexRegion.size = static_cast<uint32_t>(vertexBytes);
    SDL_UploadToGPUBuffer(copyPass, &source, &vertexRegion, false);

    source.offset = static_cast<uint32_t>(vertexBytes);

    SDL_GPUBufferRegion indexRegion{};
    indexRegion.buffer = outMesh.Indices;
    indexRegion.size = static_cast<uint32_t>(indexBytes);
    SDL_UploadToGPUBuffer(copyPass, &source, &indexRegion, false);

    SDL_EndGPUCopyPass(copyPass);
    SDL_SubmitGPUCommandBuffer(commandBuffer);
    SDL_ReleaseGPUTransferBuffer(device, transfer);

    outMesh.IndexCount = static_cast<uint32_t>(indices.size());
    outMesh.Serial = g_NextMeshSerial.fetch_add(1);
    return true;
}

SDL_GPUBuffer* UploadStorage(SDL_GPUDevice* device, const void* data, size_t bytes)
{
    if (device == nullptr || data == nullptr || bytes == 0 || bytes > 0xFFFFFFFFu)
    {
        return nullptr;
    }

    SDL_GPUBufferCreateInfo info{};
    info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    info.size = static_cast<uint32_t>(bytes);
    SDL_GPUBuffer* buffer = SDL_CreateGPUBuffer(device, &info);
    if (buffer == nullptr)
    {
        LogMessage(LogLevel::Error, "render", "Could not allocate a storage buffer: %s", SDL_GetError());
        return nullptr;
    }

    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = static_cast<uint32_t>(bytes);
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device, &transferInfo);
    void* mapped = transfer != nullptr ? SDL_MapGPUTransferBuffer(device, transfer, false) : nullptr;
    if (mapped == nullptr)
    {
        if (transfer != nullptr)
        {
            SDL_ReleaseGPUTransferBuffer(device, transfer);
        }
        SDL_ReleaseGPUBuffer(device, buffer);
        return nullptr;
    }

    std::memcpy(mapped, data, bytes);
    SDL_UnmapGPUTransferBuffer(device, transfer);

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);

    SDL_GPUTransferBufferLocation source{};
    source.transfer_buffer = transfer;
    SDL_GPUBufferRegion region{};
    region.buffer = buffer;
    region.size = static_cast<uint32_t>(bytes);
    SDL_UploadToGPUBuffer(copyPass, &source, &region, false);

    SDL_EndGPUCopyPass(copyPass);
    SDL_SubmitGPUCommandBuffer(commandBuffer);
    SDL_ReleaseGPUTransferBuffer(device, transfer);
    return buffer;
}

void ReleaseMesh(SDL_GPUDevice* device, MeshBuffers& mesh)
{
    if (mesh.Vertices != nullptr)
    {
        SDL_ReleaseGPUBuffer(device, mesh.Vertices);
    }
    if (mesh.Indices != nullptr)
    {
        SDL_ReleaseGPUBuffer(device, mesh.Indices);
    }
    if (mesh.MorphDeltas != nullptr)
    {
        SDL_ReleaseGPUBuffer(device, mesh.MorphDeltas);
    }
    mesh = MeshBuffers{};
}

SDL_GPUShader* CreateShader(SDL_GPUDevice* device, SDL_GPUShaderStage stage, const char* entryPoint,
                            const ShaderBytecode& bytecode, uint32_t samplers, uint32_t storageBuffers,
                            uint32_t uniformBuffers)
{
    const SDL_GPUShaderFormat format = DeviceShaderFormat(device);
    const void* code = nullptr;
    size_t codeSize = 0;
    if (!PickShaderCode(bytecode, format, code, codeSize))
    {
        LogMessage(LogLevel::Error, "render",
                   "The %s shader was not compiled to %s, which this device's backend takes.", entryPoint,
                   ShaderFormatName(format));
        return nullptr;
    }

    SDL_GPUShaderCreateInfo info{};
    info.code = static_cast<const Uint8*>(code);
    info.code_size = codeSize;
    info.entrypoint = entryPoint;
    info.format = format;
    info.stage = stage;
    info.num_samplers = samplers;
    info.num_storage_textures = 0;
    info.num_storage_buffers = storageBuffers;
    info.num_uniform_buffers = uniformBuffers;

    SDL_GPUShader* shader = SDL_CreateGPUShader(device, &info);
    if (shader == nullptr)
    {
        LogMessage(LogLevel::Error, "render", "Could not create the %s shader: %s", entryPoint, SDL_GetError());
    }
    return shader;
}

SDL_GPUComputePipeline* CreateComputePipeline(SDL_GPUDevice* device, const char* entryPoint,
                                              const ShaderBytecode& bytecode, uint32_t samplers,
                                              uint32_t readOnlyBuffers, uint32_t writtenTextures,
                                              uint32_t uniformBuffers)
{
    const SDL_GPUShaderFormat format = DeviceShaderFormat(device);
    const void* code = nullptr;
    size_t codeSize = 0;
    if (!PickShaderCode(bytecode, format, code, codeSize))
    {
        LogMessage(LogLevel::Error, "render",
                   "The %s shader was not compiled to %s, which this device's backend takes.", entryPoint,
                   ShaderFormatName(format));
        return nullptr;
    }

    SDL_GPUComputePipelineCreateInfo info{};
    info.code = static_cast<const Uint8*>(code);
    info.code_size = codeSize;
    info.entrypoint = entryPoint;
    info.format = format;
    info.num_samplers = samplers;
    info.num_readonly_storage_buffers = readOnlyBuffers;
    info.num_readwrite_storage_textures = writtenTextures;
    info.num_uniform_buffers = uniformBuffers;
    info.threadcount_x = 8;
    info.threadcount_y = 8;
    info.threadcount_z = 1;

    SDL_GPUComputePipeline* pipeline = SDL_CreateGPUComputePipeline(device, &info);
    if (pipeline == nullptr)
    {
        LogMessage(LogLevel::Warning, "render", "Could not create the %s compute pipeline: %s", entryPoint,
                   SDL_GetError());
    }
    return pipeline;
}

Mat4 InvertMatrix(const Mat4& matrix)
{
    // Cofactors, then the transposed adjugate over the determinant.
    const float* m = matrix.M;
    float inverse[16];

    inverse[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
                 m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inverse[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
                 m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inverse[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
                 m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inverse[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
                  m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inverse[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
                 m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inverse[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
                 m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inverse[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
                 m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inverse[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
                  m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inverse[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
                 m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inverse[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
                 m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inverse[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
                  m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inverse[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
                  m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inverse[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
                 m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inverse[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
                 m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inverse[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
                  m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inverse[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
                  m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    const float determinant = m[0] * inverse[0] + m[1] * inverse[4] + m[2] * inverse[8] + m[3] * inverse[12];
    Mat4 result;
    if (std::fabs(determinant) < 1e-30f)
    {
        return result;
    }
    const float scale = 1.0f / determinant;
    for (int index = 0; index < 16; ++index)
    {
        result.M[index] = inverse[index] * scale;
    }
    return result;
}

SDL_GPUTexture* CreateTarget(SDL_GPUDevice* device, SDL_GPUTextureFormat format, int width, int height,
                             SDL_GPUSampleCount samples, SDL_GPUTextureUsageFlags usage, uint32_t levels)
{
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = usage;
    info.width = static_cast<uint32_t>(width);
    info.height = static_cast<uint32_t>(height);
    info.layer_count_or_depth = 1;
    info.num_levels = std::max<uint32_t>(levels, 1);
    info.sample_count = samples;

    SDL_GPUTexture* texture = SDL_CreateGPUTexture(device, &info);
    if (texture == nullptr)
    {
        LogMessage(LogLevel::Error, "render", "Could not create a %dx%d render target: %s", width, height,
                   SDL_GetError());
    }
    return texture;
}

Frustum Frustum::FromViewProjection(const Mat4& viewProjection)
{
    // Gribb and Hartmann: each plane is a sum or difference of two rows of the
    // combined matrix. Storage is column-major, so row r column c is M[c*4+r].
    auto Row = [&](int r, int c) { return viewProjection.M[c * 4 + r]; };

    Frustum frustum;

    for (int axis = 0; axis < 3; ++axis)
    {
        // Left/right, bottom/top, then near/far.
        for (int side = 0; side < 2; ++side)
        {
            const int index = axis * 2 + side;
            const float sign = (side == 0) ? 1.0f : -1.0f;

            for (int component = 0; component < 4; ++component)
            {
                // The near plane is the row on its own, because clip depth runs
                // 0..1 rather than -1..1.
                const float base = (axis == 2 && side == 0) ? 0.0f : Row(3, component);
                frustum.Planes[index][component] = base + sign * Row(axis, component);
            }
        }
    }

    for (auto& plane : frustum.Planes)
    {
        const float length = std::sqrt(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
        if (length > 1e-12f)
        {
            const float inverse = 1.0f / length;
            plane[0] *= inverse;
            plane[1] *= inverse;
            plane[2] *= inverse;
            plane[3] *= inverse;
        }
    }

    return frustum;
}

bool Frustum::ContainsSphere(const Vec3& centre, float radius) const
{
    for (const auto& plane : Planes)
    {
        const float distance = plane[0] * centre.X + plane[1] * centre.Y + plane[2] * centre.Z + plane[3];

        // Entirely outside one plane means entirely outside the frustum.
        if (distance < -radius)
        {
            return false;
        }
    }
    return true;
}

} // namespace ludifex::detail
