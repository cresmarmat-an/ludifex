// World textures: images for models, actors, sprites, and backgrounds.
// Not installed and not part of the public API.
//
// Like materials, textures are process-wide. A texture is decoded when it is
// loaded and uploaded the first time a renderer draws it, so loading never
// needs a GPU device; a headless test can load and query one.
//
// A model being parsed in the background loads its images from the loading
// thread while the main thread is drawing, so the store is guarded by one
// lock, held only while a record is found or added.

#pragma once

#include "Assets.h"
#include "Internal.h"

#include <SDL3/SDL.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ludifex::detail
{

struct WorldTextureRecord
{
    uint32_t Generation = 0;
    bool Alive = false;

    int Width = 0;
    int Height = 0;

    // The mip chain, largest first, waiting to be uploaded. Released once it
    // is on the GPU unless the texture has no file to be decoded from again.
    std::vector<std::vector<uint8_t>> Levels;

    // Where the pixels came from, so a texture lost with a device can be
    // decoded again rather than kept in memory twice.
    std::string Path;

    // The file behind it, watched for changes. Empty for a texture built in
    // memory or unpacked from inside a model, which have no file to watch.
    FileWatch Watch;

    // Colour is stored as sRGB and sampled as linear light; data and normals
    // are stored and sampled exactly as they are.
    TextureUsage Usage = TextureUsage::Color;

    SDL_GPUTexture* Texture = nullptr;
    SDL_GPUDevice* Device = nullptr;

    // How many mip levels the GPU texture has, which stays known after the
    // pixels themselves are let go.
    int LevelCount = 0;
};

class WorldTextureStore
{
public:
    TextureId Load(const std::string& path, TextureUsage usage = TextureUsage::Color);
    TextureId Create(int width, int height, const void* rgbaPixels, TextureUsage usage = TextureUsage::Color);

    // An encoded image already in memory, such as a texture embedded in a .glb.
    // The key caches it, so a model loaded twice decodes its images once.
    TextureId LoadEncoded(const uint8_t* bytes, size_t size, const std::string& key,
                          TextureUsage usage = TextureUsage::Color);
    void Destroy(TextureId id);

    // Decodes again any watched texture whose file has changed, and lets go
    // of its GPU texture so the next draw uploads the new pixels. Returns how
    // many were reloaded.
    int ReloadChanged();

    WorldTextureRecord* Resolve(const TextureId& id);
    Vec2 GetSize(const TextureId& id);

    // The GPU texture for a handle, uploading it on first use. An invalid or
    // destroyed handle gives the plain white texture, so "no texture" and
    // "texture times colour" are the same shader path.
    // What a surface uses in place of a texture it does not have.
    enum class Fallback
    {
        White,       // base colour, occlusion, metallic-roughness: no change
        FlatNormal,  // a normal map that leaves the surface as it is
        Black        // emission: none
    };

    SDL_GPUTexture* GetGpuTexture(SDL_GPUDevice* device, const TextureId& id, Fallback fallback = Fallback::White);
    SDL_GPUTexture* GetWhite(SDL_GPUDevice* device);
    SDL_GPUTexture* GetFallback(SDL_GPUDevice* device, Fallback fallback);

    // Repeat for surfaces, so a texture can tile across a large face; clamp for
    // sprites and backdrops, whose edges must not pick up the opposite side.
    SDL_GPUSampler* GetRepeatSampler(SDL_GPUDevice* device);
    SDL_GPUSampler* GetClampSampler(SDL_GPUDevice* device);

    // The shared placeholder for images that could not be loaded.
    TextureId GetMissing();

    void ReleaseGpu(SDL_GPUDevice* device);

private:
    // Guards every mutation and lookup, because a model parsed in the
    // background loads its images from the loading thread.
    mutable std::recursive_mutex m_Mutex;

    TextureId Store(int width, int height, std::vector<std::vector<uint8_t>> levels, const std::string& path);
    bool Upload(SDL_GPUDevice* device, WorldTextureRecord& record);

    std::vector<WorldTextureRecord> m_Textures;
    std::vector<uint32_t> m_Free;
    std::unordered_map<std::string, TextureId> m_ByPath;

    // By contents instead of by name, so the same image under two names (or
    // embedded in two models) is decoded and uploaded once.
    std::unordered_map<uint64_t, TextureId> m_ByHash;

    TextureId m_Missing;

    SDL_GPUDevice* m_SamplerDevice = nullptr;
    SDL_GPUSampler* m_Repeat = nullptr;
    SDL_GPUSampler* m_Clamp = nullptr;

    SDL_GPUDevice* m_WhiteDevice = nullptr;
    SDL_GPUTexture* m_White = nullptr;
    SDL_GPUDevice* m_FlatNormalDevice = nullptr;
    SDL_GPUTexture* m_FlatNormal = nullptr;
    SDL_GPUDevice* m_BlackDevice = nullptr;
    SDL_GPUTexture* m_Black = nullptr;
};

WorldTextureStore& GetWorldTextures();

} // namespace ludifex::detail
