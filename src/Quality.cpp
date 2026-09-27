// The graphics quality presets. Each is a row of the table beside
// GraphicsQuality in ludifex.h; the two must agree.

#include <ludifex/ludifex.h>

namespace ludifex
{
namespace
{

struct Preset
{
    float RenderScale;
    AntiAliasing Mode;

    bool Shadows;
    int ShadowResolution;
    int Cascades;
    float ShadowDistance;
    float ShadowSoftness;

    bool Occlusion;
    int OcclusionSamples;

    bool Bloom;

    bool Detail;
    float Simpler;
    float Simplest;
};

// Level of detail swaps in simpler meshes sooner on the lower presets, where
// a GPU that struggles saves the most, and later on the higher ones.
constexpr Preset Presets[] = {
    // Potato
    { 0.5f, AntiAliasing::Off, false, 1024, 1, 30.0f, 1.0f, false, 4, false, true, 0.12f, 0.04f },
    // Low
    { 0.75f, AntiAliasing::Fast, true, 1024, 1, 30.0f, 1.0f, false, 4, false, true, 0.08f, 0.03f },
    // Medium
    { 1.0f, AntiAliasing::Balanced, true, 2048, 2, 40.0f, 1.5f, true, 8, true, true, 0.05f, 0.015f },
    // High
    { 1.0f, AntiAliasing::Balanced, true, 2048, 3, 60.0f, 1.5f, true, 12, true, true, 0.05f, 0.015f },
    // Ultra
    { 1.0f, AntiAliasing::High, true, 2048, 4, 90.0f, 2.0f, true, 16, true, true, 0.03f, 0.008f },
    // Extreme
    { 1.5f, AntiAliasing::Balanced, true, 4096, 4, 120.0f, 2.0f, true, 24, true, false, 0.03f, 0.008f },
};

} // namespace

void ApplyGraphicsQuality(RenderSettings& settings, GraphicsQuality quality)
{
    const size_t index = static_cast<size_t>(quality);
    if (index >= sizeof(Presets) / sizeof(Presets[0]))
    {
        return;
    }
    const Preset& preset = Presets[index];

    settings.RenderScale = preset.RenderScale;
    settings.Mode = preset.Mode;

    settings.Shadows.Enabled = preset.Shadows;
    settings.Shadows.Resolution = preset.ShadowResolution;
    settings.Shadows.Cascades = preset.Cascades;
    settings.Shadows.Distance = preset.ShadowDistance;
    settings.Shadows.Softness = preset.ShadowSoftness;

    settings.AmbientOcclusion.Enabled = preset.Occlusion;
    settings.AmbientOcclusion.Samples = preset.OcclusionSamples;

    settings.Bloom.Enabled = preset.Bloom;

    settings.Detail.Enabled = preset.Detail;
    settings.Detail.Simpler = preset.Simpler;
    settings.Detail.Simplest = preset.Simplest;
}

const char* GetGraphicsQualityName(GraphicsQuality quality)
{
    switch (quality)
    {
    case GraphicsQuality::Potato:
        return "Potato";
    case GraphicsQuality::Low:
        return "Low";
    case GraphicsQuality::Medium:
        return "Medium";
    case GraphicsQuality::High:
        return "High";
    case GraphicsQuality::Ultra:
        return "Ultra";
    case GraphicsQuality::Extreme:
        return "Extreme";
    }
    return "Unknown";
}

} // namespace ludifex
