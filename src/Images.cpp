#include "Images.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// stb_image is compiled with internal linkage so that opane, which carries
// its own copy, can be linked into the same program without the two colliding.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4505) // unreferenced local function removed
#pragma warning(disable : 4244) // conversion, possible loss of data
#pragma warning(disable : 4100) // unreferenced formal parameter
#pragma warning(disable : 4456) // declaration hides previous
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include <stb_image.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace ludifex::detail
{
namespace
{

float SrgbToLinear(float value)
{
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

float LinearToSrgb(float value)
{
    return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

struct LinearTable
{
    float Values[256];

    LinearTable()
    {
        for (int index = 0; index < 256; ++index)
        {
            Values[index] = SrgbToLinear(static_cast<float>(index) / 255.0f);
        }
    }
};

const LinearTable& Linear()
{
    static const LinearTable table;
    return table;
}

uint8_t ToByte(float value)
{
    return static_cast<uint8_t>(std::clamp(value * 255.0f + 0.5f, 0.0f, 255.0f));
}

// Gives every fully transparent texel the colour of its nearest visible one, by
// a breadth-first flood outward from all visible texels at once. Alpha is left
// alone, so nothing that was invisible becomes visible.
void DilateIntoTransparent(std::vector<uint8_t>& pixels, int width, int height)
{
    const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);

    std::vector<uint8_t> filled(count, 0);
    std::vector<uint32_t> frontier;
    frontier.reserve(count);

    for (size_t index = 0; index < count; ++index)
    {
        if (pixels[index * 4 + 3] != 0)
        {
            filled[index] = 1;
            frontier.push_back(static_cast<uint32_t>(index));
        }
    }

    // Wholly transparent or wholly opaque: nothing to spread, or nowhere to
    // spread it.
    if (frontier.empty() || frontier.size() == count)
    {
        return;
    }

    for (size_t head = 0; head < frontier.size(); ++head)
    {
        const uint32_t index = frontier[head];
        const int x = static_cast<int>(index % static_cast<uint32_t>(width));
        const int y = static_cast<int>(index / static_cast<uint32_t>(width));

        const int neighbours[4][2] = { { x - 1, y }, { x + 1, y }, { x, y - 1 }, { x, y + 1 } };
        for (const auto& neighbour : neighbours)
        {
            if (neighbour[0] < 0 || neighbour[0] >= width || neighbour[1] < 0 || neighbour[1] >= height)
            {
                continue;
            }

            const size_t next = static_cast<size_t>(neighbour[1]) * static_cast<size_t>(width) +
                                static_cast<size_t>(neighbour[0]);
            if (filled[next])
            {
                continue;
            }

            filled[next] = 1;
            std::memcpy(&pixels[next * 4], &pixels[static_cast<size_t>(index) * 4], 3);
            frontier.push_back(static_cast<uint32_t>(next));
        }
    }
}

std::vector<uint8_t> Downsample(const std::vector<uint8_t>& source, int sourceWidth, int sourceHeight,
                                int width, int height)
{
    const LinearTable& linear = Linear();
    std::vector<uint8_t> result(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            float weightedR = 0.0f, weightedG = 0.0f, weightedB = 0.0f, alphaSum = 0.0f;
            float plainR = 0.0f, plainG = 0.0f, plainB = 0.0f;
            int taps = 0;

            // A 2x2 footprint, clamped at the edge so an odd dimension still
            // covers its last row or column.
            for (int dy = 0; dy < 2; ++dy)
            {
                for (int dx = 0; dx < 2; ++dx)
                {
                    const int sx = std::min(x * 2 + dx, sourceWidth - 1);
                    const int sy = std::min(y * 2 + dy, sourceHeight - 1);
                    const uint8_t* texel =
                        &source[(static_cast<size_t>(sy) * static_cast<size_t>(sourceWidth) +
                                 static_cast<size_t>(sx)) * 4];

                    const float r = linear.Values[texel[0]];
                    const float g = linear.Values[texel[1]];
                    const float b = linear.Values[texel[2]];
                    const float a = static_cast<float>(texel[3]) / 255.0f;

                    weightedR += r * a;
                    weightedG += g * a;
                    weightedB += b * a;
                    alphaSum += a;

                    plainR += r;
                    plainG += g;
                    plainB += b;
                    ++taps;
                }
            }

            float r, g, b;
            if (alphaSum > 0.0f)
            {
                r = weightedR / alphaSum;
                g = weightedG / alphaSum;
                b = weightedB / alphaSum;
            }
            else
            {
                // Invisible either way, but its colour still bleeds into
                // visible neighbours under filtering, so keep it sensible.
                r = plainR / static_cast<float>(taps);
                g = plainG / static_cast<float>(taps);
                b = plainB / static_cast<float>(taps);
            }

            uint8_t* out = &result[(static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4];
            out[0] = ToByte(LinearToSrgb(r));
            out[1] = ToByte(LinearToSrgb(g));
            out[2] = ToByte(LinearToSrgb(b));
            out[3] = ToByte(alphaSum / static_cast<float>(taps));
        }
    }

    return result;
}

} // namespace

bool DecodeImage(const uint8_t* bytes, size_t size, DecodedImage& outImage, std::string& outError)
{
    if (bytes == nullptr || size == 0)
    {
        outError = "the file is empty";
        return false;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(bytes, static_cast<int>(size), &width, &height, &channels, 4);
    if (pixels == nullptr)
    {
        const char* reason = stbi_failure_reason();
        outError = reason != nullptr ? reason : "unrecognised image data";
        return false;
    }

    outImage.Width = width;
    outImage.Height = height;
    outImage.Pixels.assign(pixels, pixels + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    stbi_image_free(pixels);
    return true;
}

DecodedImage MakeCheckerboard(int size, int cellSize)
{
    DecodedImage image;
    image.Width = size;
    image.Height = size;
    image.Pixels.resize(static_cast<size_t>(size) * static_cast<size_t>(size) * 4);

    for (int y = 0; y < size; ++y)
    {
        for (int x = 0; x < size; ++x)
        {
            const bool magenta = ((x / cellSize) + (y / cellSize)) % 2 == 0;
            uint8_t* texel = &image.Pixels[(static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)) * 4];
            texel[0] = magenta ? 255 : 0;
            texel[1] = 0;
            texel[2] = magenta ? 255 : 0;
            texel[3] = 255;
        }
    }

    return image;
}

namespace
{

// A 2x2 box filter over the stored values, for textures whose values are not
// colours. Normals are decoded to vectors, averaged, and renormalized, so the
// shrunken map still describes surfaces rather than their average tilt.
std::vector<uint8_t> DownsampleData(const std::vector<uint8_t>& source, int sourceWidth, int sourceHeight,
                                    int width, int height, bool normals)
{
    std::vector<uint8_t> result(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            float sum[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            for (int dy = 0; dy < 2; ++dy)
            {
                for (int dx = 0; dx < 2; ++dx)
                {
                    const int sx = std::min(x * 2 + dx, sourceWidth - 1);
                    const int sy = std::min(y * 2 + dy, sourceHeight - 1);
                    const uint8_t* texel =
                        &source[(static_cast<size_t>(sy) * static_cast<size_t>(sourceWidth) +
                                 static_cast<size_t>(sx)) * 4];
                    for (int channel = 0; channel < 4; ++channel)
                    {
                        sum[channel] += static_cast<float>(texel[channel]);
                    }
                }
            }

            uint8_t* out = &result[(static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4];
            if (normals)
            {
                float nx = sum[0] / 4.0f / 127.5f - 1.0f;
                float ny = sum[1] / 4.0f / 127.5f - 1.0f;
                float nz = sum[2] / 4.0f / 127.5f - 1.0f;
                const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (length > 1e-6f)
                {
                    nx /= length;
                    ny /= length;
                    nz /= length;
                }
                else
                {
                    nx = 0.0f;
                    ny = 0.0f;
                    nz = 1.0f;
                }
                out[0] = static_cast<uint8_t>(std::clamp((nx + 1.0f) * 127.5f + 0.5f, 0.0f, 255.0f));
                out[1] = static_cast<uint8_t>(std::clamp((ny + 1.0f) * 127.5f + 0.5f, 0.0f, 255.0f));
                out[2] = static_cast<uint8_t>(std::clamp((nz + 1.0f) * 127.5f + 0.5f, 0.0f, 255.0f));
                out[3] = static_cast<uint8_t>(std::clamp(sum[3] / 4.0f + 0.5f, 0.0f, 255.0f));
            }
            else
            {
                for (int channel = 0; channel < 4; ++channel)
                {
                    out[channel] = static_cast<uint8_t>(std::clamp(sum[channel] / 4.0f + 0.5f, 0.0f, 255.0f));
                }
            }
        }
    }
    return result;
}

} // namespace

std::vector<std::vector<uint8_t>> BuildMipChain(const DecodedImage& image, TextureUsage usage)
{
    std::vector<std::vector<uint8_t>> levels;
    if (image.Width <= 0 || image.Height <= 0)
    {
        return levels;
    }

    std::vector<uint8_t> base = image.Pixels;

    // Dilating colour under transparent texels keeps a cutout's edge from
    // darkening as it shrinks; data has no such edge to protect.
    if (usage == TextureUsage::Color)
    {
        DilateIntoTransparent(base, image.Width, image.Height);
    }
    levels.push_back(std::move(base));

    int width = image.Width;
    int height = image.Height;
    while (width > 1 || height > 1)
    {
        const int nextWidth = std::max(1, width / 2);
        const int nextHeight = std::max(1, height / 2);
        if (usage == TextureUsage::Color)
        {
            levels.push_back(Downsample(levels.back(), width, height, nextWidth, nextHeight));
        }
        else
        {
            levels.push_back(DownsampleData(levels.back(), width, height, nextWidth, nextHeight,
                                            usage == TextureUsage::Normal));
        }
        width = nextWidth;
        height = nextHeight;
    }

    return levels;
}

} // namespace ludifex::detail
