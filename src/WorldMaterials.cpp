#include "WorldMaterials.h"

#include "Assets.h"
#include "Backend.h"
#include "Render3D.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <filesystem>

namespace ludifex
{
namespace detail
{
namespace
{

// Checking the file system every frame would be wasteful; a few times a second
// is indistinguishable while editing a shader.
constexpr uint64_t ReloadIntervalMilliseconds = 250;

constexpr SDL_GPUShaderFormat SlotFormats[3] = { SDL_GPU_SHADERFORMAT_DXIL, SDL_GPU_SHADERFORMAT_SPIRV,
                                                 SDL_GPU_SHADERFORMAT_MSL };

int FormatSlot(SDL_GPUShaderFormat format)
{
    for (int slot = 0; slot < 3; ++slot)
    {
        if (SlotFormats[slot] == format)
        {
            return slot;
        }
    }
    return -1;
}

int64_t GetTimestamp(const std::string& path)
{
    std::error_code error;
    const auto time = std::filesystem::last_write_time(path, error);
    if (error)
    {
        return 0;
    }
    return static_cast<int64_t>(time.time_since_epoch().count());
}

void CopyCode(const void* code, size_t size, std::vector<unsigned char>& out)
{
    if (code != nullptr && size > 0)
    {
        const auto* bytes = static_cast<const unsigned char*>(code);
        out.assign(bytes, bytes + size);
    }
}

} // namespace

WorldMaterialStore& GetWorldMaterials()
{
    static WorldMaterialStore store;
    return store;
}

bool WorldMaterialStore::Compile(const std::string& sourcePath, const std::string& entryPoint,
                                 SDL_GPUShaderFormat format, std::vector<unsigned char>& outBytecode,
                                 std::string& outError) const
{
    return CompileMaterialSource(sourcePath, entryPoint, format, outBytecode, outError);
}

ShaderBytecode WorldMaterialStore::CodeFor(WorldMaterialRecord& record, SDL_GPUShaderFormat format)
{
    const int slot = FormatSlot(format);
    if (slot < 0 || record.Unavailable[slot])
    {
        return ShaderBytecode{};
    }

    std::vector<unsigned char>& code = record.Code[slot];
    if (code.empty())
    {
        record.Unavailable[slot] = true;
        if (record.SourcePath.empty())
        {
            LogMessage(LogLevel::Error, "material",
                       "A material has no %s bytecode, the format this device's backend takes, so its actors draw "
                       "with the default shader. ludifex_add_material compiles to %s when the build has the "
                       "compiler for it; see the documentation's page on backends.",
                       ShaderFormatName(format), ShaderFormatName(format));
            return ShaderBytecode{};
        }

        std::string error;
        if (!Compile(record.SourcePath, record.EntryPoint, format, code, error))
        {
            LogMessage(LogLevel::Error, "material",
                       "Could not compile \"%s\" to %s; its actors draw with the default shader until it is "
                       "fixed:\n%s",
                       record.SourcePath.c_str(), ShaderFormatName(format), error.c_str());
            code.clear();
            return ShaderBytecode{};
        }
        record.Unavailable[slot] = false;
    }

    ShaderBytecode bytecode;
    switch (format)
    {
    case SDL_GPU_SHADERFORMAT_DXIL:
        bytecode.Dxil = code.data();
        bytecode.DxilSize = code.size();
        break;
    case SDL_GPU_SHADERFORMAT_SPIRV:
        bytecode.Spirv = code.data();
        bytecode.SpirvSize = code.size();
        break;
    default:
        bytecode.Msl = code.data();
        bytecode.MslSize = code.size();
        break;
    }
    return bytecode;
}

MaterialId WorldMaterialStore::Create(const MaterialDesc& desc)
{
    MaterialId id;

    if (desc.Uniforms.size() > 8)
    {
        LogMessage(LogLevel::Error, "material",
                   "A material may declare at most 8 uniforms; %zu were given.", desc.Uniforms.size());
        return id;
    }

    std::vector<unsigned char> code[3];
    const bool fromFile = desc.Bytecode.IsEmpty() && !desc.ShaderPath.empty();

    // The shader is found through the asset roots like any other file, and the
    // resolved path is what hot reload then watches.
    std::string shaderPath;
    if (fromFile)
    {
        shaderPath = ResolveAsset(desc.ShaderPath, "material");
        if (shaderPath.empty())
        {
            return id;
        }

        // Compiled now, for the format the device most likely takes, so a
        // mistake is reported where the material is made. A device that turns
        // out to take another format compiles its own when it first draws.
        const SDL_GPUShaderFormat format = ExpectedShaderFormat();
        const int slot = FormatSlot(format);
        std::string error;
        if (slot < 0 || !Compile(shaderPath, desc.EntryPoint, format, code[slot], error))
        {
            LogMessage(LogLevel::Error, "material", "Could not compile \"%s\":\n%s",
                       shaderPath.c_str(), slot < 0 ? "no backend is available to compile it for" : error.c_str());
            return id;
        }
    }
    else if (!desc.Bytecode.IsEmpty())
    {
        CopyCode(desc.Bytecode.Dxil, desc.Bytecode.DxilSize, code[0]);
        CopyCode(desc.Bytecode.Spirv, desc.Bytecode.SpirvSize, code[1]);
        CopyCode(desc.Bytecode.Msl, desc.Bytecode.MslSize, code[2]);
    }
    else
    {
        LogMessage(LogLevel::Error, "material", "CreateMaterial needs either ShaderPath or Bytecode.");
        return id;
    }

    uint32_t index;
    if (!m_Free.empty())
    {
        index = m_Free.back();
        m_Free.pop_back();
    }
    else
    {
        index = static_cast<uint32_t>(m_Materials.size());
        m_Materials.emplace_back();
    }

    WorldMaterialRecord& record = m_Materials[index];
    ++record.Generation;
    if (record.Generation == 0)
    {
        record.Generation = 1;
    }

    record.Alive = true;
    for (int slot = 0; slot < 3; ++slot)
    {
        record.Code[slot] = std::move(code[slot]);
        record.Unavailable[slot] = false;
    }
    record.EntryPoint = desc.EntryPoint;
    ++record.Version;
    record.SourcePath = shaderPath;
    record.HotReload = fromFile && desc.HotReload;
    record.SourceTimestamp = fromFile ? GetTimestamp(shaderPath) : 0;
    record.Transparent = desc.Transparent;

    record.Uniforms = MaterialUniformBlock{};
    record.UniformNames.clear();

    for (size_t slot = 0; slot < desc.Uniforms.size(); ++slot)
    {
        const MaterialUniform& uniform = desc.Uniforms[slot];
        record.UniformNames.push_back(uniform.Name);

        // Colours arrive in sRGB, like every colour in the API, and are stored
        // the way the shader wants them: linear.
        const Color value = uniform.IsColor ? ToLinear(Color{ uniform.X, uniform.Y, uniform.Z, uniform.W })
                                            : Color{ uniform.X, uniform.Y, uniform.Z, uniform.W };
        record.Uniforms.Params[slot][0] = value.R;
        record.Uniforms.Params[slot][1] = value.G;
        record.Uniforms.Params[slot][2] = value.B;
        record.Uniforms.Params[slot][3] = value.A;
    }

    if (fromFile)
    {
        LogMessage(LogLevel::Info, "material", "Compiled \"%s\"%s.", shaderPath.c_str(),
                   record.HotReload ? " with hot reload" : "");
    }

    id.Index = index;
    id.Generation = record.Generation;
    return id;
}

void WorldMaterialStore::Destroy(MaterialId id)
{
    WorldMaterialRecord* record = Resolve(id);
    if (record == nullptr)
    {
        return;
    }

    record->Alive = false;
    for (std::vector<unsigned char>& code : record->Code)
    {
        code.clear();
    }
    record->UniformNames.clear();
    record->SourcePath.clear();

    // Renderers compare generations, so bumping it here is what tells them to
    // drop the pipeline they built for this material.
    ++record->Generation;
    if (record->Generation == 0)
    {
        record->Generation = 1;
    }

    m_Free.push_back(id.Index);
}

WorldMaterialRecord* WorldMaterialStore::Resolve(const MaterialId& id)
{
    if (!id.IsValid() || id.Index >= m_Materials.size())
    {
        return nullptr;
    }

    WorldMaterialRecord& record = m_Materials[id.Index];
    if (!record.Alive || record.Generation != id.Generation)
    {
        return nullptr;
    }
    return &record;
}

int WorldMaterialStore::FindSlot(const MaterialId& id, const std::string& name)
{
    WorldMaterialRecord* record = Resolve(id);
    if (record == nullptr)
    {
        return -1;
    }

    const auto slot = std::find(record->UniformNames.begin(), record->UniformNames.end(), name);
    if (slot == record->UniformNames.end())
    {
        return -1;
    }
    return static_cast<int>(std::distance(record->UniformNames.begin(), slot));
}

void WorldMaterialStore::SetUniform(const MaterialId& id, const std::string& name, float x, float y,
                                    float z, float w)
{
    WorldMaterialRecord* record = Resolve(id);
    if (record == nullptr)
    {
        LogMessage(LogLevel::Warning, "material",
                   "SetMaterialUniform was called on a material that no longer exists.");
        return;
    }

    const auto slot = std::find(record->UniformNames.begin(), record->UniformNames.end(), name);
    if (slot == record->UniformNames.end())
    {
        LogMessage(LogLevel::Warning, "material",
                   "\"%s\" is not a uniform of this material. Declare it in MaterialDesc::Uniforms.",
                   name.c_str());
        return;
    }

    const size_t index = static_cast<size_t>(std::distance(record->UniformNames.begin(), slot));
    record->Uniforms.Params[index][0] = x;
    record->Uniforms.Params[index][1] = y;
    record->Uniforms.Params[index][2] = z;
    record->Uniforms.Params[index][3] = w;
}

void WorldMaterialStore::Update()
{
    const uint64_t now = SDL_GetTicks();
    if (m_StartTicks == 0)
    {
        m_StartTicks = now;
        m_LastTicks = now;
    }

    const float seconds = static_cast<float>(now - m_StartTicks) / 1000.0f;
    const float delta = static_cast<float>(now - m_LastTicks) / 1000.0f;
    m_LastTicks = now;

    for (WorldMaterialRecord& record : m_Materials)
    {
        if (record.Alive)
        {
            record.Uniforms.Time[0] = seconds;
            record.Uniforms.Time[1] = delta;
        }
    }

    if (now - m_LastReloadCheck < ReloadIntervalMilliseconds)
    {
        return;
    }
    m_LastReloadCheck = now;

    for (WorldMaterialRecord& record : m_Materials)
    {
        if (!record.Alive || !record.HotReload || record.SourcePath.empty())
        {
            continue;
        }

        const int64_t timestamp = GetTimestamp(record.SourcePath);
        if (timestamp == 0 || timestamp == record.SourceTimestamp)
        {
            continue;
        }

        record.SourceTimestamp = timestamp;

        // Every format a device has asked for is rebuilt; one nothing has
        // drawn with yet waits until something does.
        bool reloaded = false;
        for (int slot = 0; slot < 3; ++slot)
        {
            if (record.Code[slot].empty() && !record.Unavailable[slot])
            {
                continue;
            }

            std::vector<unsigned char> compiled;
            std::string error;
            if (!Compile(record.SourcePath, record.EntryPoint, SlotFormats[slot], compiled, error))
            {
                // The previous bytecode stays, so a typo costs a diagnostic
                // rather than a black object.
                LogMessage(LogLevel::Error, "material", "Reload of \"%s\" failed:\n%s",
                           record.SourcePath.c_str(), error.c_str());
                continue;
            }

            record.Code[slot] = std::move(compiled);
            record.Unavailable[slot] = false;
            reloaded = true;
        }

        if (reloaded)
        {
            ++record.Version;
            LogMessage(LogLevel::Info, "material", "Reloaded \"%s\".", record.SourcePath.c_str());
        }
    }
}

} // namespace detail

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

MaterialId CreateMaterial(const MaterialDesc& desc)
{
    return detail::GetWorldMaterials().Create(desc);
}

void DestroyMaterial(MaterialId material)
{
    detail::GetWorldMaterials().Destroy(material);
}

void SetMaterialUniform(MaterialId material, const std::string& name, float x, float y, float z, float w)
{
    detail::GetWorldMaterials().SetUniform(material, name, x, y, z, w);
}

void SetMaterialUniform(MaterialId material, const std::string& name, Color color)
{
    const Color linear = detail::ToLinear(color);
    detail::GetWorldMaterials().SetUniform(material, name, linear.R, linear.G, linear.B, linear.A);
}

} // namespace ludifex
