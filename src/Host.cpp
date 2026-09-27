#include "Audio.h"
#include "Backend.h"
#include "HostProtocol.h"
#include "Render3D.h"
#include "WorldTextures.h"

#include <algorithm>
#include <vector>

namespace ludifex
{
namespace detail
{

SDL_GPUDevice* g_Device = nullptr;
bool g_OwnsDevice = false;

// What SetGraphicsBackend asked for; used when ludifex makes its own device.
GraphicsBackend g_RequestedBackend = GraphicsBackend::Automatic;

namespace
{

// Every renderer alive in the process, so all of their GPU objects can be let
// go at once when a host is about to destroy the device they were made on.
std::vector<Renderer3D*>& Renderers()
{
    static std::vector<Renderer3D*> renderers;
    return renderers;
}

// Called by the host, while its device still exists, just before destroying
// it. Afterwards every renderer is inert: drawing does nothing and destroying a
// world touches no GPU object.
void ReleaseForHost(void* context)
{
    (void)context;

    const std::vector<Renderer3D*> renderers = Renderers();
    for (Renderer3D* renderer : renderers)
    {
        renderer->Shutdown();
    }

    if (g_Device != nullptr)
    {
        GetWorldTextures().ReleaseGpu(g_Device);
    }

    g_Device = nullptr;
    g_OwnsDevice = false;

    // The host's audio engine goes too, so the world's mix stops being pulled
    // from it now rather than after it is gone.
    ReleaseAudioForHost();

    LogMessage(LogLevel::Info, "render", "Released every GPU object ahead of the host's device.");
}

DeviceReleaseHook g_ReleaseHook{ HostProtocolVersion, nullptr, &ReleaseForHost };

// A device that belongs to someone else is only safe to use for as long as
// they keep it, so adopting one registers the hook that lets them say when.
void RegisterReleaseHook()
{
    SDL_SetPointerProperty(SDL_GetGlobalProperties(), HostDeviceReleaseProperty, &g_ReleaseHook);
}

} // namespace

void RegisterRenderer(Renderer3D* renderer)
{
    Renderers().push_back(renderer);
}

void UnregisterRenderer(Renderer3D* renderer)
{
    auto& renderers = Renderers();
    renderers.erase(std::remove(renderers.begin(), renderers.end(), renderer), renderers.end());
}

// Uses the host's device when one is published; otherwise creates one. No
// window is needed: the world renders into an offscreen texture, so ludifex can
// draw in a program that has no window at all, such as a thumbnail tool or a
// test.
SDL_GPUDevice* EnsureDevice()
{
    if (g_Device != nullptr)
    {
        return g_Device;
    }

    if (auto* hostDevice = static_cast<SDL_GPUDevice*>(
            SDL_GetPointerProperty(SDL_GetGlobalProperties(), HostDeviceProperty, nullptr)))
    {
        g_Device = hostDevice;
        g_OwnsDevice = false;
        RegisterReleaseHook();
        LogMessage(LogLevel::Info, "render", "Found the host application's GPU device and is sharing it.");
        return g_Device;
    }

    if (!SDL_WasInit(SDL_INIT_VIDEO))
    {
        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            LogMessage(LogLevel::Error, "render", "Could not start SDL video: %s", SDL_GetError());
            return nullptr;
        }
    }

    g_Device = CreateDevice(g_RequestedBackend, false);
    if (g_Device == nullptr)
    {
        LogMessage(LogLevel::Error, "render", "Rendering is unavailable; simulation still runs.");
        return nullptr;
    }

    g_OwnsDevice = true;
    const SDL_PropertiesID properties = SDL_GetGPUDeviceProperties(g_Device);
    LogMessage(LogLevel::Info, "render", "Created a GPU device: %s on %s.",
               GetGraphicsBackendName(DeviceBackend(g_Device)),
               SDL_GetStringProperty(properties, SDL_PROP_GPU_DEVICE_NAME_STRING, "an unnamed GPU"));
    return g_Device;
}

SDL_GPUShaderFormat ExpectedShaderFormat()
{
    SDL_GPUDevice* device = g_Device;
    if (device == nullptr)
    {
        device = static_cast<SDL_GPUDevice*>(
            SDL_GetPointerProperty(SDL_GetGlobalProperties(), HostDeviceProperty, nullptr));
    }
    return PredictShaderFormat(device, g_RequestedBackend);
}

void ReleaseDeviceIfOwned()
{
    if (g_Device != nullptr)
    {
        GetWorldTextures().ReleaseGpu(g_Device);
    }
    if (g_Device != nullptr && g_OwnsDevice)
    {
        SDL_DestroyGPUDevice(g_Device);
    }
    g_Device = nullptr;
    g_OwnsDevice = false;
}

// ---------------------------------------------------------------------------
// Running a world
// ---------------------------------------------------------------------------

namespace
{

// The loop for a program with no host: a window of its own, the world stepped
// and drawn each frame, and its image copied to the window.
void RunStandalone(const HostWorldHooks& hooks, const char* title)
{
    SDL_GPUDevice* device = EnsureDevice();
    if (device == nullptr)
    {
        return;
    }

    SDL_Window* window = SDL_CreateWindow(title, 1280, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window == nullptr)
    {
        LogMessage(LogLevel::Error, "render", "Could not open a window: %s", SDL_GetError());
        return;
    }

    if (!SDL_ClaimWindowForGPUDevice(device, window))
    {
        LogMessage(LogLevel::Error, "render", "Could not present to the window: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        return;
    }

    LogMessage(LogLevel::Info, "render", "Running the world in a window of its own.");

    const double frequency = static_cast<double>(SDL_GetPerformanceFrequency());
    uint64_t last = SDL_GetPerformanceCounter();
    bool running = true;

    while (running)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
            {
                running = false;
            }
        }

        const uint64_t now = SDL_GetPerformanceCounter();
        const float delta = static_cast<float>(std::min(static_cast<double>(now - last) / frequency, 0.25));
        last = now;

        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(window, &width, &height);
        if (width <= 0 || height <= 0)
        {
            // Minimised: keep the simulation running without drawing.
            hooks.Update(hooks.World, delta);
            SDL_Delay(10);
            continue;
        }

        hooks.SetRenderSize(hooks.World, width, height);
        hooks.Update(hooks.World, delta);
        hooks.Render(hooks.World);

        SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(device);
        if (commandBuffer == nullptr)
        {
            continue;
        }

        SDL_GPUTexture* swapchain = nullptr;
        uint32_t swapchainWidth = 0;
        uint32_t swapchainHeight = 0;
        auto* image = static_cast<SDL_GPUTexture*>(hooks.GetRenderTarget(hooks.World));

        if (SDL_WaitAndAcquireGPUSwapchainTexture(commandBuffer, window, &swapchain, &swapchainWidth,
                                                  &swapchainHeight) &&
            swapchain != nullptr && image != nullptr)
        {
            SDL_GPUBlitInfo blit{};
            blit.source.texture = image;
            blit.source.w = static_cast<uint32_t>(width);
            blit.source.h = static_cast<uint32_t>(height);
            blit.destination.texture = swapchain;
            blit.destination.w = swapchainWidth;
            blit.destination.h = swapchainHeight;
            blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
            blit.filter = SDL_GPU_FILTER_LINEAR;
            SDL_BlitGPUTexture(commandBuffer, &blit);
        }

        SDL_SubmitGPUCommandBuffer(commandBuffer);
    }

    SDL_ReleaseWindowFromGPUDevice(device, window);
    SDL_DestroyWindow(window);
}

} // namespace

void RunWorld(const HostWorldHooks& hooks, const char* title)
{
    // A host application (opane after StartApp) runs the world in its own
    // window and loop, beneath its interface.
    auto* host = static_cast<HostInterface*>(
        SDL_GetPointerProperty(SDL_GetGlobalProperties(), HostInterfaceProperty, nullptr));
    if (host != nullptr && host->Version == HostProtocolVersion && host->RunWorld != nullptr)
    {
        // The device must be the host's before the first frame renders.
        EnsureDevice();
        host->RunWorld(host->Context, &hooks);
        return;
    }

    RunStandalone(hooks, title);
}

} // namespace detail

void AdoptHost(const Host& host)
{
    if (host.GpuDevice == nullptr)
    {
        LogMessage(LogLevel::Warning, "render", "AdoptHost was given no GPU device; ignoring it.");
        return;
    }

    if (detail::g_Device != nullptr && detail::g_Device != host.GpuDevice)
    {
        if (detail::g_OwnsDevice)
        {
            LogMessage(LogLevel::Warning, "render",
                       "A device was already created here and is being replaced by the adopted one. Call "
                       "AdoptHost before creating a world that renders.");
            detail::GetWorldTextures().ReleaseGpu(detail::g_Device);
            SDL_DestroyGPUDevice(detail::g_Device);
        }
        else
        {
            LogMessage(LogLevel::Warning, "render", "Replacing a previously adopted GPU device.");
        }
    }

    detail::g_Device = static_cast<SDL_GPUDevice*>(host.GpuDevice);

    // An adopted device belongs to whoever created it, so it is never destroyed
    // here. The owner is given a hook to make the guest release its objects
    // first.
    detail::g_OwnsDevice = false;
    detail::RegisterReleaseHook();

    LogMessage(LogLevel::Info, "render", "Adopted an existing GPU device.");
}

bool IsRenderingAvailable()
{
    return detail::g_Device != nullptr;
}

void SetGraphicsBackend(GraphicsBackend backend)
{
    if (detail::g_Device != nullptr && backend != GraphicsBackend::Automatic &&
        detail::DeviceBackend(detail::g_Device) != backend)
    {
        LogMessage(LogLevel::Warning, "render",
                   "SetGraphicsBackend(%s) came after the device was made, on %s, and does not change it. Call it "
                   "before the first world that renders.",
                   GetGraphicsBackendName(backend), GetGraphicsBackendName(detail::DeviceBackend(detail::g_Device)));
    }
    detail::g_RequestedBackend = backend;
}

GraphicsBackend GetGraphicsBackend()
{
    return detail::DeviceBackend(detail::g_Device);
}

} // namespace ludifex
