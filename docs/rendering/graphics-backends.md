# Graphics backends

SDL's GPU API runs on three graphics APIs: Direct3D 12, Vulkan, and Metal.
When ludifex creates its own device, it uses the first backend the machine can
run, tried in the order Metal, Direct3D 12, Vulkan. In practice that is
Direct3D 12 on Windows (Vulkan where Direct3D 12 is missing), Metal on a Mac,
and Vulkan on Linux. Only backends this build of ludifex has shaders for are
tried; see [Installation](../getting-started/installation.md#requirements).

## Choosing a backend

Name a backend before the first world renders to use it and no other:

```cpp
ludifex::SetGraphicsBackend(ludifex::GraphicsBackend::Vulkan);
```

A named backend is strict. When the machine cannot run it, rendering is
unavailable and the log says why, instead of silently falling back to another
backend.

For a settings screen, ask which backends would work:

```cpp
ludifex::IsGraphicsBackendAvailable(ludifex::GraphicsBackend::Direct3D12);
ludifex::GetGraphicsBackendName(ludifex::GraphicsBackend::Direct3D12);   // "Direct3D 12"
ludifex::GetGraphicsBackend();   // the device's backend, once there is a device
```

A device adopted from a host keeps the host's backend. With opane, choose the
backend in `opane::AppConfig::Backend` instead.

To try another backend without rebuilding, set the `SDL_GPU_DRIVER`
environment variable to `direct3d12`, `vulkan`, or `metal`. It changes the
automatic choice; a named backend ignores it.

## The same on every backend

Worlds look and behave the same on each backend, including custom materials,
ray tracing, and path tracing, which run in compute shaders on all three.

## Limitations

- The backend cannot change while the program runs; save the choice and apply
  it at the next start.
- Metal is built on macOS but has not been run on Apple hardware yet.
- There is no OpenGL, WebGPU, or software backend.
