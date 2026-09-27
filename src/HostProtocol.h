// The host handshake, seen from the guest side: how ludifex finds a host
// application's GPU device, window, and frame loop (opane's, when it is
// present) without either library including the other's headers.
//
// Everything travels through SDL's global property set, which both libraries
// share because they link the same SDL. These plain C layouts are declared
// identically by the host; the Version field guards the layout. Not installed
// and not part of the public API.
//
//   "host.gpu_device"      SDL_GPUDevice*     set by the host
//   "host.window"          SDL_Window*        set by the host
//   "host.interface"       HostInterface*     set by the host
//   "host.audio"           HostAudio*         set by the host when audio runs
//   "host.device_release"  DeviceReleaseHook* set by the guest, called by the
//                                             host before the device is destroyed

#pragma once

#include <cstdint>

namespace ludifex::detail
{

constexpr const char* HostDeviceProperty = "host.gpu_device";
constexpr const char* HostWindowProperty = "host.window";
constexpr const char* HostInterfaceProperty = "host.interface";
constexpr const char* HostDeviceReleaseProperty = "host.device_release";

constexpr uint32_t HostProtocolVersion = 1;

struct HostWorldHooks
{
    uint32_t Version;
    void* World;
    void (*Update)(void* world, float seconds);
    void (*SetRenderSize)(void* world, int width, int height);
    void (*Render)(void* world);
    void* (*GetRenderTarget)(void* world);
};

struct HostInterface
{
    uint32_t Version;
    void* Context;
    void (*RunWorld)(void* context, const HostWorldHooks* world);
};

// The host's audio output: a guest adds a source, and the host calls Read on
// its audio thread for exactly the frames it is about to mix, interleaved
// 32-bit float at the host's rate and channel count.
constexpr const char* HostAudioProperty = "host.audio";

struct HostAudio
{
    uint32_t Version;
    void* Context;
    uint32_t SampleRate;
    uint32_t Channels;
    int (*AddSource)(void* context, void (*read)(void* user, float* frames, uint32_t frameCount), void* user);
    void (*RemoveSource)(void* context, int source);
};

struct DeviceReleaseHook
{
    uint32_t Version;
    void* Context;
    void (*Release)(void* context);
};

// Shows a world until its window closes: in the host's window and frame loop
// when a host is present, in a window of its own otherwise.
void RunWorld(const HostWorldHooks& hooks, const char* title);

template <typename World>
HostWorldHooks MakeHostHooks(World* world)
{
    HostWorldHooks hooks{};
    hooks.Version = HostProtocolVersion;
    hooks.World = world;
    hooks.Update = [](void* object, float seconds) { static_cast<World*>(object)->Update(seconds); };
    hooks.SetRenderSize = [](void* object, int width, int height) {
        static_cast<World*>(object)->SetRenderSize(width, height);
    };
    hooks.Render = [](void* object) { static_cast<World*>(object)->Render(); };
    hooks.GetRenderTarget = [](void* object) -> void* { return static_cast<World*>(object)->GetRenderTarget(); };
    return hooks;
}

} // namespace ludifex::detail
