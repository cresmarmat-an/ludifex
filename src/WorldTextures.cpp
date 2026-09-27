#include "WorldTextures.h"

#include "Assets.h"
#include "Images.h"

#include <algorithm>
#include <cstring>

namespace ludifex
{
namespace detail
{
namespace
{

SDL_GPUSampler* CreateSampler(SDL_GPUDevice* device, SDL_GPUSamplerAddressMode addressMode)
{
    SDL_GPUSamplerCreateInfo info{};
    info.min_filter = SDL_GPU_FILTER_LINEAR;
    info.mag_filter = SDL_GPU_FILTER_LINEAR;
    info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    info.address_mode_u = addressMode;
    info.address_mode_v = addressMode;
    info.address_mode_w = addressMode;

    // Level 2 of the anti-aliasing policy: trilinear with 8x anisotropy, so a
    // texture seen at a grazing angle stays sharp without shimmering.
    info.min_lod = 0.0f;
    info.max_lod = 1000.0f;
    info.enable_anisotropy = true;
    info.max_anisotropy = 8.0f;

    SDL_GPUSampler* sampler = SDL_CreateGPUSampler(device, &info);
    if (sampler == nullptr)
    {
        LogMessage(LogLevel::Error, "render", "Could not create a texture sampler: %s", SDL_GetError());
    }
    return sampler;
}

SDL_GPUTexture* UploadLevels(SDL_GPUDevice* device, int width, int height,
                             const std::vector<std::vector<uint8_t>>& levels,
                             TextureUsage usage = TextureUsage::Color)
{
    const uint32_t levelCount = static_cast<uint32_t>(levels.size());

    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;

    // Colour is stored as sRGB, so the sampler hands the shader linear light
    // and the filtering between texels happens in linear light too. Data is
    // stored as it is: a roughness of 0.5 must sample as 0.5.
    info.format = usage == TextureUsage::Color ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB
                                               : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = static_cast<uint32_t>(width);
    info.height = static_cast<uint32_t>(height);
    info.layer_count_or_depth = 1;
    info.num_levels = levelCount;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;

    SDL_GPUTexture* texture = SDL_CreateGPUTexture(device, &info);
    if (texture == nullptr)
    {
        LogMessage(LogLevel::Error, "render", "Could not create a texture: %s", SDL_GetError());
        return nullptr;
    }

    std::vector<uint32_t> offsets(levelCount);
    uint32_t total = 0;
    for (uint32_t level = 0; level < levelCount; ++level)
    {
        offsets[level] = total;
        total += static_cast<uint32_t>(levels[level].size());
    }

    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = total;

    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device, &transferInfo);
    if (transfer == nullptr)
    {
        SDL_ReleaseGPUTexture(device, texture);
        return nullptr;
    }

    auto* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device, transfer, false));
    if (mapped == nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        SDL_ReleaseGPUTexture(device, texture);
        return nullptr;
    }
    for (uint32_t level = 0; level < levelCount; ++level)
    {
        std::memcpy(mapped + offsets[level], levels[level].data(), levels[level].size());
    }
    SDL_UnmapGPUTransferBuffer(device, transfer);

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);

    for (uint32_t level = 0; level < levelCount; ++level)
    {
        const uint32_t levelWidth = std::max(1u, static_cast<uint32_t>(width) >> level);
        const uint32_t levelHeight = std::max(1u, static_cast<uint32_t>(height) >> level);

        SDL_GPUTextureTransferInfo source{};
        source.transfer_buffer = transfer;
        source.offset = offsets[level];
        source.pixels_per_row = levelWidth;
        source.rows_per_layer = levelHeight;

        SDL_GPUTextureRegion destination{};
        destination.texture = texture;
        destination.mip_level = level;
        destination.w = levelWidth;
        destination.h = levelHeight;
        destination.d = 1;

        SDL_UploadToGPUTexture(copyPass, &source, &destination, false);
    }

    SDL_EndGPUCopyPass(copyPass);
    SDL_SubmitGPUCommandBuffer(commandBuffer);
    SDL_ReleaseGPUTransferBuffer(device, transfer);

    return texture;
}

std::vector<std::vector<uint8_t>> Decode(const std::string& path, int& outWidth, int& outHeight,
                                         TextureUsage usage)
{
    std::vector<uint8_t> bytes;
    std::string resolved;
    if (!LoadAssetFile(path, "texture", bytes, &resolved))
    {
        return {};
    }

    DecodedImage image;
    std::string error;
    if (!DecodeImage(bytes.data(), bytes.size(), image, error))
    {
        LogMessage(LogLevel::Error, "texture", "Could not decode \"%s\": %s", resolved.c_str(), error.c_str());
        return {};
    }

    outWidth = image.Width;
    outHeight = image.Height;
    return BuildMipChain(image, usage);
}

// Caches are keyed by name and by usage together: the same file loaded as
// colour and as data is two textures, stored two ways.
std::string UsageKey(const std::string& name, TextureUsage usage)
{
    switch (usage)
    {
        case TextureUsage::Color: return name;
        case TextureUsage::Data: return name + "|data";
        case TextureUsage::Normal: return name + "|normal";
    }
    return name;
}

uint64_t UsageHash(uint64_t hash, TextureUsage usage)
{
    return hash == 0 ? 0 : hash ^ (static_cast<uint64_t>(usage) * 0x9E3779B97F4A7C15ull);
}

} // namespace

TextureId WorldTextureStore::Store(int width, int height, std::vector<std::vector<uint8_t>> levels,
                                   const std::string& path)
{
    uint32_t index;
    if (!m_Free.empty())
    {
        index = m_Free.back();
        m_Free.pop_back();
    }
    else
    {
        index = static_cast<uint32_t>(m_Textures.size());
        m_Textures.emplace_back();
    }

    WorldTextureRecord& record = m_Textures[index];
    ++record.Generation;
    if (record.Generation == 0)
    {
        record.Generation = 1;
    }

    record.Alive = true;
    record.Width = width;
    record.Height = height;
    record.Levels = std::move(levels);
    record.Path = path;
    record.Texture = nullptr;
    record.Device = nullptr;

    return TextureId{ index, record.Generation };
}

TextureId WorldTextureStore::GetMissing()
{
    if (Resolve(m_Missing) == nullptr)
    {
        const DecodedImage checker = MakeCheckerboard();
        m_Missing = Store(checker.Width, checker.Height, BuildMipChain(checker), std::string());
    }
    return m_Missing;
}

TextureId WorldTextureStore::Load(const std::string& path, TextureUsage usage)
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);

    const std::string key = UsageKey(path, usage);
    const auto cached = m_ByPath.find(key);
    if (cached != m_ByPath.end() && Resolve(cached->second) != nullptr)
    {
        return cached->second;
    }

    int width = 0;
    int height = 0;
    std::vector<std::vector<uint8_t>> levels = Decode(path, width, height, usage);
    if (levels.empty())
    {
        // A visible placeholder, so the failure is obvious.
        return GetMissing();
    }

    const TextureId id = Store(width, height, std::move(levels), path);
    if (WorldTextureRecord* stored = Resolve(id))
    {
        stored->Usage = usage;
    }
    m_ByPath[key] = id;

    // Watched from the resolved path, and hashed, so the same image under two
    // names is decoded once and a changed one is noticed.
    const std::string resolved = ResolveAsset(path, "texture", LogLevel::Warning);
    if (!resolved.empty())
    {
        if (WorldTextureRecord* record = Resolve(id))
        {
            BeginWatching(record->Watch, resolved);

            // Not shared while hot reload is on, for the same reason models
            // are not: a record shared between two names can watch only one
            // of them, and editing the other would change nothing.
            const uint64_t hash = UsageHash(record->Watch.Hash, usage);
            const auto shared = IsHotReloadEnabled() ? m_ByHash.end() : m_ByHash.find(hash);
            if (shared != m_ByHash.end() && Resolve(shared->second) != nullptr &&
                shared->second != id)
            {
                // Already decoded under another name. The record just made is
                // released rather than kept: one set of pixels, one upload.
                LogMessage(LogLevel::Info, "texture", "\"%s\" is an image already loaded; sharing it.",
                           path.c_str());
                const TextureId existing = shared->second;
                Destroy(id);
                m_ByPath[key] = existing;
                return existing;
            }

            m_ByHash[hash] = id;
        }
    }

    LogMessage(LogLevel::Info, "texture", "Loaded \"%s\" (%dx%d).", path.c_str(), width, height);
    return id;
}

int WorldTextureStore::ReloadChanged()
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);

    int reloaded = 0;
    for (WorldTextureRecord& record : m_Textures)
    {
        if (!record.Alive || record.Watch.Path.empty() || !HasFileChanged(record.Watch))
        {
            continue;
        }

        int width = 0;
        int height = 0;
        std::vector<std::vector<uint8_t>> levels = Decode(record.Path, width, height, record.Usage);
        if (levels.empty())
        {
            LogMessage(LogLevel::Error, "texture",
                       "Reload of \"%s\" failed; the image on screen is the one that worked.",
                       record.Path.c_str());
            continue;
        }

        // The GPU texture goes, so the next draw uploads the new pixels. It
        // may be a different size now, which is why the whole texture is
        // replaced rather than written over.
        if (record.Texture != nullptr && record.Device != nullptr)
        {
            SDL_ReleaseGPUTexture(record.Device, record.Texture);
        }
        record.Texture = nullptr;
        record.Device = nullptr;

        record.Width = width;
        record.Height = height;
        record.Levels = std::move(levels);
        ++reloaded;

        LogMessage(LogLevel::Info, "texture", "Reloaded \"%s\" (%dx%d).", record.Path.c_str(), width,
                   height);
    }

    return reloaded;
}

TextureId WorldTextureStore::LoadEncoded(const uint8_t* bytes, size_t size, const std::string& name,
                                          TextureUsage usage)
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);

    const std::string key = UsageKey(name, usage);
    const auto cached = m_ByPath.find(key);
    if (cached != m_ByPath.end() && Resolve(cached->second) != nullptr)
    {
        return cached->second;
    }

    // An image embedded in a model has no file, but it still has contents:
    // two models carrying the same texture decode it once.
    const uint64_t hash = UsageHash(HashBytes(bytes, size), usage);
    if (hash != 0)
    {
        const auto shared = m_ByHash.find(hash);
        if (shared != m_ByHash.end() && Resolve(shared->second) != nullptr)
        {
            m_ByPath[key] = shared->second;
            return shared->second;
        }
    }

    DecodedImage image;
    std::string error;
    if (!DecodeImage(bytes, size, image, error))
    {
        LogMessage(LogLevel::Error, "texture", "Could not decode the image \"%s\": %s", key.c_str(),
                   error.c_str());
        return GetMissing();
    }

    // No file to decode it from again, so its pixels are kept.
    const TextureId id = Store(image.Width, image.Height, BuildMipChain(image, usage), std::string());
    if (WorldTextureRecord* stored = Resolve(id))
    {
        stored->Usage = usage;
    }
    m_ByPath[key] = id;
    if (hash != 0)
    {
        m_ByHash[hash] = id;
    }
    return id;
}

TextureId WorldTextureStore::Create(int width, int height, const void* rgbaPixels, TextureUsage usage)
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);

    if (width <= 0 || height <= 0 || rgbaPixels == nullptr)
    {
        LogMessage(LogLevel::Error, "texture", "CreateTexture was given invalid dimensions or no pixels.");
        return TextureId{};
    }

    DecodedImage image;
    image.Width = width;
    image.Height = height;
    const auto* bytes = static_cast<const uint8_t*>(rgbaPixels);
    image.Pixels.assign(bytes, bytes + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);

    const TextureId id = Store(width, height, BuildMipChain(image, usage), std::string());
    if (WorldTextureRecord* stored = Resolve(id))
    {
        stored->Usage = usage;
    }
    return id;
}

void WorldTextureStore::Destroy(TextureId id)
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);

    WorldTextureRecord* record = Resolve(id);
    if (record == nullptr || id == m_Missing)
    {
        return;
    }

    if (record->Texture != nullptr && record->Device != nullptr)
    {
        SDL_ReleaseGPUTexture(record->Device, record->Texture);
    }

    for (auto entry = m_ByPath.begin(); entry != m_ByPath.end(); ++entry)
    {
        if (entry->second == id)
        {
            m_ByPath.erase(entry);
            break;
        }
    }

    record->Alive = false;
    record->Texture = nullptr;
    record->Device = nullptr;
    record->Levels.clear();
    record->Path.clear();

    ++record->Generation;
    if (record->Generation == 0)
    {
        record->Generation = 1;
    }
    m_Free.push_back(id.Index);
}

WorldTextureRecord* WorldTextureStore::Resolve(const TextureId& id)
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);

    if (!id.IsValid() || id.Index >= m_Textures.size())
    {
        return nullptr;
    }
    WorldTextureRecord& record = m_Textures[id.Index];
    if (!record.Alive || record.Generation != id.Generation)
    {
        return nullptr;
    }
    return &record;
}

Vec2 WorldTextureStore::GetSize(const TextureId& id)
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);

    const WorldTextureRecord* record = Resolve(id);
    if (record == nullptr)
    {
        return Vec2{};
    }
    return Vec2{ static_cast<float>(record->Width), static_cast<float>(record->Height) };
}

bool WorldTextureStore::Upload(SDL_GPUDevice* device, WorldTextureRecord& record)
{
    // A texture uploaded to a device that has since been replaced is gone;
    // decode it again from its file if its pixels were let go.
    if (record.Levels.empty() && !record.Path.empty())
    {
        int width = 0;
        int height = 0;
        record.Levels = Decode(record.Path, width, height, record.Usage);
        if (record.Levels.empty())
        {
            return false;
        }
    }

    if (record.Levels.empty())
    {
        return false;
    }

    record.Texture = UploadLevels(device, record.Width, record.Height, record.Levels, record.Usage);
    record.Device = device;
    record.LevelCount = static_cast<int>(record.Levels.size());

    // Pixels with a file behind them can be decoded again; keeping them would
    // hold every texture in memory twice.
    if (record.Texture != nullptr && !record.Path.empty())
    {
        record.Levels.clear();
        record.Levels.shrink_to_fit();
    }

    return record.Texture != nullptr;
}

SDL_GPUTexture* WorldTextureStore::GetWhite(SDL_GPUDevice* device)
{
    if (m_White != nullptr && m_WhiteDevice == device)
    {
        return m_White;
    }

    const std::vector<std::vector<uint8_t>> levels{ { 255, 255, 255, 255 } };
    m_White = UploadLevels(device, 1, 1, levels);
    m_WhiteDevice = device;
    return m_White;
}

SDL_GPUTexture* WorldTextureStore::GetGpuTexture(SDL_GPUDevice* device, const TextureId& id, Fallback fallback)
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);

    WorldTextureRecord* record = Resolve(id);
    if (record == nullptr)
    {
        return GetFallback(device, fallback);
    }

    if (record->Texture == nullptr || record->Device != device)
    {
        if (!Upload(device, *record))
        {
            return GetFallback(device, fallback);
        }
    }
    return record->Texture;
}

SDL_GPUTexture* WorldTextureStore::GetFallback(SDL_GPUDevice* device, Fallback fallback)
{
    if (fallback == Fallback::White)
    {
        return GetWhite(device);
    }

    SDL_GPUTexture*& texture = fallback == Fallback::FlatNormal ? m_FlatNormal : m_Black;
    SDL_GPUDevice*& owner = fallback == Fallback::FlatNormal ? m_FlatNormalDevice : m_BlackDevice;
    if (texture != nullptr && owner == device)
    {
        return texture;
    }

    // A normal pointing straight out of the surface, and no light at all, each
    // one texel, stored as data so they sample exactly as written.
    const std::vector<std::vector<uint8_t>> levels{ fallback == Fallback::FlatNormal
                                                        ? std::vector<uint8_t>{ 128, 128, 255, 255 }
                                                        : std::vector<uint8_t>{ 0, 0, 0, 255 } };
    texture = UploadLevels(device, 1, 1, levels, TextureUsage::Data);
    owner = device;
    return texture;
}

SDL_GPUSampler* WorldTextureStore::GetRepeatSampler(SDL_GPUDevice* device)
{
    if (m_SamplerDevice != device)
    {
        m_Repeat = nullptr;
        m_Clamp = nullptr;
        m_SamplerDevice = device;
    }
    if (m_Repeat == nullptr)
    {
        m_Repeat = CreateSampler(device, SDL_GPU_SAMPLERADDRESSMODE_REPEAT);
    }
    return m_Repeat;
}

SDL_GPUSampler* WorldTextureStore::GetClampSampler(SDL_GPUDevice* device)
{
    if (m_SamplerDevice != device)
    {
        m_Repeat = nullptr;
        m_Clamp = nullptr;
        m_SamplerDevice = device;
    }
    if (m_Clamp == nullptr)
    {
        m_Clamp = CreateSampler(device, SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE);
    }
    return m_Clamp;
}

void WorldTextureStore::ReleaseGpu(SDL_GPUDevice* device)
{
    for (WorldTextureRecord& record : m_Textures)
    {
        if (record.Texture != nullptr && record.Device == device)
        {
            SDL_ReleaseGPUTexture(device, record.Texture);
            record.Texture = nullptr;
            record.Device = nullptr;
        }
    }

    if (m_White != nullptr && m_WhiteDevice == device)
    {
        SDL_ReleaseGPUTexture(device, m_White);
        m_White = nullptr;
        m_WhiteDevice = nullptr;
    }
    if (m_FlatNormal != nullptr && m_FlatNormalDevice == device)
    {
        SDL_ReleaseGPUTexture(device, m_FlatNormal);
        m_FlatNormal = nullptr;
        m_FlatNormalDevice = nullptr;
    }
    if (m_Black != nullptr && m_BlackDevice == device)
    {
        SDL_ReleaseGPUTexture(device, m_Black);
        m_Black = nullptr;
        m_BlackDevice = nullptr;
    }

    if (m_SamplerDevice == device)
    {
        if (m_Repeat != nullptr)
        {
            SDL_ReleaseGPUSampler(device, m_Repeat);
        }
        if (m_Clamp != nullptr)
        {
            SDL_ReleaseGPUSampler(device, m_Clamp);
        }
        m_Repeat = nullptr;
        m_Clamp = nullptr;
        m_SamplerDevice = nullptr;
    }
}

WorldTextureStore& GetWorldTextures()
{
    static WorldTextureStore store;
    return store;
}

} // namespace detail

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

TextureId LoadTexture(const std::string& path, TextureUsage usage)
{
    return detail::GetWorldTextures().Load(path, usage);
}

TextureId CreateTexture(int width, int height, const void* rgbaPixels, TextureUsage usage)
{
    return detail::GetWorldTextures().Create(width, height, rgbaPixels, usage);
}

void DestroyTexture(TextureId texture)
{
    detail::GetWorldTextures().Destroy(texture);
}

Vec2 GetTextureSize(TextureId texture)
{
    return detail::GetWorldTextures().GetSize(texture);
}

} // namespace ludifex
