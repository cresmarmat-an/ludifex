// What differs between Direct3D 12, Vulkan, and Metal: the backend a device is
// created on, the shader format it takes, and compiling a material's source to
// that format at runtime. Everything else in ludifex is the same on all three.
// Mirrors opane's, since the libraries do not share code. Not installed and
// not part of the public API.

#pragma once

#include "Internal.h"

#include <SDL3/SDL.h>

#include <string>
#include <vector>

namespace ludifex::detail
{

// ludifex's own shaders.
enum class BuiltInShader
{
    WorldVertex,
    WorldFragment,
    ShadowVertex,
    ShadowFragment,
    MotionVertex,
    MotionFragment,
    LineVertex,
    LineFragment,
    FullscreenVertex,
    BackdropFragment,
    ToneMapFragment,
    FxaaFragment,
    TaaFragment,
    SkyFragment,
    OcclusionFragment,
    OcclusionBlurFragment,
    BloomPrefilterFragment,
    BloomDownFragment,
    BloomUpFragment,
    LuminanceFragment,
    AdaptFragment,
    UpscaleFragment,
    RayHybrid,
    RayTemporal,
    RayFilter,
    PathTrace,
    Count
};

// One of ludifex's own shaders, in every format the build made.
ShaderBytecode GetBuiltInShader(BuiltInShader shader);

// The formats ludifex's own shaders were built in. A device on a backend
// outside these could draw nothing, so none is ever created there.
SDL_GPUShaderFormat BuiltInShaderFormats();

// Creates the device: on the first backend this machine runs, for Automatic,
// or on exactly the one named. Logs why when it cannot.
SDL_GPUDevice* CreateDevice(GraphicsBackend backend, bool debug);

GraphicsBackend DeviceBackend(SDL_GPUDevice* device);

// The format a device will take: this one's when there is a device, or else
// the format of the backend that would be chosen for the request (for
// Automatic, decided by SDL_GPU_DRIVER and the platform).
SDL_GPUShaderFormat PredictShaderFormat(SDL_GPUDevice* device, GraphicsBackend requested);

// The one of DXIL, SPIR-V, and MSL this device takes.
SDL_GPUShaderFormat DeviceShaderFormat(SDL_GPUDevice* device);

// "DXIL", "SPIR-V", or "MSL".
const char* ShaderFormatName(SDL_GPUShaderFormat format);

// The bytecode in one format, or false when it has none.
bool PickShaderCode(const ShaderBytecode& bytecode, SDL_GPUShaderFormat format, const void*& outCode,
                    size_t& outSize);

// Compiles a material's fragment stage from an .hlsl file to the format,
// with compilers found at runtime. On failure, outError holds the compiler's
// own message, with the file, line, and column, or says why none could run.
bool CompileMaterialSource(const std::string& sourcePath, const std::string& entryPoint,
                           SDL_GPUShaderFormat format, std::vector<unsigned char>& outCode,
                           std::string& outError);

} // namespace ludifex::detail
