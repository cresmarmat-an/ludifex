#include "Backend.h"

#include <SDL3/SDL_process.h>

#include <algorithm>
#include <filesystem>
#include <functional>

#ifndef LUDIFEX_DXC_PATH
#define LUDIFEX_DXC_PATH ""
#endif
#ifndef LUDIFEX_DXC_SPIRV_PATH
#define LUDIFEX_DXC_SPIRV_PATH ""
#endif
#ifndef LUDIFEX_SPIRV_CROSS_PATH
#define LUDIFEX_SPIRV_CROSS_PATH ""
#endif

// Embedded by the build, one per format. A format the build did not make is
// present and empty.
#define LUDIFEX_DECLARE_SHADER(name)                                                                     \
    extern const unsigned char name##Dxil[];                                                           \
    extern const size_t name##DxilSize;                                                                \
    extern const unsigned char name##Spirv[];                                                          \
    extern const size_t name##SpirvSize;                                                               \
    extern const unsigned char name##Msl[];                                                            \
    extern const size_t name##MslSize;

LUDIFEX_DECLARE_SHADER(LudifexWorldVertex)
LUDIFEX_DECLARE_SHADER(LudifexWorldFragment)
LUDIFEX_DECLARE_SHADER(LudifexShadowVertex)
LUDIFEX_DECLARE_SHADER(LudifexShadowFragment)
LUDIFEX_DECLARE_SHADER(LudifexMotionVertex)
LUDIFEX_DECLARE_SHADER(LudifexMotionFragment)
LUDIFEX_DECLARE_SHADER(LudifexLineVertex)
LUDIFEX_DECLARE_SHADER(LudifexLineFragment)
LUDIFEX_DECLARE_SHADER(LudifexFullscreenVertex)
LUDIFEX_DECLARE_SHADER(LudifexBackdropFragment)
LUDIFEX_DECLARE_SHADER(LudifexToneMapFragment)
LUDIFEX_DECLARE_SHADER(LudifexFxaaFragment)
LUDIFEX_DECLARE_SHADER(LudifexTaaFragment)
LUDIFEX_DECLARE_SHADER(LudifexSkyFragment)
LUDIFEX_DECLARE_SHADER(LudifexOcclusionFragment)
LUDIFEX_DECLARE_SHADER(LudifexOcclusionBlurFragment)
LUDIFEX_DECLARE_SHADER(LudifexBloomPrefilterFragment)
LUDIFEX_DECLARE_SHADER(LudifexBloomDownFragment)
LUDIFEX_DECLARE_SHADER(LudifexBloomUpFragment)
LUDIFEX_DECLARE_SHADER(LudifexLuminanceFragment)
LUDIFEX_DECLARE_SHADER(LudifexAdaptFragment)
LUDIFEX_DECLARE_SHADER(LudifexUpscaleFragment)
LUDIFEX_DECLARE_SHADER(LudifexRayHybrid)
LUDIFEX_DECLARE_SHADER(LudifexRayTemporal)
LUDIFEX_DECLARE_SHADER(LudifexRayFilter)
LUDIFEX_DECLARE_SHADER(LudifexPathTrace)

// The includes a material shader reads.
extern const unsigned char LudifexWorldMaterialInclude[];
extern const size_t LudifexWorldMaterialIncludeSize;
extern const unsigned char LudifexWorldPostInclude[];
extern const size_t LudifexWorldPostIncludeSize;

namespace ludifex
{
namespace detail
{
namespace
{

#define LUDIFEX_SHADER_BYTECODE(name)                                                                    \
    ShaderBytecode                                                                                     \
    {                                                                                                  \
        name##Dxil, name##DxilSize, name##Spirv, name##SpirvSize, name##Msl, name##MslSize             \
    }

#ifdef _WIN32
constexpr const char* ExecutableSuffix = ".exe";
#else
constexpr const char* ExecutableSuffix = "";
#endif

const char* DriverName(GraphicsBackend backend)
{
    switch (backend)
    {
    case GraphicsBackend::Direct3D12:
        return "direct3d12";
    case GraphicsBackend::Vulkan:
        return "vulkan";
    case GraphicsBackend::Metal:
        return "metal";
    default:
        return nullptr;
    }
}

SDL_GPUShaderFormat BackendShaderFormat(GraphicsBackend backend)
{
    switch (backend)
    {
    case GraphicsBackend::Direct3D12:
        return SDL_GPU_SHADERFORMAT_DXIL;
    case GraphicsBackend::Vulkan:
        return SDL_GPU_SHADERFORMAT_SPIRV;
    case GraphicsBackend::Metal:
        return SDL_GPU_SHADERFORMAT_MSL;
    default:
        return SDL_GPU_SHADERFORMAT_INVALID;
    }
}

// What a build would need to carry the backend's shaders.
const char* HowToBuild(GraphicsBackend backend)
{
    switch (backend)
    {
    case GraphicsBackend::Direct3D12:
        return "Direct3D 12 shaders are built on Windows, with the Windows SDK.";
    case GraphicsBackend::Vulkan:
        return "Vulkan shaders are built when the Vulkan SDK is installed.";
    case GraphicsBackend::Metal:
        return "Metal shaders are built on macOS, with the Vulkan SDK installed.";
    default:
        return "";
    }
}

// SDL reads SDL_GPU_DRIVER before the name it is given, so a strict choice
// holds the hint at its own backend for as long as it takes to act on it.
class DriverOverride
{
public:
    explicit DriverOverride(const char* driver)
    {
        SDL_SetHintWithPriority(SDL_HINT_GPU_DRIVER, driver, SDL_HINT_OVERRIDE);
    }
    ~DriverOverride() { SDL_ResetHint(SDL_HINT_GPU_DRIVER); }

    DriverOverride(const DriverOverride&) = delete;
    DriverOverride& operator=(const DriverOverride&) = delete;
};

// --- compilers --------------------------------------------------------------

bool IsFile(const std::string& path)
{
    std::error_code error;
    return !path.empty() && std::filesystem::is_regular_file(path, error);
}

std::string FromEnvironment(const char* variable)
{
    const char* value = SDL_getenv(variable);
    return value != nullptr && IsFile(value) ? std::string(value) : std::string();
}

// A tool the Vulkan SDK ships, found through VULKAN_SDK, which its installer
// sets. The folder is Bin on Windows and bin elsewhere.
std::string FromVulkanSdk(const char* tool)
{
    const char* sdk = SDL_getenv("VULKAN_SDK");
    if (sdk == nullptr)
    {
        return std::string();
    }
    for (const char* folder : { "Bin", "bin" })
    {
        const std::string candidate =
            (std::filesystem::path(sdk) / folder / (std::string(tool) + ExecutableSuffix)).string();
        if (IsFile(candidate))
        {
            return candidate;
        }
    }
    return std::string();
}

// dxc for DXIL: the one named by LUDIFEX_DXC in the environment, the one this
// build of ludifex used if it is still there, or the newest Windows SDK's.
// Found once; empty when there is none.
const std::string& FindDxilCompiler()
{
    static const std::string compiler = [] {
        if (std::string named = FromEnvironment("LUDIFEX_DXC"); !named.empty())
        {
            return named;
        }
        if (IsFile(LUDIFEX_DXC_PATH))
        {
            return std::string(LUDIFEX_DXC_PATH);
        }

        // dxc signs its output with the dxil.dll beside it, and Direct3D 12
        // refuses an unsigned shader, so a compiler is only taken from a
        // folder that has both, as the Windows SDK's does.
        std::error_code error;
        const std::filesystem::path kits = "C:/Program Files (x86)/Windows Kits/10/bin";
        std::vector<std::filesystem::path> versions;
        for (const auto& entry : std::filesystem::directory_iterator(kits, error))
        {
            if (entry.is_directory(error) && entry.path().filename().string().rfind("10.", 0) == 0)
            {
                versions.push_back(entry.path());
            }
        }
        std::sort(versions.begin(), versions.end(), std::greater<>());
        for (const std::filesystem::path& version : versions)
        {
            const std::filesystem::path candidate = version / "x64" / "dxc.exe";
            if (IsFile(candidate.string()) && IsFile((version / "x64" / "dxil.dll").string()))
            {
                return candidate.string();
            }
        }
        return std::string();
    }();
    return compiler;
}

// dxc with SPIR-V output, for Vulkan and, through spirv-cross, Metal: named by
// LUDIFEX_DXC_SPIRV, the one this build used, or the Vulkan SDK's.
const std::string& FindSpirvCompiler()
{
    static const std::string compiler = [] {
        if (std::string named = FromEnvironment("LUDIFEX_DXC_SPIRV"); !named.empty())
        {
            return named;
        }
        if (IsFile(LUDIFEX_DXC_SPIRV_PATH))
        {
            return std::string(LUDIFEX_DXC_SPIRV_PATH);
        }
        return FromVulkanSdk("dxc");
    }();
    return compiler;
}

// spirv-cross, which turns SPIR-V into Metal's source: named by
// LUDIFEX_SPIRV_CROSS, the one this build used, or the Vulkan SDK's.
const std::string& FindSpirvCross()
{
    static const std::string tool = [] {
        if (std::string named = FromEnvironment("LUDIFEX_SPIRV_CROSS"); !named.empty())
        {
            return named;
        }
        if (IsFile(LUDIFEX_SPIRV_CROSS_PATH))
        {
            return std::string(LUDIFEX_SPIRV_CROSS_PATH);
        }
        return FromVulkanSdk("spirv-cross");
    }();
    return tool;
}

bool ReadWholeFile(const std::string& path, std::vector<unsigned char>& outBytes)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0)
    {
        return false;
    }

    SDL_IOStream* stream = SDL_IOFromFile(path.c_str(), "rb");
    if (stream == nullptr)
    {
        return false;
    }

    outBytes.resize(static_cast<size_t>(size));
    const size_t read = SDL_ReadIO(stream, outBytes.data(), outBytes.size());
    SDL_CloseIO(stream);

    return read == outBytes.size();
}

// Writes the files a material shader includes (the versions built into this
// copy of ludifex) into directory, and returns false if that fails. A file is only rewritten when its contents differ, so a
// program that reloads a material does not keep touching them.
bool WriteShaderIncludes(const std::filesystem::path& directory)
{
    struct Include
    {
        const char* Name;
        const unsigned char* Bytes;
        size_t Size;
    };
    static const Include includes[] = {
        { "world_material.hlsli", LudifexWorldMaterialInclude, LudifexWorldMaterialIncludeSize },
        { "world_post.hlsli", LudifexWorldPostInclude, LudifexWorldPostIncludeSize },
    };

    std::error_code error;
    std::filesystem::create_directories(directory, error);

    for (const Include& include : includes)
    {
        const std::string path = (directory / include.Name).string();

        std::vector<unsigned char> existing;
        if (ReadWholeFile(path, existing) && existing.size() == include.Size &&
            std::equal(existing.begin(), existing.end(), include.Bytes))
        {
            continue;
        }

        SDL_IOStream* stream = SDL_IOFromFile(path.c_str(), "wb");
        if (stream == nullptr)
        {
            return false;
        }
        const size_t written = SDL_WriteIO(stream, include.Bytes, include.Size);
        SDL_CloseIO(stream);
        if (written != include.Size)
        {
            return false;
        }
    }
    return true;
}

// Runs a tool to completion and returns its exit code, or -1 when it could
// not be started. Everything it printed lands in outOutput: compilers report
// errors on stderr, and folding that into stdout is what lets the file, line,
// and column reach the log handler instead of a console the program may not
// even have.
int RunTool(const std::vector<std::string>& arguments, std::string& outOutput)
{
    std::vector<const char*> argv;
    for (const std::string& argument : arguments)
    {
        argv.push_back(argument.c_str());
    }
    argv.push_back(nullptr);

    SDL_PropertiesID properties = SDL_CreateProperties();
    SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, const_cast<char**>(argv.data()));
    SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);

    SDL_Process* process = SDL_CreateProcessWithProperties(properties);
    SDL_DestroyProperties(properties);

    if (process == nullptr)
    {
        outOutput = std::string("could not run ") + arguments.front() + ": " + SDL_GetError();
        return -1;
    }

    size_t outputSize = 0;
    int exitCode = -1;
    void* output = SDL_ReadProcess(process, &outputSize, &exitCode);
    if (output != nullptr)
    {
        outOutput.assign(static_cast<const char*>(output), outputSize);
        SDL_free(output);
    }
    SDL_DestroyProcess(process);
    return exitCode;
}

} // namespace

ShaderBytecode GetBuiltInShader(BuiltInShader shader)
{
    switch (shader)
    {
    case BuiltInShader::WorldVertex:
        return LUDIFEX_SHADER_BYTECODE(LudifexWorldVertex);
    case BuiltInShader::WorldFragment:
        return LUDIFEX_SHADER_BYTECODE(LudifexWorldFragment);
    case BuiltInShader::ShadowVertex:
        return LUDIFEX_SHADER_BYTECODE(LudifexShadowVertex);
    case BuiltInShader::ShadowFragment:
        return LUDIFEX_SHADER_BYTECODE(LudifexShadowFragment);
    case BuiltInShader::MotionVertex:
        return LUDIFEX_SHADER_BYTECODE(LudifexMotionVertex);
    case BuiltInShader::MotionFragment:
        return LUDIFEX_SHADER_BYTECODE(LudifexMotionFragment);
    case BuiltInShader::LineVertex:
        return LUDIFEX_SHADER_BYTECODE(LudifexLineVertex);
    case BuiltInShader::LineFragment:
        return LUDIFEX_SHADER_BYTECODE(LudifexLineFragment);
    case BuiltInShader::FullscreenVertex:
        return LUDIFEX_SHADER_BYTECODE(LudifexFullscreenVertex);
    case BuiltInShader::BackdropFragment:
        return LUDIFEX_SHADER_BYTECODE(LudifexBackdropFragment);
    case BuiltInShader::ToneMapFragment:
        return LUDIFEX_SHADER_BYTECODE(LudifexToneMapFragment);
    case BuiltInShader::FxaaFragment:
        return LUDIFEX_SHADER_BYTECODE(LudifexFxaaFragment);
    case BuiltInShader::TaaFragment:
        return LUDIFEX_SHADER_BYTECODE(LudifexTaaFragment);
    case BuiltInShader::SkyFragment:
        return LUDIFEX_SHADER_BYTECODE(LudifexSkyFragment);
    case BuiltInShader::OcclusionFragment:
        return LUDIFEX_SHADER_BYTECODE(LudifexOcclusionFragment);
    case BuiltInShader::OcclusionBlurFragment:
        return LUDIFEX_SHADER_BYTECODE(LudifexOcclusionBlurFragment);
    case BuiltInShader::BloomPrefilterFragment:
        return LUDIFEX_SHADER_BYTECODE(LudifexBloomPrefilterFragment);
    case BuiltInShader::BloomDownFragment:
        return LUDIFEX_SHADER_BYTECODE(LudifexBloomDownFragment);
    case BuiltInShader::BloomUpFragment:
        return LUDIFEX_SHADER_BYTECODE(LudifexBloomUpFragment);
    case BuiltInShader::LuminanceFragment:
        return LUDIFEX_SHADER_BYTECODE(LudifexLuminanceFragment);
    case BuiltInShader::AdaptFragment:
        return LUDIFEX_SHADER_BYTECODE(LudifexAdaptFragment);
    case BuiltInShader::UpscaleFragment:
        return LUDIFEX_SHADER_BYTECODE(LudifexUpscaleFragment);
    case BuiltInShader::RayHybrid:
        return LUDIFEX_SHADER_BYTECODE(LudifexRayHybrid);
    case BuiltInShader::RayTemporal:
        return LUDIFEX_SHADER_BYTECODE(LudifexRayTemporal);
    case BuiltInShader::RayFilter:
        return LUDIFEX_SHADER_BYTECODE(LudifexRayFilter);
    case BuiltInShader::PathTrace:
        return LUDIFEX_SHADER_BYTECODE(LudifexPathTrace);
    default:
        return ShaderBytecode{};
    }
}

SDL_GPUShaderFormat BuiltInShaderFormats()
{
    SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_INVALID;
    for (SDL_GPUShaderFormat format :
         { SDL_GPU_SHADERFORMAT_DXIL, SDL_GPU_SHADERFORMAT_SPIRV, SDL_GPU_SHADERFORMAT_MSL })
    {
        bool complete = true;
        for (int shader = 0; shader < static_cast<int>(BuiltInShader::Count); ++shader)
        {
            const void* code = nullptr;
            size_t size = 0;
            complete = complete && PickShaderCode(GetBuiltInShader(static_cast<BuiltInShader>(shader)), format,
                                                  code, size);
        }
        if (complete)
        {
            formats |= format;
        }
    }
    return formats;
}

SDL_GPUDevice* CreateDevice(GraphicsBackend backend, bool debug)
{
    const SDL_GPUShaderFormat builtIn = BuiltInShaderFormats();

    // Offering SDL only the formats ludifex has shaders in is what keeps it from
    // choosing a backend nothing could be drawn on.
    if (backend == GraphicsBackend::Automatic)
    {
        SDL_GPUDevice* device = SDL_CreateGPUDevice(builtIn, debug, nullptr);
        if (device == nullptr)
        {
            LogMessage(LogLevel::Error, "gpu",
                       "SDL_CreateGPUDevice failed: %s. This usually means the graphics drivers are out of date, "
                       "or that none of the backends this build of ludifex has shaders for (%s%s%s) is present.",
                       SDL_GetError(), (builtIn & SDL_GPU_SHADERFORMAT_DXIL) ? "Direct3D 12 " : "",
                       (builtIn & SDL_GPU_SHADERFORMAT_SPIRV) ? "Vulkan " : "",
                       (builtIn & SDL_GPU_SHADERFORMAT_MSL) ? "Metal" : "");
        }
        return device;
    }

    const SDL_GPUShaderFormat format = BackendShaderFormat(backend);
    if ((builtIn & format) == 0)
    {
        LogMessage(LogLevel::Error, "gpu", "%s was asked for, and this build of ludifex has no shaders for it. %s",
                   GetGraphicsBackendName(backend), HowToBuild(backend));
        return nullptr;
    }

    SDL_GPUDevice* device = nullptr;
    {
        DriverOverride strict(DriverName(backend));
        device = SDL_CreateGPUDevice(format, debug, DriverName(backend));
    }
    if (device == nullptr)
    {
        LogMessage(LogLevel::Error, "gpu",
                   "%s was asked for, and this machine cannot run it: %s. Leave SetGraphicsBackend at "
                   "Automatic to use whichever backend it can.",
                   GetGraphicsBackendName(backend), SDL_GetError());
    }
    return device;
}

GraphicsBackend DeviceBackend(SDL_GPUDevice* device)
{
    const char* driver = device != nullptr ? SDL_GetGPUDeviceDriver(device) : nullptr;
    for (GraphicsBackend backend : { GraphicsBackend::Direct3D12, GraphicsBackend::Vulkan, GraphicsBackend::Metal })
    {
        if (driver != nullptr && SDL_strcasecmp(driver, DriverName(backend)) == 0)
        {
            return backend;
        }
    }
    return GraphicsBackend::Automatic;
}

SDL_GPUShaderFormat PredictShaderFormat(SDL_GPUDevice* device, GraphicsBackend requested)
{
    if (device != nullptr)
    {
        return DeviceShaderFormat(device);
    }
    if (requested != GraphicsBackend::Automatic)
    {
        return BackendShaderFormat(requested);
    }
    if (const char* driver = SDL_GetHint(SDL_HINT_GPU_DRIVER))
    {
        for (GraphicsBackend backend :
             { GraphicsBackend::Direct3D12, GraphicsBackend::Vulkan, GraphicsBackend::Metal })
        {
            if (SDL_strcasecmp(driver, DriverName(backend)) == 0)
            {
                return BackendShaderFormat(backend);
            }
        }
    }

    // SDL tries Metal, then Direct3D 12, then Vulkan, among the backends
    // ludifex has shaders for; the first a platform has is almost always the
    // one it runs.
    const SDL_GPUShaderFormat builtIn = BuiltInShaderFormats();
    for (SDL_GPUShaderFormat format :
         { SDL_GPU_SHADERFORMAT_MSL, SDL_GPU_SHADERFORMAT_DXIL, SDL_GPU_SHADERFORMAT_SPIRV })
    {
        if (builtIn & format)
        {
            return format;
        }
    }
    return SDL_GPU_SHADERFORMAT_INVALID;
}

SDL_GPUShaderFormat DeviceShaderFormat(SDL_GPUDevice* device)
{
    const SDL_GPUShaderFormat formats = device != nullptr ? SDL_GetGPUShaderFormats(device) : 0;
    for (SDL_GPUShaderFormat format :
         { SDL_GPU_SHADERFORMAT_DXIL, SDL_GPU_SHADERFORMAT_SPIRV, SDL_GPU_SHADERFORMAT_MSL })
    {
        if (formats & format)
        {
            return format;
        }
    }
    return SDL_GPU_SHADERFORMAT_INVALID;
}

const char* ShaderFormatName(SDL_GPUShaderFormat format)
{
    switch (format)
    {
    case SDL_GPU_SHADERFORMAT_DXIL:
        return "DXIL";
    case SDL_GPU_SHADERFORMAT_SPIRV:
        return "SPIR-V";
    case SDL_GPU_SHADERFORMAT_MSL:
        return "MSL";
    default:
        return "an unknown format";
    }
}

bool PickShaderCode(const ShaderBytecode& bytecode, SDL_GPUShaderFormat format, const void*& outCode,
                    size_t& outSize)
{
    switch (format)
    {
    case SDL_GPU_SHADERFORMAT_DXIL:
        outCode = bytecode.Dxil;
        outSize = bytecode.DxilSize;
        break;
    case SDL_GPU_SHADERFORMAT_SPIRV:
        outCode = bytecode.Spirv;
        outSize = bytecode.SpirvSize;
        break;
    case SDL_GPU_SHADERFORMAT_MSL:
        outCode = bytecode.Msl;
        outSize = bytecode.MslSize;
        break;
    default:
        outCode = nullptr;
        outSize = 0;
        break;
    }
    return outCode != nullptr && outSize > 0;
}

bool CompileMaterialSource(const std::string& sourcePath, const std::string& entryPoint,
                           SDL_GPUShaderFormat format, std::vector<unsigned char>& outCode,
                           std::string& outError)
{
    const char* shipped = " A shipped program passes Bytecode instead, compiled by ludifex_add_material.";

    const bool viaSpirv = format == SDL_GPU_SHADERFORMAT_SPIRV || format == SDL_GPU_SHADERFORMAT_MSL;
    if (format != SDL_GPU_SHADERFORMAT_DXIL && !viaSpirv)
    {
        outError = "the device takes a shader format ludifex cannot compile to";
        return false;
    }

    const std::string& compiler = viaSpirv ? FindSpirvCompiler() : FindDxilCompiler();
    if (compiler.empty())
    {
        outError = viaSpirv ? std::string("could not find a dxc with SPIR-V output to compile the material. The "
                                          "Vulkan SDK has one; install it, or set LUDIFEX_DXC_SPIRV to its dxc.") +
                                  shipped
                            : std::string("could not find dxc, the DirectX Shader Compiler, to compile the material. "
                                          "It ships with the Windows SDK; install that, or set LUDIFEX_DXC to "
                                          "dxc.exe.") +
                                  shipped;
        return false;
    }
    if (format == SDL_GPU_SHADERFORMAT_MSL && FindSpirvCross().empty())
    {
        outError = std::string("could not find spirv-cross, which turns the material into Metal's shading "
                               "language. The Vulkan SDK has it; install that, or set LUDIFEX_SPIRV_CROSS.") +
                   shipped;
        return false;
    }

    if (!std::filesystem::exists(sourcePath))
    {
        outError = "the shader file does not exist";
        return false;
    }

    // Written beside the user's preference directory rather than the source,
    // which may live somewhere read-only.
    char* prefPath = SDL_GetPrefPath("ludifex", "shadercache");
    if (prefPath == nullptr)
    {
        outError = "could not find a writable directory for compiled shaders";
        return false;
    }
    const std::string cacheDirectory = prefPath;
    SDL_free(prefPath);

    // Named for the whole path, not just the file name, so two materials
    // called glow.hlsl in different folders do not overwrite each other.
    std::error_code pathError;
    const std::string absoluteSource = std::filesystem::absolute(sourcePath, pathError).string();
    char pathHash[17];
    SDL_snprintf(pathHash, sizeof(pathHash), "%016llx",
                 static_cast<unsigned long long>(std::hash<std::string>{}(absoluteSource)));

    const char* extension = format == SDL_GPU_SHADERFORMAT_DXIL    ? ".dxil"
                            : format == SDL_GPU_SHADERFORMAT_SPIRV ? ".spv"
                                                                   : ".metal";
    const std::string outputPath = cacheDirectory + std::filesystem::path(sourcePath).filename().string() + "." +
                                   pathHash + "." + entryPoint + extension;

    const std::string includeDir = (std::filesystem::path(cacheDirectory) / "include").string();
    if (!WriteShaderIncludes(includeDir))
    {
        outError = "could not write the material includes beside the shader cache, in " + includeDir;
        return false;
    }

    std::vector<std::string> arguments = { compiler };
    if (viaSpirv)
    {
        arguments.insert(arguments.end(), { "-spirv", "-fspv-target-env=vulkan1.0" });
    }
    arguments.insert(arguments.end(), { "-T", "ps_6_0", "-E", entryPoint, "-I", includeDir, "-O3" });

    // Metal goes through SPIR-V, with the includes told to number buffers
    // Metal's way, and then through spirv-cross.
    const std::string compiledPath = format == SDL_GPU_SHADERFORMAT_MSL ? outputPath + ".spv" : outputPath;
    if (format == SDL_GPU_SHADERFORMAT_MSL)
    {
        arguments.insert(arguments.end(), { "-D", "LUDIFEX_METAL" });
    }
    arguments.insert(arguments.end(), { "-Fo", compiledPath, sourcePath });

    std::string message;
    if (RunTool(arguments, message) != 0)
    {
        // The compiler's own diagnostic carries the file, line, and column, so
        // it is passed through rather than summarized.
        outError = message.empty() ? "the shader compiler reported an error" : message;
        return false;
    }

    if (format == SDL_GPU_SHADERFORMAT_MSL)
    {
        const std::vector<std::string> translate = { FindSpirvCross(), compiledPath,       "--msl",
                                                     "--msl-version",  "20100",            "--msl-decoration-binding",
                                                     "--output",       outputPath };
        if (RunTool(translate, message) != 0)
        {
            outError = message.empty() ? "spirv-cross could not translate the material to Metal" : message;
            return false;
        }
    }

    if (!ReadWholeFile(outputPath, outCode))
    {
        outError = "the shader compiled but its output could not be read back";
        return false;
    }
    return true;
}

} // namespace detail

const char* GetGraphicsBackendName(GraphicsBackend backend)
{
    switch (backend)
    {
    case GraphicsBackend::Direct3D12:
        return "Direct3D 12";
    case GraphicsBackend::Vulkan:
        return "Vulkan";
    case GraphicsBackend::Metal:
        return "Metal";
    default:
        return "Automatic";
    }
}

bool IsGraphicsBackendAvailable(GraphicsBackend backend)
{
    const SDL_GPUShaderFormat builtIn = detail::BuiltInShaderFormats();
    const SDL_GPUShaderFormat format =
        backend == GraphicsBackend::Automatic ? builtIn : detail::BackendShaderFormat(backend);
    if ((builtIn & format) == 0)
    {
        return false;
    }

    // SDL asks the video subsystem which backends the platform has, so it is
    // started for the question when the program has not started it yet.
    const bool startVideo = !SDL_WasInit(SDL_INIT_VIDEO);
    if (startVideo && !SDL_InitSubSystem(SDL_INIT_VIDEO))
    {
        return false;
    }

    bool available = false;
    if (backend == GraphicsBackend::Automatic)
    {
        available = SDL_GPUSupportsShaderFormats(format, nullptr);
    }
    else
    {
        detail::DriverOverride strict(detail::DriverName(backend));
        available = SDL_GPUSupportsShaderFormats(format, detail::DriverName(backend));
    }

    if (startVideo)
    {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
    return available;
}

} // namespace ludifex
