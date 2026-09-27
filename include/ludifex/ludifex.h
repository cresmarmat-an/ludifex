// ludifex: 2D and 3D world library.
// Copyright (c) 2026 Cresmar Mat-an. MIT License; see LICENSE.
//
// Worlds and actors over Box2D (2D) and Box3D (3D), collision events, queries,
// joints, and characters; models and animation; a physically based renderer
// with optional ray and path tracing; and positional sound. Documentation:
// https://cresmarmat-an.github.io/ludifex/
//
// This header includes neither the physics libraries nor SDL.
// <ludifex/native.h> gives access to the physics handles when you need them.

#pragma once

#include <ludifex/version.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ludifex
{

// ---------------------------------------------------------------------------
// Math
//
// Right-handed, +Y up, in metres, kilograms, and seconds. This matches Box3D's
// convention and its default gravity of {0, -10, 0}.
// ---------------------------------------------------------------------------

struct Vec2
{
    float X = 0.0f;
    float Y = 0.0f;
};

struct Vec3
{
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

// Unit quaternion. Identity is {0, 0, 0, 1}.
struct Quat
{
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    float W = 1.0f;

    // A rotation of this many radians about an axis. The axis is normalized
    // for you.
    static Quat FromAxisAngle(Vec3 axis, float radians);

    // Yaw about Y, then pitch about X, then roll about Z.
    static Quat FromEuler(float yawRadians, float pitchRadians, float rollRadians);

    // The opposite rotation.
    Quat Inverse() const;

    // This rotation followed by next.
    Quat Then(const Quat& next) const;

    // The vector, rotated.
    Vec3 Rotate(Vec3 value) const;
};

struct Transform3
{
    Vec3 Position;
    Quat Rotation;
    Vec3 Scale{ 1.0f, 1.0f, 1.0f };
};

struct Transform2
{
    Vec2 Position;
    float Rotation = 0.0f;
    Vec2 Scale{ 1.0f, 1.0f };
};

struct Color
{
    float R = 1.0f;
    float G = 1.0f;
    float B = 1.0f;
    float A = 1.0f;

    static Color FromBytes(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
    {
        return Color{ r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f };
    }
};

// Column-major 4x4, matching what the shaders expect.
struct Mat4
{
    float M[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

    static Mat4 Identity() { return Mat4{}; }
    static Mat4 Perspective(float fieldOfViewRadians, float aspect, float nearPlane, float farPlane);
    static Mat4 Orthographic(float left, float right, float bottom, float top, float nearPlane,
                             float farPlane);
    static Mat4 LookAt(Vec3 eye, Vec3 target, Vec3 up);
    static Mat4 FromTransform(const Transform3& transform);

    Mat4 operator*(const Mat4& other) const;
};

// ---------------------------------------------------------------------------
// Assets
//
// A relative name such as "robot.gltf" is searched for, in order, under each
// root you add (most recent first), then under the working directory's ./,
// ./assets/, and ./assets/models|images|sounds|music|shaders|fonts/, then the
// same folders beside the executable. The first match wins and is cached. A
// name that matches nothing is logged with every path that was tried.
// Absolute paths are used as they are.
//
// Every loader in ludifex uses this: models, textures, sounds, music, and
// material shaders.
// ---------------------------------------------------------------------------

void AddAssetRoot(const std::string& directory);
void ClearAssetRoots();

// The roots in search order, user roots first. The executable-relative copies
// of the defaults are implied rather than listed.
std::vector<std::string> GetAssetRoots();

// The file a name resolves to, or an empty string (with a report of every
// path tried) when there is none.
std::string ResolveAssetPath(const std::string& path);

// ---------------------------------------------------------------------------
// Textures
//
// Images for models, actors, sprites, and backgrounds. Like materials they are
// process-wide, so one texture can be shared by actors in several worlds. An
// image is decoded once, uploaded with a full mip chain the first time it is
// drawn, and sampled with trilinear 8x anisotropic filtering, so distant
// textured surfaces do not shimmer.
// ---------------------------------------------------------------------------

struct TextureId
{
    uint32_t Index = 0;
    uint32_t Generation = 0;

    bool IsValid() const { return Generation != 0; }

    friend bool operator==(const TextureId& a, const TextureId& b)
    {
        return a.Index == b.Index && a.Generation == b.Generation;
    }
    friend bool operator!=(const TextureId& a, const TextureId& b) { return !(a == b); }
};

// What the pixels of a texture mean, which decides how they are stored and
// how their smaller copies are made.
enum class TextureUsage
{
    // Colour, authored in sRGB: sampled as linear light, and averaged in
    // linear light when shrunk. Base colour and emission.
    Color,

    // Numbers stored in an image, such as roughness, metalness, occlusion, or
    // a mask. Sampled and averaged as stored.
    Data,

    // A tangent-space normal map. Stored like data, and each smaller copy is
    // renormalized so a distant bumpy surface does not go dim.
    Normal
};

// PNG, JPEG, BMP, TGA, GIF (first frame), PSD, and HDR, found through the asset
// roots. The same name returns the same texture. A file that cannot be found
// or decoded returns a magenta-and-black checkerboard and logs why.
TextureId LoadTexture(const std::string& path, TextureUsage usage = TextureUsage::Color);

// From pixels you already have: four bytes per pixel, straight alpha, rows top
// to bottom. The pixels are copied. The usage says what they are, as for
// LoadTexture: a normal map made in code wants TextureUsage::Normal.
TextureId CreateTexture(int width, int height, const void* rgbaPixels, TextureUsage usage = TextureUsage::Color);

void DestroyTexture(TextureId texture);

// In pixels. Zero for an invalid texture.
Vec2 GetTextureSize(TextureId texture);

// ---------------------------------------------------------------------------
// Materials
//
// A material is a custom fragment shader plus a uniform block. Only the
// actor's pixels change: it is still instanced, culled, and moved by physics
// as before, because the vertex stage stays ludifex's.
//
// Author the shader against shaders/world_material.hlsli. Call Shade() inside
// it to keep the built-in lighting (sun, shadows, point lights, ambient light,
// and fog) while choosing your own colour, or skip it and light the surface
// yourself.
//
// The same kind of material also runs as a full-screen pass: author it against
// shaders/world_post.hlsli and hand it to AddPostProcess.
//
// Materials are process-wide, not per world, so one material can be shared by
// actors in several worlds.
// ---------------------------------------------------------------------------

struct MaterialId
{
    uint32_t Index = 0;
    uint32_t Generation = 0;

    bool IsValid() const { return Generation != 0; }

    friend bool operator==(const MaterialId& a, const MaterialId& b)
    {
        return a.Index == b.Index && a.Generation == b.Generation;
    }
    friend bool operator!=(const MaterialId& a, const MaterialId& b) { return !(a == b); }
};

// One named value. The order these are declared in defines which Params[] slot
// each name occupies in the shader.
//
// A value given as a Color is authored the way every colour in ludifex is, in
// sRGB, and reaches the shader converted to linear light, the space lighting is
// computed in. A value given as numbers reaches the shader unchanged.
struct MaterialUniform
{
    std::string Name;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    float W = 0.0f;
    bool IsColor = false;

    MaterialUniform() = default;
    MaterialUniform(std::string name, float x, float y = 0.0f, float z = 0.0f, float w = 0.0f)
        : Name(std::move(name)), X(x), Y(y), Z(z), W(w)
    {
    }
    MaterialUniform(std::string name, Color color)
        : Name(std::move(name)), X(color.R), Y(color.G), Z(color.B), W(color.A), IsColor(true)
    {
    }
};

// A compiled shader in each backend's format. ludifex_add_material fills one
// in for every format the build makes; a format left empty is one the
// material cannot be drawn with.
struct ShaderBytecode
{
    const void* Dxil = nullptr; // Direct3D 12
    size_t DxilSize = 0;
    const void* Spirv = nullptr; // Vulkan
    size_t SpirvSize = 0;
    const void* Msl = nullptr; // Metal, as source text
    size_t MslSize = 0;

    bool IsEmpty() const { return DxilSize == 0 && SpirvSize == 0 && MslSize == 0; }
};

struct MaterialDesc
{
    // Development path: an .hlsl file compiled when the material is created,
    // found through the asset roots, for whichever backend draws it. Saving
    // the file recompiles it in place; a compile error keeps the working
    // shader and reports the file, line, and column.
    std::string ShaderPath;

    // Shipping path: bytecode compiled at build time, so no shader compiler is
    // needed at runtime. The function that ludifex_add_material generates
    // returns it. Takes precedence over ShaderPath when it holds any format.
    ShaderBytecode Bytecode;

    std::string EntryPoint = "FragmentMain";

    // At most eight. Order defines the Params[] slots.
    std::vector<MaterialUniform> Uniforms;

    // An opaque material turns alpha below 1 into coverage, for cutouts and
    // dissolves. A transparent one is blended instead: drawn after everything
    // opaque, sorted back to front, without writing depth. Use it for glass,
    // smoke, and fades.
    bool Transparent = false;

    bool HotReload = true;
};

// On failure the returned id is invalid and the compiler's message has gone to
// the log.
MaterialId CreateMaterial(const MaterialDesc& desc);
void DestroyMaterial(MaterialId material);

// Setting a name the material did not declare is a no-op with a warning.
void SetMaterialUniform(MaterialId material, const std::string& name, float x, float y = 0.0f,
                        float z = 0.0f, float w = 0.0f);
void SetMaterialUniform(MaterialId material, const std::string& name, Color color);

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

struct Camera3D
{
    Vec3 Position{ 0.0f, 6.0f, 14.0f };
    Vec3 Target{ 0.0f, 1.0f, 0.0f };
    Vec3 Up{ 0.0f, 1.0f, 0.0f };

    float FieldOfViewDegrees = 55.0f;
    float NearPlane = 0.1f;
    float FarPlane = 500.0f;
};

struct Camera2D
{
    Vec2 Center;

    // How many world units are visible vertically. The horizontal extent
    // follows from the view's aspect ratio, so a resize widens the view rather
    // than stretching it.
    float Height = 20.0f;
};

struct DirectionalLight
{
    Vec3 Direction{ -0.45f, -1.0f, -0.35f };
    Color Tint{ 1.0f, 0.98f, 0.94f, 1.0f };
    float Intensity = 1.0f;
};

// The anti-aliasing presets. Every preset keeps the levels that cost little or
// nothing on: mip-mapped anisotropic texture filtering, alpha-to-coverage on
// cutouts, specular anti-aliasing on curved surfaces, and analytic smoothing
// of shader-drawn edges. What a preset chooses is the geometric level
// (multisampling) and the final-image level (a post-process pass).
//
//   Preset      Multisampling   Final image
//   Off         none            none
//   Fast        none            FXAA
//   Balanced    4x              none        <- the default
//   High        8x              FXAA
//   Temporal    2x              TAA
//
// Multisampling falls back to the highest count the device supports.
enum class AntiAliasing
{
    Off,
    Fast,
    Balanced,
    High,
    Temporal
};

// Shadows from the directional light, rendered into a map that follows the
// camera. Distance is how far from the camera shadows reach; a shorter
// distance spends the same map on less ground, so shadows get sharper.
struct ShadowSettings
{
    bool Enabled = true;
    float Distance = 60.0f;

    // The size of each map, in texels a side.
    int Resolution = 2048;

    // Width of the filtered edge, in shadow-map texels. Larger is softer.
    float Softness = 1.5f;

    // How many slices the distance is cut into, 1 to 4, each with a map of
    // its own. The nearest slice is the smallest, so nearby shadows are the
    // sharpest while distant ones still appear.
    int Cascades = 3;
};

// Linear distance fog, applied by Shade() so custom materials that call it
// fog the same way the built-in shader does.
struct FogSettings
{
    bool Enabled = false;
    Color Tint = Color::FromBytes(24, 28, 38);
    float Start = 30.0f;
    float End = 150.0f;
};

// Level of detail: an object that covers only a small part of the screen is
// drawn from a simpler mesh. The thresholds are the fraction of the view's
// height the object covers.
struct DetailSettings
{
    bool Enabled = true;

    // Below this much of the screen, a simpler mesh; below Simplest, the
    // simplest one there is.
    float Simpler = 0.05f;
    float Simplest = 0.015f;
};

// Screen-space ambient occlusion: creases, corners, and the ground beneath
// objects darken where light from the sky would be blocked. It is computed
// each frame from the depth buffer.
struct AmbientOcclusionSettings
{
    bool Enabled = true;

    // How far a surface looks for whatever hides it, in world units.
    float Radius = 0.5f;
    float Intensity = 1.0f;

    // Samples per pixel, 4 to 32. More is smoother and costs more.
    int Samples = 12;
};

// Light brighter than the display can show spreads into its surroundings, as
// it does through a camera lens: a lamp glows and a reflection of the sun
// flares. Light at ordinary brightness is not affected.
struct BloomSettings
{
    bool Enabled = true;

    // The linear brightness the glow begins at, with a soft start instead of
    // a hard cut.
    float Threshold = 1.2f;
    float Intensity = 0.35f;

    // How far the glow spreads, relative to its natural reach.
    float Radius = 1.0f;
};

// Exposure that adapts to the brightness of the view, like an eye adjusting
// when walking from sunlight into a cave. Exposure in the render settings
// still scales the result.
struct AutoExposureSettings
{
    bool Enabled = false;

    // In stops: 1 is twice as bright as the eye would settle on, -1 half.
    float Compensation = 0.0f;

    // The furthest it may go, darkening and brightening.
    float Minimum = 0.25f;
    float Maximum = 4.0f;

    // How quickly it adapts. Higher is faster.
    float Speed = 1.5f;
};

// How light becomes the colours on screen.
//
//   Neutral   values below 0.9 stay as authored, and brighter light rolls
//             off smoothly so highlights soften instead of clipping. The
//             colour you choose is the colour you see.
//   Filmic    the ACES filmic curve, with a toe and a shoulder: deeper
//             shadows, richer colour, and highlights that fade toward white.
enum class ToneCurve
{
    Neutral,
    Filmic
};

// The last adjustments to the image, applied after tone mapping. Every field
// at its default changes nothing.
struct ColorGrading
{
    ToneCurve Curve = ToneCurve::Neutral;
    float Contrast = 1.0f;
    float Saturation = 1.0f;

    // White balance: -1 cooler to 1 warmer, and -1 greener to 1 more magenta.
    float Temperature = 0.0f;
    float Tint = 0.0f;

    // Lift is added to the shadows and gain multiplies the highlights.
    Color Lift{ 0.0f, 0.0f, 0.0f, 1.0f };
    Color Gain{ 1.0f, 1.0f, 1.0f, 1.0f };

    // Darkens toward the corners, 0 none to 1 strong.
    float Vignette = 0.0f;
};

// A sky drawn from three colours and the sun, in place of the flat SkyColor.
// It lights the world too, in place of AmbientColor: surfaces take their
// ambient light from the sky above them and the ground below, and glossy ones
// reflect it. The sun is drawn where the directional light comes from.
struct SkySettings
{
    bool Enabled = false;
    Color Zenith = Color::FromBytes(46, 98, 186);
    Color Horizon = Color::FromBytes(172, 200, 228);
    Color Ground = Color::FromBytes(62, 58, 54);

    // The size of the sun's disc, 1 as it appears from Earth; 0 hides it.
    float SunSize = 1.0f;
    float Brightness = 1.0f;
};

// Ray tracing in compute shaders. It needs no ray-tracing hardware, so it runs
// on every device ludifex supports, but it is expensive: each effect traces
// one ray per pixel at a fraction of the render resolution, and the result is
// smoothed over time and space. Where the device cannot run it, the rasterized
// effects are used instead and World3D::IsRayTracingActive returns false.
struct RayTracingSettings
{
    // Traced sun shadows instead of shadow maps: accurate at any distance,
    // with no cascades or bias, and softer the farther a shadow falls from
    // what casts it.
    bool Shadows = false;

    // Ambient occlusion traced against the world instead of the screen, so a
    // corner darkens even when what blocks it is off screen.
    bool AmbientOcclusion = false;

    // Glossy surfaces reflect the world, including what the camera cannot see.
    bool Reflections = false;

    // Rays are traced at this fraction of the render resolution.
    float ResolutionScale = 0.5f;

    // The sun's apparent diameter in degrees. Wider softens every shadow.
    float SunAngle = 1.0f;

    // How far occlusion rays look, in world units.
    float OcclusionRadius = 1.5f;

    // Surfaces rougher than this reflect the sky and the ambient light
    // instead of tracing.
    float MaxRoughness = 0.6f;
};

// Path tracing: the whole image computed by following light as it bounces,
// instead of rasterizing. Soft shadows, colour bleeding between surfaces,
// reflections of reflections, and light from emissive surfaces all come from
// the same method. The image accumulates while the camera and the world are
// still, and starts over when anything moves. It is meant for stills,
// previews, and as a lighting reference, not for real-time play.
struct PathTracingSettings
{
    bool Enabled = false;

    // How many times light may bounce on its way to the camera.
    int MaxBounces = 4;
    int SamplesPerFrame = 1;

    // Once this many samples have accumulated, the image is left as it is.
    int MaxSamples = 1024;
};

// A world starts at the High preset, below.
struct RenderSettings
{
    Color SkyColor = Color::FromBytes(24, 28, 38);
    Color AmbientColor = Color::FromBytes(96, 106, 130);
    DirectionalLight Light;
    AntiAliasing Mode = AntiAliasing::Balanced;
    ShadowSettings Shadows;
    FogSettings Fog;
    DetailSettings Detail;

    // The resolution the world is drawn at, relative to its target, from 0.25
    // to 2. Below 1 is faster and softer, and scaled up at the end; above 1
    // draws more pixels than are shown and scales them down, which gives the
    // best anti-aliasing at the highest cost.
    float RenderScale = 1.0f;

    AmbientOcclusionSettings AmbientOcclusion;
    BloomSettings Bloom;
    SkySettings Sky;
    ColorGrading Grading;

    // Scales the whole image before tone mapping. Lighting is computed in
    // linear light with room above 1, and the tone curve decides what becomes
    // of the light brighter than the display can show.
    float Exposure = 1.0f;
    AutoExposureSettings AutoExposure;

    RayTracingSettings RayTracing;
    PathTracingSettings PathTracing;
};

// Graphics quality presets. Each one sets how much work the renderer does
// (render scale, anti-aliasing, shadows, ambient occlusion, bloom, and level
// of detail) and leaves the look alone: colours, lights, fog, sky, grading,
// and exposure are unchanged. Ray tracing and path tracing are not changed
// either and can be used with any preset.
//
//   Preset   Scale  Anti-aliasing  Shadows (per map, maps, reach)  Occlusion  Bloom  Detail
//   Potato    50%   Off            none                            none       no     least
//   Low       75%   Fast           1024, 1, 30 m                   none       no     less
//   Medium   100%   Balanced       2048, 2, 40 m                   8 samples  yes    usual
//   High     100%   Balanced       2048, 3, 60 m                   12         yes    usual   <- the default
//   Ultra    100%   High           2048, 4, 90 m, softer           16         yes    more
//   Extreme  150%   Balanced       4096, 4, 120 m, softer          24         yes    always full
enum class GraphicsQuality
{
    Potato,
    Low,
    Medium,
    High,
    Ultra,
    Extreme
};

// Sets the quality fields of settings to the preset's, leaving everything else.
void ApplyGraphicsQuality(RenderSettings& settings, GraphicsQuality quality);

// "Potato", "Low", "Medium", "High", "Ultra", or "Extreme".
const char* GetGraphicsQualityName(GraphicsQuality quality);

// Where a full-screen material runs in the frame.
enum class PassPoint
{
    // On the scene in linear light, before tone mapping: bloom, colour
    // grading in linear space, outlines that read depth.
    BeforeToneMap,

    // On the final image, after tone mapping and anti-aliasing: vignettes,
    // film grain, screen-space overlays. The default.
    AfterToneMap
};

// A light that shines in every direction from a point, falling off with
// distance and reaching nothing beyond Range.
struct PointLightDesc
{
    Vec3 Position;
    Color Tint{ 1.0f, 0.92f, 0.80f, 1.0f };
    float Intensity = 4.0f;
    float Range = 10.0f;
};

// ---------------------------------------------------------------------------
// Reloading assets while the program runs
//
// Custom shaders reload on their own, per material. This does the same for
// models and textures: save a .gltf or a .png and the scene updates without a
// restart, and every actor keeps its place.
//
// It checks one timestamp per watched file every few frames and reads a file
// only when its timestamp changes. It is off by default and meant for
// development builds. A file saved again without changes is detected and
// ignored, because the contents are compared, not only the timestamp.
//
// A file that fails to load keeps the version that worked, and the error is
// logged.
// ---------------------------------------------------------------------------

void SetAssetHotReload(bool enabled);
bool IsAssetHotReloadEnabled();

// ---------------------------------------------------------------------------
// Measuring
//
// Where the time in a frame went, and how busy the worker threads were.
// Profiling is off by default and costs nothing while it is off.
// ---------------------------------------------------------------------------

// One named stretch of a frame: how long it took and how many times it ran.
struct ProfileSection
{
    const char* Name = "";
    float Milliseconds = 0.0f;
    int Calls = 0;
};

// Off by default. While on, the library times its own work (stepping,
// culling, drawing, and sound) and returns the last frame's sections.
void SetProfilingEnabled(bool enabled);
bool IsProfilingEnabled();

// Last complete frame, in the order the sections first ran.
std::vector<ProfileSection> GetProfile();

// How busy each worker in the shared scheduler has been, from 0 to 1, since
// the last call. Empty when the scheduler is not running. The first entry is
// the thread that started it, which does its share of the work while it waits.
std::vector<float> GetWorkerUtilisation();

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

enum class LogLevel
{
    Trace,
    Info,
    Warning,
    Error
};

using LogHandler = std::function<void(LogLevel level, const char* category, const char* message)>;

void SetLogHandler(LogHandler handler);
void SetMinimumLogLevel(LogLevel level);
void LogMessage(LogLevel level, const char* category, const char* format, ...);

// ---------------------------------------------------------------------------
// Handles
//
// Every object you hold is an index plus a generation counter. Destroying an
// object increments its generation, so a handle kept past the object's
// lifetime is detected as stale: using it does nothing and logs a diagnostic,
// instead of crashing or corrupting memory.
// ---------------------------------------------------------------------------

struct ActorId
{
    uint32_t Index = 0;
    uint32_t Generation = 0;

    // Generation 0 is reserved to mean "never referred to anything".
    bool IsValid() const { return Generation != 0; }

    friend bool operator==(const ActorId& a, const ActorId& b)
    {
        return a.Index == b.Index && a.Generation == b.Generation;
    }

    friend bool operator!=(const ActorId& a, const ActorId& b) { return !(a == b); }
};

struct LightId
{
    uint32_t Index = 0;
    uint32_t Generation = 0;

    bool IsValid() const { return Generation != 0; }

    friend bool operator==(const LightId& a, const LightId& b)
    {
        return a.Index == b.Index && a.Generation == b.Generation;
    }
    friend bool operator!=(const LightId& a, const LightId& b) { return !(a == b); }
};

enum class BodyType
{
    Static,
    Dynamic,
    Kinematic
};

// Automatic: the world steps physics itself from a fixed-timestep accumulator
// inside Update(). Manual: the world steps only when StepPhysics() is called.
enum class PhysicsMode
{
    Automatic,
    Manual
};

class Actor3D;
class Actor2D;

namespace detail
{
struct World3DState;
struct World2DState;

// Builds a handle to an actor without a world in hand.
Actor3D MakeActor(World3DState* state, const ActorId& id);
Actor2D MakeActor(World2DState* state, const ActorId& id);
}

// ---------------------------------------------------------------------------
// Host
//
// ludifex can create and own its GPU device, so a program can use it without
// opane. When another library already owns one (opane or an existing engine),
// hand it over here instead, so the process uses a single device.
//
// This is a plain struct of opaque pointers; neither library includes the
// other's headers.
// ---------------------------------------------------------------------------

struct Host
{
    void* GpuDevice = nullptr; // SDL_GPUDevice*
    void* Window = nullptr;    // SDL_Window*, optional
};

// Adopts an existing device. Call before creating a world that will render.
void AdoptHost(const Host& host);

// True once a device is available, whether adopted or created here.
bool IsRenderingAvailable();

// The graphics API worlds are drawn with.
enum class GraphicsBackend
{
    // The first this machine runs, tried in the order Metal, Direct3D 12,
    // Vulkan: Direct3D 12 on Windows, falling back to Vulkan where Direct3D 12
    // is missing; Metal on Apple hardware; Vulkan on Linux. The SDL_GPU_DRIVER
    // environment variable ("direct3d12", "vulkan", or "metal") overrides the
    // order, so another backend can be tried without rebuilding.
    Automatic,
    Direct3D12,
    Vulkan,
    Metal,
};

// The backend ludifex creates its own device on. Anything but Automatic is
// strict: when this machine cannot run it, rendering is unavailable and the
// log says why, instead of falling back to another backend. Call before
// the first world that renders; a device that already exists, or one adopted
// from a host such as opane, is not changed.
void SetGraphicsBackend(GraphicsBackend backend);

// The backend of the device in use, or Automatic when there is none yet.
GraphicsBackend GetGraphicsBackend();

// "Automatic", "Direct3D 12", "Vulkan", or "Metal", for a settings screen.
const char* GetGraphicsBackendName(GraphicsBackend backend);

// True when this build of ludifex carries shaders for the backend and this
// machine can run it. Automatic asks whether any backend is usable. Cheap
// enough for a settings screen, not for every frame.
bool IsGraphicsBackendAvailable(GraphicsBackend backend);

// ---------------------------------------------------------------------------
// Audio
//
// Worlds play sounds at a point or on an actor, attenuated by distance from a
// listener that follows the camera, with optional Doppler shift, directional
// cones, and occlusion by the world.
//
// There is one output for the whole process. After opane::StartApp, a world's
// sound is mixed into opane's audio output with the interface's; on its own,
// ludifex opens the audio device itself. Every change of volume (a voice
// starting or stopping, a fade, or a voice culled for distance or displaced
// by a more important one) is ramped to avoid clicks.
// ---------------------------------------------------------------------------

struct SoundId
{
    uint32_t Index = 0;
    uint32_t Generation = 0;

    bool IsValid() const { return Generation != 0; }

    friend bool operator==(const SoundId& a, const SoundId& b)
    {
        return a.Index == b.Index && a.Generation == b.Generation;
    }
    friend bool operator!=(const SoundId& a, const SoundId& b) { return !(a == b); }
};

// One playing instance of a sound.
struct VoiceId
{
    uint32_t Index = 0;
    uint32_t Generation = 0;

    bool IsValid() const { return Generation != 0; }

    friend bool operator==(const VoiceId& a, const VoiceId& b)
    {
        return a.Index == b.Index && a.Generation == b.Generation;
    }
    friend bool operator!=(const VoiceId& a, const VoiceId& b) { return !(a == b); }
};

// WAV, FLAC, MP3, and Ogg Vorbis, found through the asset roots, decoded once
// and kept in memory. A file that cannot be loaded gives an invalid id and a
// warning; playing that id does nothing.
SoundId LoadSound(const std::string& path);
void DestroySound(SoundId sound);

// Length in seconds. Zero for an invalid sound.
float GetSoundDuration(SoundId sound);

struct SoundSettings
{
    float Volume = 1.0f;
    float Pitch = 1.0f;
    bool Looping = false;

    // Full volume within MinDistance, falling off inversely to silence at
    // MaxDistance. A voice beyond MaxDistance costs nothing: a one-shot that
    // starts out of range is not started, and a looping one is kept in step
    // silently until it comes back in range.
    float MinDistance = 1.0f;
    float MaxDistance = 60.0f;
    float Rolloff = 1.0f;

    // A directional emitter, such as a loudspeaker: full volume within
    // ConeInnerDegrees of the way it faces, ConeOuterGain beyond
    // ConeOuterDegrees, blended between. 360 sounds the same all round. An
    // actor faces down its own -Z.
    float ConeInnerDegrees = 360.0f;
    float ConeOuterDegrees = 360.0f;
    float ConeOuterGain = 0.0f;

    // How strongly motion shifts the pitch; 0 turns the Doppler effect off.
    float Doppler = 1.0f;

    // Whether solid things between the listener and the sound muffle it.
    bool Occlusion = false;

    // When more voices want to play than MaxVoices allows, higher priority
    // wins; among equals, the more audible one.
    int Priority = 0;
};

// How a world's sound is mixed.
struct AudioSettings
{
    float Volume = 1.0f;

    // Voices mixed at once. Beyond this the least important give way, and a
    // looping voice that gives way keeps its place silently to resume later.
    int MaxVoices = 32;

    // Metres per second, for the Doppler effect.
    float SpeedOfSound = 343.0f;

    // The listener sits at the camera and faces where it faces. Turn this off
    // to place it yourself with SetListener, for example on a character
    // instead of a chase camera.
    bool ListenerFollowsCamera = true;

    // How much an occluded sound is turned down, 0 to 1.
    float OcclusionGain = 0.35f;
};

// What a world's audio is doing, for a readout or a test.
struct AudioStats
{
    int Playing = 0;     // voices being mixed
    int Virtual = 0;     // looping voices kept in step silently
    int Culled = 0;      // one-shots never started because they were out of range
    int Stolen = 0;      // voices that gave way to more important ones
    int Occluded = 0;    // voices currently muffled
};

// Where the audio goes. Automatic is the device (or opane's mix, after
// StartApp). Manual mixes nothing on its own: the program pulls the mix with
// MixAudio, to record it, stream it, or test it. Choose before any sound plays.
enum class AudioOutput
{
    Automatic,
    Manual
};

struct AudioConfig
{
    AudioOutput Output = AudioOutput::Automatic;

    // Used for Manual output; Automatic follows the device.
    uint32_t SampleRate = 48000;
    uint32_t Channels = 2;
};

void ConfigureAudio(const AudioConfig& config);

// Manual output only: mixes the next frameCount frames, interleaved 32-bit
// float, into frames. Returns false when audio is not in manual mode.
bool MixAudio(float* frames, uint32_t frameCount);

// Grants <ludifex/native.h> access to the underlying Box2D and Box3D handles.
// Declared here as a tag so the physics libraries stay out of this header.
namespace native
{
struct Accessor;
}

// ===========================================================================
// 3D
// ===========================================================================

struct World3DConfig
{
    Vec3 Gravity{ 0.0f, -10.0f, 0.0f };

    // Physics advances in fixed increments regardless of frame rate. Rendering
    // blends between the previous and current transforms by the leftover
    // fraction, so fixed-step motion looks smooth.
    float FixedTimeStep = 1.0f / 60.0f;

    // Sub-steps give each constraint several chances to react within one step.
    // Four at 60 Hz runs constraints at 240 Hz internally. Raise it first when
    // stacks jitter.
    int SubStepCount = 4;

    // Caps how many steps a single Update() may run, so a slow frame cannot
    // ask for more steps and slow the next frame further.
    int MaxStepsPerFrame = 8;

    bool PhysicsEnabled = true;
    bool EnableSleep = true;
    bool EnableContinuous = true;

    // Pins the worker count to one and fixes the step, so the same inputs give
    // the same results on every run.
    bool Deterministic = false;

    uint32_t WorkerCount = 1;
};

struct BoxDesc
{
    Vec3 Scale{ 1.0f, 1.0f, 1.0f }; // full extents, in meters
    Vec3 Position;
    Quat Rotation;
    BodyType Type = BodyType::Dynamic;
    float Density = 1.0f;
    float Friction = 0.3f;
    float Restitution = 0.0f;
    bool IsBullet = false;
    bool IsSensor = false;
    std::string Name;
};

struct SphereDesc
{
    float Radius = 0.5f;
    Vec3 Position;
    BodyType Type = BodyType::Dynamic;
    float Density = 1.0f;
    float Friction = 0.3f;
    float Restitution = 0.2f;
    bool IsBullet = false;
    bool IsSensor = false;
    std::string Name;
};

struct CapsuleDesc
{
    float Radius = 0.5f;
    float Height = 2.0f; // total height including both caps
    Vec3 Position;
    BodyType Type = BodyType::Dynamic;
    float Density = 1.0f;
    float Friction = 0.3f;
    float Restitution = 0.0f;
    bool IsSensor = false;
    std::string Name;
};

struct ModelDesc
{
    // A model file: glTF (.gltf, .glb) or any format Assimp reads, including
    // FBX, OBJ, Collada, 3DS, Blender, PLY, and STL. Buffers and textures may
    // be external or embedded.
    std::string Path;

    // Parses the file on a background thread. The actor appears immediately
    // as a plain box and takes the model's shape when it arrives, so loading
    // many models does not stall a frame.
    //
    // The collider follows the model's bounds once it arrives, so an actor
    // that has to be exact from the first frame should load in the
    // foreground.
    bool LoadInBackground = false;

    Vec3 Position;
    Quat Rotation;

    // Uniform scale applied to the model as authored.
    float Scale = 1.0f;

    BodyType Type = BodyType::Dynamic;
    float Density = 1.0f;
    float Friction = 0.3f;
    float Restitution = 0.0f;
    std::string Name;
};

struct GroundDesc
{
    float Width = 50.0f;
    float Depth = 50.0f;
    float Thickness = 1.0f;

    // The ground's top surface sits at this height, so the default puts the
    // walkable surface at y = 0 rather than at the slab's center.
    Vec3 Position;
    float Friction = 0.6f;
    std::string Name = "Ground";
};

struct CollisionInfo;
struct TriggerInfo;

// Delivered at a sync point rather than from inside the solver, so a handler
// may freely create and destroy actors.
using CollisionHandler = std::function<void(const CollisionInfo&)>;
using TriggerHandler = std::function<void(const TriggerInfo&)>;

// How to start a clip, with named fields:
// Play({ .Name = "run", .Fade = 0.15f }).
struct AnimationPlay
{
    // The clip, by the name its glTF file gave it. Leave it empty and set
    // Index instead to play by position.
    const char* Name = nullptr;
    int Index = -1;

    // A clip that does not loop holds its last pose.
    bool Loop = true;

    // Multiplies the clip's own timing. Negative runs it backwards.
    float Speed = 1.0f;

    // Where in the clip to begin, in seconds.
    float StartTime = 0.0f;

    // Seconds to crossfade from whatever is playing now. Zero cuts instantly.
    float Fade = 0.2f;

    // How much of the final pose this clip accounts for once it has faded in.
    // Weights are normalized across everything playing, so two clips at 1.0
    // mix half and half rather than doubling.
    float Weight = 1.0f;
};

// A handle-backed view onto one actor in a World3D.
//
// Copying an Actor3D copies the handle, not the actor. An Actor3D whose actor
// has been destroyed reports IsValid() == false; calling through it does
// nothing and logs a diagnostic.
class Actor3D
{
public:
    Actor3D() = default;

    bool IsValid() const;
    ActorId GetId() const { return m_Id; }

    // Two handles are equal when they refer to the same actor.
    friend bool operator==(const Actor3D& a, const Actor3D& b) { return a.m_Id == b.m_Id; }
    friend bool operator!=(const Actor3D& a, const Actor3D& b) { return !(a == b); }
    const std::string& GetName() const;
    void SetName(const std::string& name);

    // Immediate operations. These take effect at once and are visible to the
    // next read, per the mutability contract.
    Vec3 GetPosition() const;
    void SetPosition(Vec3 position);
    Quat GetRotation() const;
    void SetRotation(Quat rotation);
    Vec3 GetScale() const;

    Vec3 GetLinearVelocity() const;
    void SetLinearVelocity(Vec3 velocity);
    Vec3 GetAngularVelocity() const;
    void SetAngularVelocity(Vec3 velocity);

    void ApplyForce(Vec3 force);
    void ApplyImpulse(Vec3 impulse);
    void ApplyTorque(Vec3 torque);

    float GetMass() const;
    bool IsAwake() const;
    void SetAwake(bool awake);

    // The transform blended between the previous and current physics steps by
    // the world's interpolation alpha. This is what a renderer should read.
    Transform3 GetInterpolatedTransform() const;

    // Appearance. These affect rendering only and never the simulation.
    //
    // An alpha below 1 makes the actor translucent: it is drawn after
    // everything opaque, sorted back to front, and blended.
    Color GetColor() const;
    void SetColor(Color color);
    bool IsVisible() const;
    void SetVisible(bool visible);

    // The image on the actor's surface, multiplied by its colour. A model
    // starts with the texture its glTF file names, a primitive with none.
    // Pass an empty TextureId to return to that.
    void SetTexture(TextureId texture);
    TextureId GetTexture() const;

    // Repeats the texture across each face ({10, 10} tiles it ten times each
    // way) and shifts it by offset, in texture widths.
    void SetTextureTiling(Vec2 repeat, Vec2 offset = {});

    // A tangent-space normal map, loaded with TextureUsage::Normal: the
    // surface's small bumps and grooves, lit as though they were geometry.
    // Strength scales how far it bends the surface; 0 turns it off. A model
    // uses the normal map its file names unless the actor is given one.
    void SetNormalMap(TextureId texture, float strength = 1.0f);

    // Light the surface gives off, independent of the lights in the scene.
    // Strength goes past 1 for things brighter than white. A model's own
    // emission applies until this is set.
    void SetEmission(Color color, float strength = 1.0f);

    // How the surface catches light. Roughness runs from 0, a mirror-sharp
    // highlight, to 1, none at all; the default is 0.6. Metallic runs from 0,
    // plastic or wood, to 1, bare metal tinted by its own colour.
    void SetRoughness(float roughness);
    float GetRoughness() const;
    void SetMetallic(float metallic);
    float GetMetallic() const;

    // A custom shader for this actor. Pass an empty MaterialId to return to
    // the default. The actor's colour still reaches the shader as input.Color.
    void SetMaterial(MaterialId material);
    MaterialId GetMaterial() const;

    // Overrides one of the material's uniforms for this actor alone, read in
    // the shader with Param(input, slot). Actors sharing a material still draw
    // together however their values differ, so a thousand differently tinted
    // actors cost one draw call. Setting a name the actor's material did not
    // declare is a no-op with a warning; changing the material clears these.
    void SetUniform(const std::string& name, float x, float y = 0.0f, float z = 0.0f, float w = 0.0f);
    void SetUniform(const std::string& name, Color color);
    void ClearUniforms();

    // --- animation ---------------------------------------------------------
    //
    // A model with a skeleton or morph targets and clips can animate. Other
    // actors return zero from GetAnimationCount and ignore the rest, so these
    // are safe to call on any actor.
    //
    // Clips belong to the model and are shared by every actor loaded from the
    // same file; each actor keeps only its own playback state.

    int GetAnimationCount() const;
    const char* GetAnimationName(int index) const;

    // -1 when no clip has that name.
    int FindAnimation(const char* name) const;

    // How long a clip runs at speed 1, in seconds.
    float GetAnimationDuration(int index) const;

    // Starts a clip, fading whatever is playing out over the same time.
    //
    //   guard.Play({ .Name = "walk" });
    //   guard.Play({ .Name = "run", .Fade = 0.15f });   // crossfades
    //
    // Up to four clips mix at once; a fifth retires the quietest.
    void Play(const AnimationPlay& play);

    // Fades everything out over this many seconds. Zero stops at once, and
    // the actor returns to the pose its model was authored in.
    void StopAnimation(float fadeSeconds = 0.2f);

    // True while any clip is playing, including one that has faded to nothing
    // but not yet been retired.
    bool IsAnimating() const;

    // True once a non-looping clip has reached its end. False while anything
    // is still running.
    bool IsAnimationFinished() const;

    // The time of the strongest clip, in seconds. Setting it scrubs that
    // clip, so an animation can be driven by something other than time.
    float GetAnimationTime() const;
    void SetAnimationTime(float seconds);

    // --- joints -------------------------------------------------------------
    //
    // Where the bones are, for attaching things to them, such as a sword to a
    // hand. Read the joint after the world updates and move the attached
    // actor there.
    //
    //   const int hand = knight.FindJoint("hand.R");
    //   ...
    //   const ludifex::Transform3 grip = knight.GetJointTransform(hand);
    //   sword.SetPosition(grip.Position);
    //   sword.SetRotation(grip.Rotation);

    int GetJointCount() const;
    const char* GetJointName(int index) const;

    // -1 when no joint has that name.
    int FindJoint(const char* name) const;

    // Where the joint is in the world, with the actor's own transform applied.
    // Identity for an actor with no skeleton, or for one that has not been
    // posed yet.
    Transform3 GetJointTransform(int index) const;

    // --- morph targets ------------------------------------------------------
    //
    // The blend shapes a model carries. Each is a weight: zero for the model
    // as built and one for the full shape. Values past either end exaggerate
    // or invert it, as glTF allows. A weight set here belongs to the actor; a
    // playing clip that animates the same weight mixes over it in proportion
    // to the clip's share and releases it when it stops. GetMorphWeight
    // returns the value being drawn, including the mix.
    int GetMorphTargetCount() const;
    const char* GetMorphTargetName(int index) const;
    int FindMorphTarget(const char* name) const;
    void SetMorphWeight(int index, float weight);
    void SetMorphWeight(const char* name, float weight);
    float GetMorphWeight(int index) const;

    // Called when this actor is struck hard enough to register. Replaces any
    // previous handler; pass {} to clear it.
    void WhenCollided(CollisionHandler handler);

    // Called when something starts or stops overlapping this actor, which only
    // happens if it was created as a sensor. Replaces any previous handler.
    void WhenEntered(TriggerHandler handler);
    void WhenExited(TriggerHandler handler);

    // True when this actor was created with IsSensor set: it detects overlaps
    // and never pushes anything.
    bool IsSensor() const;

    // Which layers this actor belongs to, and which it collides with. Two
    // actors touch only when each one's category is in the other's mask, so
    // filtering is always symmetric.
    //
    //   constexpr uint64_t Player = 1 << 0;
    //   constexpr uint64_t Enemy  = 1 << 1;
    //   constexpr uint64_t Pickup = 1 << 2;
    //
    //   player.SetCollisionFilter(Player, ~Pickup);   // walks through pickups
    void SetCollisionFilter(uint64_t category, uint64_t mask);

    // --- hierarchy --------------------------------------------------------
    //
    // A child keeps its place relative to its parent. Whatever moves the
    // parent (physics, a setter, or its own parent) carries the child.
    //
    // Physics owns a dynamic body's transform, so a dynamic child is left to
    // the solver, with a warning logged once. Use the hierarchy for static
    // and kinematic actors; to hold two dynamic bodies together, use a weld
    // joint.

    // Deferred, like every other structural change. The child stays where it
    // is in the world: its offset from the parent is computed at the sync
    // point.
    void SetParent(Actor3D parent);
    void ClearParent();

    Actor3D GetParent() const;
    std::vector<Actor3D> GetChildren() const;

    // Where this actor sits relative to its parent. Without a parent these
    // are its world position and rotation.
    Vec3 GetLocalPosition() const;
    void SetLocalPosition(Vec3 position);
    Quat GetLocalRotation() const;
    void SetLocalRotation(Quat rotation);

    // Deferred operations. These are recorded and applied at the next sync
    // point, so they are safe to call while iterating.
    void SetBodyType(BodyType type);

    // Destroys this actor and everything parented to it, at the next sync
    // point.
    void Destroy();

private:
    friend class World3D;
    friend class Character3D;
    friend struct native::Accessor;
    friend Actor3D detail::MakeActor(detail::World3DState* state, const ActorId& id);

    detail::World3DState* m_State = nullptr;
    ActorId m_Id;
};

// ---------------------------------------------------------------------------
// Joints
//
// Joints are described in world space (a pivot and an axis) and converted to
// the body-local frames the solver uses.
// ---------------------------------------------------------------------------

struct JointId
{
    uint32_t Index = 0;
    uint32_t Generation = 0;

    bool IsValid() const { return Generation != 0; }
};

enum class JointKind
{
    Hinge,    // one rotational degree of freedom about an axis
    Slider,   // one translational degree of freedom along an axis
    Distance, // holds two points a set distance apart, optionally springy
    Weld,     // holds two bodies rigidly together
    Cone,     // 3D: a ball joint that turns freely within a cone, and twists within limits
    Wheel     // 2D: turns freely, and rides a spring along an axis, the way a car's wheel does
};

struct HingeDesc
{
    Actor3D BodyA;
    Actor3D BodyB;

    Vec3 Anchor;                    // world-space pivot
    Vec3 Axis{ 0.0f, 1.0f, 0.0f };  // world-space axis to turn about

    bool EnableLimit = false;
    float LowerAngle = 0.0f;        // radians; the range should include zero
    float UpperAngle = 0.0f;

    bool EnableMotor = false;
    float MotorSpeed = 0.0f;        // radians per second
    float MaxMotorTorque = 0.0f;

    // Pulls B back toward the angle it was created at.
    bool EnableSpring = false;
    float Hertz = 4.0f;
    float DampingRatio = 0.7f;

    // Connected bodies do not collide unless you say so.
    bool CollideConnected = false;
};

struct SliderDesc
{
    Actor3D BodyA;
    Actor3D BodyB;

    Vec3 Anchor;
    Vec3 Axis{ 1.0f, 0.0f, 0.0f };  // world-space axis to slide along

    bool EnableLimit = false;
    float LowerTranslation = 0.0f;  // metres
    float UpperTranslation = 0.0f;

    bool EnableMotor = false;
    float MotorSpeed = 0.0f;        // metres per second
    float MaxMotorForce = 0.0f;

    // Pulls B back toward where it was created.
    bool EnableSpring = false;
    float Hertz = 4.0f;
    float DampingRatio = 0.7f;

    bool CollideConnected = false;
};

struct DistanceJointDesc
{
    Actor3D BodyA;
    Actor3D BodyB;

    Vec3 AnchorA;   // world-space point on A
    Vec3 AnchorB;   // world-space point on B

    // Negative keeps whatever distance the two anchors are at now, which is
    // usually what you want when building from placed bodies.
    float Length = -1.0f;

    bool EnableSpring = false;
    float Hertz = 4.0f;
    float DampingRatio = 0.5f;

    // Without a spring the length is held exactly. With one, the length can
    // also be held inside a range: a rope is a spring of zero hertz, which
    // leaves it slack, with a range from zero to the rope's length.
    bool EnableLimit = false;
    float MinLength = 0.0f;
    float MaxLength = 0.0f;

    bool CollideConnected = false;
};

struct WeldDesc
{
    Actor3D BodyA;
    Actor3D BodyB;

    Vec3 Anchor;

    // Zero is rigid. A frequency makes the weld springy.
    float LinearHertz = 0.0f;
    float AngularHertz = 0.0f;
    float LinearDampingRatio = 1.0f;
    float AngularDampingRatio = 1.0f;

    bool CollideConnected = false;
};

// A ball joint that swings within a cone and twists within limits, as used
// for shoulders, hips, and necks.
struct ConeJointDesc
{
    Actor3D BodyA;
    Actor3D BodyB;

    Vec3 Anchor;                     // world-space pivot
    Vec3 Axis{ 0.0f, -1.0f, 0.0f };  // world-space centre of the cone, from A toward B

    // How far B may swing from the axis, in radians, 0 to pi/2.
    bool EnableConeLimit = true;
    float ConeAngle = 0.8f;

    // How far B may turn about the axis, in radians. The range should
    // include zero, which is how the bodies stand when the joint is made.
    bool EnableTwistLimit = true;
    float LowerTwist = -0.5f;
    float UpperTwist = 0.5f;

    // Pulls B back toward the pose it had when the joint was created.
    bool EnableSpring = false;
    float Hertz = 2.0f;
    float DampingRatio = 0.7f;

    // Drives B's angular velocity relative to A toward MotorVelocity, in
    // radians per second about each world axis, with at most MaxMotorTorque.
    // A motor with no velocity and some torque is joint friction.
    bool EnableMotor = false;
    Vec3 MotorVelocity;
    float MaxMotorTorque = 0.0f;

    bool CollideConnected = false;
};

// A handle-backed view onto one joint. Destroying either connected actor
// destroys the joint too, and this handle then reports IsValid() == false.
class Joint3D
{
public:
    Joint3D() = default;

    bool IsValid() const;
    JointId GetId() const { return m_Id; }
    JointKind GetKind() const;

    // Hinge: the angle in radians. Cone: how far B has swung from the axis.
    float GetAngle() const;

    // Slider: the translation in metres. Distance: the current length.
    float GetTranslation() const;

    void EnableLimit(bool enabled);
    void SetLimits(float lower, float upper);

    void EnableMotor(bool enabled);
    void SetMotorSpeed(float speed);

    // Torque for a hinge or a cone joint, force for a slider or a distance joint.
    void SetMaxMotorEffort(float effort);
    float GetMotorEffort() const;

    // Distance only.
    void SetLength(float length);

    // Hinge, slider, distance, and cone. SetSpring on a weld sets both its
    // linear and its angular springiness.
    void EnableSpring(bool enabled);
    void SetSpring(float hertz, float dampingRatio);

    // Cone only. The swing allowed from the axis, and how far B is from it
    // now; the twist about the axis. SetLimits sets the twist limits, and
    // EnableLimit turns the cone and the twist limits on or off together.
    void SetConeAngle(float radians);
    float GetConeAngle() const;
    float GetTwistAngle() const;

    // Cone only: the angular velocity the motor drives B at relative to A, in
    // radians per second about each world axis.
    void SetMotorVelocity(Vec3 velocity);

    // Deferred to the next sync point, like destroying an actor.
    void Destroy();

private:
    friend class World3D;

    detail::World3DState* m_State = nullptr;
    JointId m_Id;
};

struct RagdollDesc
{
    // Where the feet stand, and how tall the figure is.
    Vec3 Position;
    float Height = 1.8f;

    // As on every other shape.
    float Density = 1.0f;

    // Adds springs so the figure holds its pose a little instead of
    // collapsing at once.
    bool Stiff = false;

    Color Tint{ 0.82f, 0.62f, 0.32f, 1.0f };

    // Prefixed to each part's name: "Guard" makes "Guard.Head" and so on.
    std::string Name = "Ragdoll";
};

// The parts and joints of a ragdoll, by name.
struct Ragdoll
{
    Actor3D Pelvis, Chest, Head;
    Actor3D UpperArmLeft, LowerArmLeft, UpperArmRight, LowerArmRight;
    Actor3D UpperLegLeft, LowerLegLeft, UpperLegRight, LowerLegRight;

    Joint3D Spine, Neck;
    Joint3D ShoulderLeft, ElbowLeft, ShoulderRight, ElbowRight;
    Joint3D HipLeft, KneeLeft, HipRight, KneeRight;

    // Every part, pelvis first: for pushing the whole figure, or colouring it.
    std::vector<Actor3D> Parts() const
    {
        return { Pelvis, Chest, Head, UpperArmLeft, LowerArmLeft, UpperArmRight, LowerArmRight,
                 UpperLegLeft, LowerLegLeft, UpperLegRight, LowerLegRight };
    }

    bool IsValid() const { return Pelvis.IsValid(); }
};

// What a ray found, or Hit == false when it found nothing.
struct RayHit
{
    bool Hit = false;
    Actor3D Actor;

    Vec3 Point;
    Vec3 Normal;

    // 0 at the ray's origin, 1 at its end.
    float Fraction = 0.0f;
    float Distance = 0.0f;
};

// An overlap starting or ending between a sensor and something else.
struct TriggerInfo
{
    Actor3D Sensor;   // the actor created with IsSensor
    Actor3D Other;    // what entered or left it
};

struct CollisionInfo
{
    Actor3D Self;
    Actor3D Other;

    Vec3 Point;    // world-space contact point
    Vec3 Normal;   // contact normal, pointing from Other toward Self

    // How fast the two were closing, in meters per second. Use it to scale a
    // sound or an effect so a nudge and a crash do not look the same.
    float ImpactSpeed = 0.0f;
};

// A handle-backed view onto one point light. Like an actor, a light kept past
// its destruction reports IsValid() == false and ignores calls.
class Light3D
{
public:
    Light3D() = default;

    bool IsValid() const;
    LightId GetId() const { return m_Id; }

    void SetPosition(Vec3 position);
    Vec3 GetPosition() const;
    void SetColor(Color color);
    Color GetColor() const;
    void SetIntensity(float intensity);
    float GetIntensity() const;
    void SetRange(float range);
    float GetRange() const;

    // Follows an actor from then on, at an offset in the actor's own space.
    // The light detaches by itself if the actor is destroyed.
    void AttachTo(Actor3D actor, Vec3 offset = {});
    void Detach();

    void Destroy();

private:
    friend class World3D;

    detail::World3DState* m_State = nullptr;
    LightId m_Id;
};

// A character controller: a capsule that you move directly instead of a body
// that physics pushes around. Each move slides along what it meets, steps
// over small ledges, and reports what the character is standing on.
struct CharacterDesc
{
    Vec3 Position;

    float Radius = 0.4f;
    float Height = 1.8f; // total, including both caps

    // A ledge no taller than this is stepped over rather than walked into.
    float StepHeight = 0.35f;

    // Ground steeper than this is a wall: it holds the character up but is not
    // stood on, and it does not count as being on the ground.
    float SlopeLimitDegrees = 55.0f;

    std::string Name;
};

class Character3D
{
public:
    Character3D() = default;

    bool IsValid() const;
    ActorId GetId() const { return m_Id; }

    friend bool operator==(const Character3D& a, const Character3D& b) { return a.m_Id == b.m_Id; }
    friend bool operator!=(const Character3D& a, const Character3D& b) { return !(a == b); }

    Vec3 GetPosition() const; // the middle of the capsule
    void SetPosition(Vec3 position);

    // Moves by this much, sliding along whatever is in the way and stepping
    // over ledges no taller than StepHeight. Returns how far it actually
    // moved; walking into a wall returns no movement along the wall's normal.
    Vec3 Move(Vec3 delta);

    // True while it is standing on ground no steeper than the slope limit.
    // Answered by the last Move.
    bool IsOnGround() const;
    Vec3 GetGroundNormal() const;

    // The capsule the rest of the world sees: it renders, it can be given a
    // colour or a material, and dynamic bodies collide with it.
    Actor3D GetActor() const;

    void Destroy();

private:
    friend class World3D;

    ActorId m_Id;
    detail::World3DState* m_State = nullptr;
};

class World3D
{
public:
    World3D() = default;
    ~World3D();

    World3D(const World3D&) = delete;
    World3D& operator=(const World3D&) = delete;
    World3D(World3D&& other) noexcept;
    World3D& operator=(World3D&& other) noexcept;

    bool IsValid() const { return m_State != nullptr; }

    Actor3D AddBox(const BoxDesc& desc = {});
    Actor3D AddSphere(const SphereDesc& desc = {});
    Actor3D AddCapsule(const CapsuleDesc& desc = {});
    Actor3D AddGround(const GroundDesc& desc = {});

    // A capsule moved by Character3D::Move instead of by physics.
    Character3D AddCharacter(const CharacterDesc& desc = {});

    // --- saving and restoring --------------------------------------------
    //
    // Captures the world's actors (shapes, materials, positions, velocities,
    // appearance, and parenting), its lights, its camera, and its step
    // settings. Useful for save games, level editing, and undo.
    //
    // Not captured: registered callbacks, textures and custom materials
    // created through handles, joints, and sounds. A program that restores a
    // world adds those back itself.
    std::vector<uint8_t> Save() const;

    // Destroys everything in this world and rebuilds it from the bytes.
    // False when they are not a world this build can read, and the world is
    // then left as it was.
    bool Load(const std::vector<uint8_t>& bytes);

    bool SaveToFile(const std::string& path) const;
    bool LoadFromFile(const std::string& path);

    // Loads a model and adds it. A file is loaded once however many actors
    // use it. The collider is a box fitted to the model's bounds, which suits
    // most props but not every shape (a torus, for example).
    Actor3D AddModel(const ModelDesc& desc);
    Actor3D AddModel(const std::string& path);

    // --- joints -----------------------------------------------------------

    Joint3D AddHinge(const HingeDesc& desc);
    Joint3D AddSlider(const SliderDesc& desc);
    Joint3D AddDistanceJoint(const DistanceJointDesc& desc);
    Joint3D AddWeld(const WeldDesc& desc);
    Joint3D AddConeJoint(const ConeJointDesc& desc);

    // A humanoid of eleven parts (pelvis, chest, head, and two parts per
    // limb) held together by cone joints at the neck, spine, shoulders, and
    // hips and by hinges at the elbows and knees, each with human-like
    // limits. Built standing, arms at its sides, facing +Z.
    Ragdoll AddRagdoll(const RagdollDesc& desc);

    size_t GetJointCount() const;

    Actor3D GetActor(ActorId id);

    // Advances the world by one frame. In automatic mode this runs as many
    // fixed steps as the accumulated time allows, up to MaxStepsPerFrame.
    void Update(float deltaSeconds);

    // Runs exactly one physics step and refreshes the interpolation snapshots.
    void StepPhysics(float timeStep);

    // Flushes queued structural changes. Update() and StepPhysics() call this
    // at their sync points; call it directly when an immediate structural
    // effect is needed.
    void ApplyPendingChanges();

    void StartPhysics();
    void StopPhysics();
    bool IsPhysicsRunning() const;

    void SetPhysicsMode(PhysicsMode mode);
    PhysicsMode GetPhysicsMode() const;

    void SetGravity(Vec3 gravity);
    Vec3 GetGravity() const;

    void SetFixedTimeStep(float seconds);
    float GetFixedTimeStep() const;
    void SetSubStepCount(int count);
    int GetSubStepCount() const;

    // 0 at the moment of the last step, approaching 1 as the next step nears.
    float GetInterpolationAlpha() const;

    // How far the frame being drawn sits between the last physics step and
    // the next, from 0 to 1. Set by Update; a program stepping physics itself
    // sets it from its own accumulator, so motion stays smooth when the frame
    // rate and the step rate differ.
    void SetInterpolationAlpha(float alpha);

    size_t GetActorCount() const;
    uint64_t GetStepCount() const;

    // How many threads actually step this world. Asking for more than one
    // starts the shared scheduler; asking for 0 uses one per hardware thread.
    // Deterministic worlds always report 1.
    uint32_t GetWorkerCount() const;

    void ForEachActor(const std::function<void(Actor3D&)>& visit);

    // Called for every registered collision in the world, after each actor's
    // own handler.
    void WhenActorCollided(CollisionHandler handler);

    // Called for every sensor overlap in the world, after the sensor's own
    // handler.
    void WhenActorEnteredTrigger(TriggerHandler handler);
    void WhenActorLeftTrigger(TriggerHandler handler);

    // Collisions softer than this are not reported, which keeps resting
    // contacts from firing constantly. In meters per second.
    void SetCollisionThreshold(float metersPerSecond);

    // --- queries ----------------------------------------------------------

    // Sweeps a shape along a direction and stops at the first solid thing it
    // meets, to find out whether something of that size can pass. Sensors are
    // ignored, as with rays.
    RayHit CastSphere(Vec3 from, float radius, Vec3 direction, float maxDistance);
    RayHit CastBox(Vec3 from, Vec3 scale, Vec3 direction, float maxDistance);
    RayHit CastCapsule(Vec3 from, float radius, float height, Vec3 direction, float maxDistance);

    // Every actor a shape overlaps where it stands now. Sensors are included
    // only when asked for.
    std::vector<Actor3D> OverlapSphere(Vec3 center, float radius, bool includeSensors = false);
    std::vector<Actor3D> OverlapBox(Vec3 center, Vec3 scale, bool includeSensors = false);
    std::vector<Actor3D> OverlapCapsule(Vec3 center, float radius, float height,
                                        bool includeSensors = false);

    // Casts from origin along direction for maxDistance and returns the nearest
    // thing it struck. Direction does not need to be normalized.
    RayHit CastRay(Vec3 origin, Vec3 direction, float maxDistance = 1000.0f);

    // Casts from the camera through a point on the view, to pick the object
    // under the pointer. The coordinates are 0..1 across the view, with (0,0)
    // at the top left.
    RayHit PickFromView(float normalizedX, float normalizedY, float maxDistance = 1000.0f);

    // --- rendering --------------------------------------------------------

    // Runs the world until its window closes: every frame it is stepped,
    // drawn, and shown. After opane::StartApp this is that application's
    // window and loop, with the world beneath its interface; in a program
    // without one, the world opens a window of its own. Returns when the
    // window closes.
    void Run();

    // Sizes the offscreen target the world draws into. Call it when the view
    // resizes; it is a no-op when the size is unchanged.
    void SetRenderSize(int width, int height);

    // Draws the world into its own target, on its own command buffer, using
    // the interpolated transforms so motion stays smooth at any frame rate.
    void Render();

    // The resolved colour texture, as an SDL_GPUTexture*. Pass it to a
    // renderer that can sample it, such as opane's App::WrapExternalTexture.
    void* GetRenderTarget() const;

    Camera3D& GetCamera();
    const Camera3D& GetCamera() const;
    void SetCamera(const Camera3D& camera);

    RenderSettings& GetRenderSettings();
    void SetRenderSettings(const RenderSettings& settings);

    // Chooses an anti-aliasing preset. Shorthand for the Mode field of the
    // render settings.
    void SetAntiAliasing(AntiAliasing mode);

    // Applies a graphics quality preset to the render settings: shorthand for
    // ApplyGraphicsQuality on GetRenderSettings().
    void SetGraphicsQuality(GraphicsQuality quality);

    // An image behind everything, filling the view and cropped to keep its
    // shape. Pass an empty TextureId to return to the plain sky colour.
    void SetBackground(TextureId texture);
    void SetBackground(const std::string& path);

    // --- lights -----------------------------------------------------------

    // At most 256 point lights shade a frame (those contributing most to the
    // view win), and at most 32 of them reach any one cluster of the view.
    Light3D AddPointLight(const PointLightDesc& desc);
    size_t GetLightCount() const;

    // --- post-processing --------------------------------------------------

    // Runs a full-screen material over the frame at the given point. Several
    // at the same point run in the order they were added, each reading the
    // previous one's output. Author the shader against world_post.hlsli.
    void AddPostProcess(MaterialId material, PassPoint point = PassPoint::AfterToneMap);
    void RemovePostProcess(MaterialId material);
    void ClearPostProcess();

    // --- audio ------------------------------------------------------------
    //
    // A world's voices are updated in Update: an emitter on an actor follows
    // it, the listener follows the camera, and voice limiting, distance
    // culling, and occlusion are re-evaluated.

    // Not positioned in the world: for interface sounds or narration.
    VoiceId PlaySound(SoundId sound, const SoundSettings& settings = {});

    // At a fixed point in the world.
    VoiceId PlaySoundAt(SoundId sound, Vec3 position, const SoundSettings& settings = {});

    // On an actor: the sound follows it, its velocity drives the Doppler
    // effect, and a cone points down the actor's -Z. The voice stops, with a
    // short fade, if the actor is destroyed.
    VoiceId PlaySoundAt(SoundId sound, Actor3D emitter, const SoundSettings& settings = {});

    void StopSound(VoiceId voice, float fadeSeconds = 0.05f);
    void StopAllSounds(float fadeSeconds = 0.05f);

    // True while the voice is being mixed or waiting silently to resume.
    bool IsPlaying(VoiceId voice) const;

    // True only while the voice is being mixed: in range, and not displaced
    // by more important sounds.
    bool IsAudible(VoiceId voice) const;

    // True while the voice is being mixed and something solid stands between
    // it and the listener. Only a voice asking for Occlusion is ever muffled.
    bool IsOccluded(VoiceId voice) const;

    // Ramped over a few milliseconds, so a sudden change does not click.
    void SetVoiceVolume(VoiceId voice, float volume);
    void SetVoicePosition(VoiceId voice, Vec3 position);

    // Streamed from disk, looping, and not placed. Starting new music fades
    // the old out as the new fades in.
    void PlayMusic(const std::string& path, float volume = 1.0f, float fadeSeconds = 1.0f);
    void StopMusic(float fadeSeconds = 1.0f);

    // Places the listener, when AudioSettings::ListenerFollowsCamera is off.
    void SetListener(Vec3 position, Vec3 forward, Vec3 up = { 0.0f, 1.0f, 0.0f });

    AudioSettings& GetAudioSettings();
    void SetAudioSettings(const AudioSettings& settings);
    AudioStats GetAudioStats() const;

    // --- debug drawing ----------------------------------------------------
    //
    // Lines for the next frame only: call these every frame you want them
    // shown. They are depth-tested against the scene and cost nothing once you
    // stop calling them.

    void DrawLine(Vec3 from, Vec3 to, Color color = Color{ 1.0f, 1.0f, 0.2f, 1.0f });
    void DrawBox(Vec3 center, Vec3 size, Color color = Color{ 1.0f, 1.0f, 0.2f, 1.0f });
    void DrawSphere(Vec3 center, float radius, Color color = Color{ 1.0f, 1.0f, 0.2f, 1.0f });

    // Draws every collider's bounds and every joint's anchors each frame.
    void SetPhysicsDebugDraw(bool enabled);

    // --- background loading ------------------------------------------------

    // Models still being parsed. Zero once everything requested has arrived.
    size_t GetPendingLoadCount() const;

    // Blocks until every model in flight has loaded.
    void WaitForLoads();

    // Draw calls issued by the last Render. Objects sharing a mesh are drawn
    // together, so this counts distinct meshes rather than actors.
    uint32_t GetLastDrawCallCount() const;

    // Objects actually submitted, and objects rejected by frustum culling
    // before they reached the GPU.
    uint32_t GetDrawnInstanceCount() const;
    uint32_t GetCulledCount() const;

    // The most objects that went out in a single draw call. Objects sharing a
    // mesh, a material, and a texture are drawn together however many of them
    // there are, so this shows how well instancing is working.
    uint32_t GetLargestBatchSize() const;

    // Whether the last Render traced rays: ray tracing or path tracing was on
    // and this device could run it.
    bool IsRayTracingActive() const;

    // The samples the path-traced image has gathered so far, which climbs
    // while nothing moves; 0 when path tracing is off.
    uint32_t GetPathTracedSampleCount() const;

private:
    friend World3D CreateWorld3D(const World3DConfig& config);
    friend class Actor3D;
    friend struct native::Accessor;

    detail::World3DState* m_State = nullptr;
};

World3D CreateWorld3D(const World3DConfig& config = {});

// ===========================================================================
// 2D
// ===========================================================================

struct World2DConfig
{
    Vec2 Gravity{ 0.0f, -10.0f };
    float FixedTimeStep = 1.0f / 60.0f;
    int SubStepCount = 4;
    int MaxStepsPerFrame = 8;
    bool PhysicsEnabled = true;
    bool EnableSleep = true;
    bool EnableContinuous = true;
    bool Deterministic = false;
    uint32_t WorkerCount = 1;
};

struct RectangleDesc
{
    float Width = 1.0f;
    float Height = 1.0f;
    Vec2 Position;
    float Rotation = 0.0f;
    BodyType Type = BodyType::Dynamic;
    float Density = 1.0f;
    float Friction = 0.3f;
    float Restitution = 0.0f;
    bool IsBullet = false;
    bool IsSensor = false;
    std::string Name;
};

struct CircleDesc
{
    float Radius = 0.5f;
    Vec2 Position;
    BodyType Type = BodyType::Dynamic;
    float Density = 1.0f;
    float Friction = 0.3f;
    float Restitution = 0.2f;
    bool IsBullet = false;
    bool IsSensor = false;
    std::string Name;
};

// A capsule standing along its own Y axis. Rotation turns it.
struct Capsule2DDesc
{
    float Radius = 0.25f;
    float Height = 1.0f; // total, including both caps
    Vec2 Position;
    float Rotation = 0.0f;
    BodyType Type = BodyType::Dynamic;
    float Density = 1.0f;
    float Friction = 0.3f;
    float Restitution = 0.0f;
    bool IsBullet = false;
    bool IsSensor = false;
    std::string Name;
};

struct Ground2DDesc
{
    float Width = 50.0f;
    float Thickness = 1.0f;
    Vec2 Position; // top surface height
    float Friction = 0.6f;
    std::string Name = "Ground";
};

// The shape a sprite collides as. None gives it a body with no collider, for
// scenery and effects.
enum class SpriteCollider
{
    Box,
    Circle,
    None
};

// An image in the world with a body behind it. Sprites are drawn unlit, so the
// image appears as authored, and blended, so soft edges work.
struct SpriteDesc
{
    // An image file found through the asset roots, or a texture you already
    // have. Path wins when both are set.
    std::string Path;
    TextureId Texture;

    Vec2 Position;
    float Rotation = 0.0f;

    // The size in world units. Left at zero it comes from the image at
    // PixelsPerUnit; with one axis given, the other keeps the image's aspect.
    Vec2 Size;
    float PixelsPerUnit = 100.0f;

    BodyType Type = BodyType::Dynamic;
    SpriteCollider Collider = SpriteCollider::Box;
    float Density = 1.0f;
    float Friction = 0.3f;
    float Restitution = 0.0f;

    // Keeps the body upright however it is pushed.
    bool FixedRotation = false;

    bool IsSensor = false;

    // Drawing order among sprites: a higher layer is drawn in front. Sprites
    // on the same layer are drawn in the order they were added.
    int Layer = 0;

    std::string Name;
};

struct CollisionInfo2D;
struct TriggerInfo2D;

using CollisionHandler2D = std::function<void(const CollisionInfo2D&)>;
using TriggerHandler2D = std::function<void(const TriggerInfo2D&)>;

class Actor2D
{
public:
    Actor2D() = default;

    bool IsValid() const;
    ActorId GetId() const { return m_Id; }

    // Two handles are equal when they refer to the same actor.
    friend bool operator==(const Actor2D& a, const Actor2D& b) { return a.m_Id == b.m_Id; }
    friend bool operator!=(const Actor2D& a, const Actor2D& b) { return !(a == b); }
    const std::string& GetName() const;
    void SetName(const std::string& name);

    Vec2 GetPosition() const;
    void SetPosition(Vec2 position);
    float GetRotation() const;
    void SetRotation(float radians);

    Vec2 GetLinearVelocity() const;
    void SetLinearVelocity(Vec2 velocity);
    float GetAngularVelocity() const;
    void SetAngularVelocity(float radiansPerSecond);

    Color GetColor() const;
    void SetColor(Color color);
    bool IsVisible() const;
    void SetVisible(bool visible);

    // The image on the actor, multiplied by its colour. A sprite starts with
    // its own image; any other actor starts with none.
    void SetTexture(TextureId texture);
    TextureId GetTexture() const;

    // Repeats or crops the texture: {2, 2} tiles it twice each way, and a
    // repeat below 1 with an offset shows one part of the image.
    void SetTextureTiling(Vec2 repeat, Vec2 offset = {});

    // One frame of a sprite sheet laid out in a grid, counted left to right
    // and top to bottom from zero. Change it over time to animate a sprite.
    void SetFrame(int frame, int columns, int rows);

    // Drawing order among sprites and other flat actors: higher is in front.
    void SetLayer(int layer);
    int GetLayer() const;

    // Mirrors the image left to right.
    void SetFlipX(bool flipped);
    bool GetFlipX() const;

    void SetMaterial(MaterialId material);
    MaterialId GetMaterial() const;

    // Per-actor values for the material's uniforms, as on Actor3D.
    void SetUniform(const std::string& name, float x, float y = 0.0f, float z = 0.0f, float w = 0.0f);
    void SetUniform(const std::string& name, Color color);
    void ClearUniforms();

    // Keeps the body upright however it is pushed.
    void SetFixedRotation(bool fixed);

    void ApplyForce(Vec2 force);
    void ApplyImpulse(Vec2 impulse);

    float GetMass() const;
    bool IsAwake() const;
    void SetAwake(bool awake);

    Transform2 GetInterpolatedTransform() const;

    // Called when this actor is struck hard enough to register. Replaces any
    // previous handler; pass {} to clear it.
    void WhenCollided(CollisionHandler2D handler);

    // Called when something starts or stops overlapping this actor, which only
    // happens if it was created as a sensor. Replaces any previous handler.
    void WhenEntered(TriggerHandler2D handler);
    void WhenExited(TriggerHandler2D handler);

    // True when this actor was created with IsSensor set: it detects overlaps
    // and never pushes anything.
    bool IsSensor() const;

    // Which layers this actor belongs to, and which it collides with. Two
    // actors touch only when each one's category is in the other's mask.
    void SetCollisionFilter(uint64_t category, uint64_t mask);

    void SetBodyType(BodyType type);
    void Destroy();

private:
    friend class World2D;
    friend struct native::Accessor;
    friend Actor2D detail::MakeActor(detail::World2DState* state, const ActorId& id);

    detail::World2DState* m_State = nullptr;
    ActorId m_Id;
};

// What one actor was hit by, from that actor's point of view.
struct CollisionInfo2D
{
    Actor2D Self;
    Actor2D Other;

    Vec2 Point;    // world-space contact point
    Vec2 Normal;   // contact normal, pointing from Other toward Self

    // How fast the two were closing, in meters per second.
    float ImpactSpeed = 0.0f;
};

// An overlap starting or ending between a sensor and something else.
struct TriggerInfo2D
{
    Actor2D Sensor;   // the actor created with IsSensor
    Actor2D Other;    // what entered or left it
};

// The result of a ray cast against a 2D world.
struct RayHit2D
{
    bool Hit = false;
    Actor2D Actor;

    Vec2 Point;
    Vec2 Normal;

    // 0 at the ray's origin, 1 at its end.
    float Fraction = 0.0f;
    float Distance = 0.0f;
};

// --- 2D joints ---------------------------------------------------------------
//
// As in 3D, a joint is described in world space from bodies already placed,
// and holds them however they stand when it is made: an angle of zero is the
// angle they have now. Angles are counter-clockwise, in radians.

struct HingeDesc2D
{
    Actor2D BodyA;
    Actor2D BodyB;

    Vec2 Anchor; // world-space pivot

    // At most a little under half a turn either way; the range should include
    // zero.
    bool EnableLimit = false;
    float LowerAngle = 0.0f;
    float UpperAngle = 0.0f;

    bool EnableMotor = false;
    float MotorSpeed = 0.0f; // radians per second
    float MaxMotorTorque = 0.0f;

    // Pulls B back toward the angle it was created at.
    bool EnableSpring = false;
    float Hertz = 4.0f;
    float DampingRatio = 0.7f;

    bool CollideConnected = false;
};

struct SliderDesc2D
{
    Actor2D BodyA;
    Actor2D BodyB;

    Vec2 Anchor;
    Vec2 Axis{ 1.0f, 0.0f }; // world-space direction B slides along

    bool EnableLimit = false;
    float LowerTranslation = 0.0f; // metres
    float UpperTranslation = 0.0f;

    bool EnableMotor = false;
    float MotorSpeed = 0.0f; // metres per second
    float MaxMotorForce = 0.0f;

    // Pulls B back toward where it was made.
    bool EnableSpring = false;
    float Hertz = 4.0f;
    float DampingRatio = 0.7f;

    bool CollideConnected = false;
};

struct DistanceJointDesc2D
{
    Actor2D BodyA;
    Actor2D BodyB;

    Vec2 AnchorA; // world-space point on A
    Vec2 AnchorB; // world-space point on B

    // Negative keeps whatever distance the anchors are apart now.
    float Length = -1.0f;

    bool EnableSpring = false;
    float Hertz = 4.0f;
    float DampingRatio = 0.5f;

    // Without a spring the length is held exactly. With one, the length can
    // also be held inside a range: a rope is a spring of zero hertz, which
    // leaves it slack, with a range from zero to the rope's length.
    bool EnableLimit = false;
    float MinLength = 0.0f;
    float MaxLength = 0.0f;

    bool CollideConnected = false;
};

struct WeldDesc2D
{
    Actor2D BodyA;
    Actor2D BodyB;

    Vec2 Anchor;

    // Zero is rigid. A frequency makes the weld springy.
    float LinearHertz = 0.0f;
    float AngularHertz = 0.0f;
    float LinearDampingRatio = 1.0f;
    float AngularDampingRatio = 1.0f;

    bool CollideConnected = false;
};

// A wheel on a suspension: B turns freely about the anchor and rides a spring
// along Axis. BodyA is the chassis, BodyB the wheel, and the anchor the
// wheel's centre. The motor turns the wheel.
struct WheelDesc2D
{
    Actor2D BodyA;
    Actor2D BodyB;

    Vec2 Anchor;
    Vec2 Axis{ 0.0f, 1.0f }; // world-space direction of the suspension's travel

    bool EnableSpring = true;
    float Hertz = 4.0f;
    float DampingRatio = 0.7f;

    // How far the suspension may travel along the axis, in metres.
    bool EnableLimit = false;
    float LowerTranslation = 0.0f;
    float UpperTranslation = 0.0f;

    bool EnableMotor = false;
    float MotorSpeed = 0.0f; // radians per second; negative drives toward +X
    float MaxMotorTorque = 0.0f;

    bool CollideConnected = false;
};

// A handle-backed view onto one 2D joint, as Joint3D is for 3D. Destroying
// either connected actor destroys the joint too.
class Joint2D
{
public:
    Joint2D() = default;

    bool IsValid() const;
    JointId GetId() const { return m_Id; }
    JointKind GetKind() const;

    // Hinge: B's angle relative to A since the joint was made, in radians.
    float GetAngle() const;

    // Slider: the translation in metres. Distance: the current length.
    float GetTranslation() const;

    // Hinge: angles. Slider and wheel: translations. Distance: lengths.
    void EnableLimit(bool enabled);
    void SetLimits(float lower, float upper);

    void EnableMotor(bool enabled);
    void SetMotorSpeed(float speed);

    // Torque for a hinge or a wheel, force for a slider or a distance joint.
    void SetMaxMotorEffort(float effort);
    float GetMotorEffort() const;

    // Hinge, slider, distance, and wheel. SetSpring on a weld sets both its
    // linear and its angular springiness.
    void EnableSpring(bool enabled);
    void SetSpring(float hertz, float dampingRatio);

    // Distance only.
    void SetLength(float length);

    // Deferred to the next sync point, like destroying an actor.
    void Destroy();

private:
    friend class World2D;

    detail::World2DState* m_State = nullptr;
    JointId m_Id;
};

// --- 2D character --------------------------------------------------------------

struct Character2DDesc
{
    Vec2 Position;

    float Radius = 0.3f;
    float Height = 1.2f; // total, including both caps

    // A ledge no taller than this is stepped up rather than walked into.
    float StepHeight = 0.2f;

    // Ground steeper than this is a wall: it holds the character up but is not
    // stood on.
    float SlopeLimitDegrees = 55.0f;

    std::string Name;
};

// A capsule that is moved rather than simulated, as Character3D is in 3D: it
// slides along what it meets, steps up low ledges, and says what it stands on.
// The world still sees it as a kinematic actor, so dynamic bodies bump into
// it and it can be drawn and coloured like any other.
class Character2D
{
public:
    Character2D() = default;

    bool IsValid() const;
    ActorId GetId() const { return m_Id; }

    friend bool operator==(const Character2D& a, const Character2D& b) { return a.m_Id == b.m_Id; }
    friend bool operator!=(const Character2D& a, const Character2D& b) { return !(a == b); }

    Vec2 GetPosition() const; // the middle of the capsule
    void SetPosition(Vec2 position);

    // Moves by this much, sliding along whatever is in the way and stepping
    // up ledges no taller than StepHeight. Returns how far it actually moved;
    // running into a wall returns no movement along the wall's normal.
    Vec2 Move(Vec2 delta);

    // True while standing on ground no steeper than the slope limit. Answered
    // by the last Move.
    bool IsOnGround() const;
    Vec2 GetGroundNormal() const;

    Actor2D GetActor() const;

    void Destroy();

private:
    friend class World2D;

    ActorId m_Id;
    detail::World2DState* m_State = nullptr;
};

class World2D
{
public:
    World2D() = default;
    ~World2D();

    World2D(const World2D&) = delete;
    World2D& operator=(const World2D&) = delete;
    World2D(World2D&& other) noexcept;
    World2D& operator=(World2D&& other) noexcept;

    bool IsValid() const { return m_State != nullptr; }

    Actor2D AddRectangle(const RectangleDesc& desc = {});
    Actor2D AddCircle(const CircleDesc& desc = {});
    Actor2D AddCapsule(const Capsule2DDesc& desc = {});
    Actor2D AddGround(const Ground2DDesc& desc = {});

    // --- joints -----------------------------------------------------------

    Joint2D AddHinge(const HingeDesc2D& desc);
    Joint2D AddSlider(const SliderDesc2D& desc);
    Joint2D AddDistanceJoint(const DistanceJointDesc2D& desc);
    Joint2D AddWeld(const WeldDesc2D& desc);
    Joint2D AddWheel(const WheelDesc2D& desc);
    size_t GetJointCount() const;

    // A kinematic capsule moved by Character2D::Move.
    Character2D AddCharacter(const Character2DDesc& desc = {});

    // An image with a body behind it. The path form makes a dynamic box-shaped
    // sprite at the origin, sized from the image at 100 pixels per unit.
    Actor2D AddSprite(const SpriteDesc& desc);
    Actor2D AddSprite(const std::string& path);

    Actor2D GetActor(ActorId id);

    void Update(float deltaSeconds);
    void StepPhysics(float timeStep);
    void ApplyPendingChanges();

    void StartPhysics();
    void StopPhysics();
    bool IsPhysicsRunning() const;

    void SetPhysicsMode(PhysicsMode mode);
    PhysicsMode GetPhysicsMode() const;

    void SetGravity(Vec2 gravity);
    Vec2 GetGravity() const;

    void SetFixedTimeStep(float seconds);
    float GetFixedTimeStep() const;
    void SetSubStepCount(int count);
    int GetSubStepCount() const;

    float GetInterpolationAlpha() const;

    // How far the frame being drawn sits between the last physics step and
    // the next, from 0 to 1. Set by Update; a program stepping physics itself
    // sets it from its own accumulator, so motion stays smooth when the frame
    // rate and the step rate differ.
    void SetInterpolationAlpha(float alpha);

    size_t GetActorCount() const;
    uint64_t GetStepCount() const;

    void ForEachActor(const std::function<void(Actor2D&)>& visit);

    // --- events -----------------------------------------------------------
    //
    // Delivered after the solver has finished, never from inside it, so a
    // handler may create and destroy actors freely.

    void WhenActorCollided(CollisionHandler2D handler);

    // Resting contacts would otherwise fire constantly, so collisions softer
    // than this are not reported. Meters per second; the default is 1.
    void SetCollisionThreshold(float metersPerSecond);

    void WhenActorEnteredTrigger(TriggerHandler2D handler);
    void WhenActorLeftTrigger(TriggerHandler2D handler);

    // --- queries ----------------------------------------------------------

    RayHit2D CastRay(Vec2 origin, Vec2 direction, float maxDistance = 1000.0f);

    // A shape swept along a direction: the first solid thing it would touch,
    // or Hit == false. Rectangles are axis-aligned; capsules stand along Y.
    RayHit2D CastCircle(Vec2 from, float radius, Vec2 direction, float maxDistance);
    RayHit2D CastRectangle(Vec2 from, Vec2 size, Vec2 direction, float maxDistance);
    RayHit2D CastCapsule(Vec2 from, float radius, float height, Vec2 direction, float maxDistance);

    // Every actor overlapping a shape, each once. Sensors are left out unless
    // asked for.
    std::vector<Actor2D> OverlapCircle(Vec2 center, float radius, bool includeSensors = false);
    std::vector<Actor2D> OverlapRectangle(Vec2 center, Vec2 size, bool includeSensors = false);
    std::vector<Actor2D> OverlapCapsule(Vec2 center, float radius, float height, bool includeSensors = false);
    std::vector<Actor2D> OverlapPoint(Vec2 point, bool includeSensors = false);

    // The actor under a point given in 0..1 view coordinates, with (0, 0) at
    // the top left.
    RayHit2D PickFromView(float normalizedX, float normalizedY);

    // The world-space point a 0..1 view coordinate falls on, whether or not
    // anything is there. Useful for spawning at the pointer.
    Vec2 ViewToWorld(float normalizedX, float normalizedY) const;

    // The inverse: where a world-space point lands in 0..1 view coordinates.
    // Useful for drawing a label or an outline over an actor.
    Vec2 WorldToView(Vec2 world) const;

    // --- rendering --------------------------------------------------------
    //
    // A 2D world draws through the same forward pipeline as a 3D one, with an
    // orthographic camera looking down the Z axis. Shapes are given a little
    // depth so the light still shades them.

    // Runs the world until its window closes, as World3D::Run does.
    void Run();

    void SetRenderSize(int width, int height);
    void Render();
    void* GetRenderTarget() const;

    Camera2D& GetCamera();
    void SetCamera(const Camera2D& camera);

    RenderSettings& GetRenderSettings();
    void SetRenderSettings(const RenderSettings& settings);

    void SetAntiAliasing(AntiAliasing mode);

    // Applies a graphics quality preset. A flat world has no shadows, ambient
    // occlusion, or rays, so here it is the scale, anti-aliasing, and bloom.
    void SetGraphicsQuality(GraphicsQuality quality);

    // An image behind everything, filling the view and cropped to keep its
    // shape. It does not move with the camera. Pass an empty TextureId to
    // return to the plain sky colour.
    void SetBackground(TextureId texture);
    void SetBackground(const std::string& path);

    void AddPostProcess(MaterialId material, PassPoint point = PassPoint::AfterToneMap);
    void RemovePostProcess(MaterialId material);
    void ClearPostProcess();

    // Lines for the next frame only, as on World3D.
    void DrawLine(Vec2 from, Vec2 to, Color color = Color{ 1.0f, 1.0f, 0.2f, 1.0f });

    // --- audio ------------------------------------------------------------
    //
    // As on World3D, in the plane. The listener sits at the camera's centre,
    // so a sound to the right of the view is heard on the right.

    VoiceId PlaySound(SoundId sound, const SoundSettings& settings = {});
    VoiceId PlaySoundAt(SoundId sound, Vec2 position, const SoundSettings& settings = {});
    VoiceId PlaySoundAt(SoundId sound, Actor2D emitter, const SoundSettings& settings = {});
    void StopSound(VoiceId voice, float fadeSeconds = 0.05f);
    void StopAllSounds(float fadeSeconds = 0.05f);
    bool IsPlaying(VoiceId voice) const;
    bool IsAudible(VoiceId voice) const;
    bool IsOccluded(VoiceId voice) const;
    void SetVoiceVolume(VoiceId voice, float volume);
    void SetVoicePosition(VoiceId voice, Vec2 position);

    void PlayMusic(const std::string& path, float volume = 1.0f, float fadeSeconds = 1.0f);
    void StopMusic(float fadeSeconds = 1.0f);

    void SetListener(Vec2 position);

    AudioSettings& GetAudioSettings();
    void SetAudioSettings(const AudioSettings& settings);
    AudioStats GetAudioStats() const;

    uint32_t GetLastDrawCallCount() const;

private:
    friend World2D CreateWorld2D(const World2DConfig& config);
    friend class Actor2D;
    friend struct native::Accessor;

    detail::World2DState* m_State = nullptr;
};

World2D CreateWorld2D(const World2DConfig& config = {});

} // namespace ludifex
