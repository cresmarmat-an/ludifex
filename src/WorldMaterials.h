// World materials: custom fragment shaders for actors.
// Not installed and not part of the public API.
//
// The store keeps bytecode and uniforms, not pipelines. A pipeline depends on
// the sample count of the target it draws into, and two worlds may render at
// different anti-aliasing settings, so each renderer builds and caches its own
// pipelines from the bytecode kept here. A hot reload bumps Version, and every
// renderer notices and rebuilds.
//
// Bytecode is kept per format (DXIL, SPIR-V, MSL) because a material can be
// created before the device that draws it exists, and so before the backend
// is known.

#pragma once

#include "Internal.h"

#include <SDL3/SDL_gpu.h>

#include <string>
#include <vector>

namespace ludifex::detail
{

// Mirrors MaterialUniforms in world_material.hlsli. The layouts must agree.
struct MaterialUniformBlock
{
    float Params[8][4] = {};
    float Time[4] = {};
};

struct WorldMaterialRecord
{
    uint32_t Generation = 0;
    bool Alive = false;

    // The shader in each format it has, indexed DXIL, SPIR-V, MSL: given as
    // bytecode, or compiled from SourcePath the first time a device taking
    // that format asks for it.
    std::vector<unsigned char> Code[3];

    // A format that is unavailable (a compile error, or bytecode the build did
    // not make), remembered until the source changes, so it is reported once
    // instead of every frame.
    bool Unavailable[3] = {};

    std::string EntryPoint;

    // Increments on every successful reload so renderers know their cached
    // pipeline is out of date.
    uint32_t Version = 0;

    MaterialUniformBlock Uniforms;
    std::vector<std::string> UniformNames;

    std::string SourcePath;
    bool HotReload = false;
    int64_t SourceTimestamp = 0;

    // Blended in the transparent pass rather than drawn opaque with
    // alpha-to-coverage.
    bool Transparent = false;
};

class WorldMaterialStore
{
public:
    MaterialId Create(const MaterialDesc& desc);
    void Destroy(MaterialId id);

    WorldMaterialRecord* Resolve(const MaterialId& id);

    // The material's shader in the format a device takes, compiled from
    // source first when that format has not been needed before. Empty when it
    // cannot be had; the reason has gone to the log. The bytes stay the
    // record's.
    ShaderBytecode CodeFor(WorldMaterialRecord& record, SDL_GPUShaderFormat format);

    void SetUniform(const MaterialId& id, const std::string& name, float x, float y, float z, float w);

    // The Params[] slot a uniform name occupies, or -1 when the material is
    // gone or never declared it.
    int FindSlot(const MaterialId& id, const std::string& name);

    // Advances the clock and recompiles changed sources. Throttled internally,
    // so calling it every frame is cheap.
    void Update();

    size_t GetCapacity() const { return m_Materials.size(); }
    WorldMaterialRecord* At(size_t index) { return index < m_Materials.size() ? &m_Materials[index] : nullptr; }

private:
    bool Compile(const std::string& sourcePath, const std::string& entryPoint, SDL_GPUShaderFormat format,
                 std::vector<unsigned char>& outBytecode, std::string& outError) const;

    std::vector<WorldMaterialRecord> m_Materials;
    std::vector<uint32_t> m_Free;

    uint64_t m_StartTicks = 0;
    uint64_t m_LastTicks = 0;
    uint64_t m_LastReloadCheck = 0;
};

WorldMaterialStore& GetWorldMaterials();

} // namespace ludifex::detail
