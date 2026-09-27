// Clustered forward lighting.
//
// A forward renderer that loops every light for every pixel gets slower in
// proportion to the number of lights, whether or not they reach the pixel. A
// clustered one cuts the view into a grid of cells (a tile on the screen and a
// slice in depth), works out once per frame which lights reach which cells,
// and then each pixel shades against the few lights in its own cell.
//
// The slices are spaced logarithmically, because perspective makes a metre
// near the camera worth far more than a metre far away. The assignment is done
// here on the CPU, over the lights that survived culling. Each light only
// visits the cells its screen rectangle and depth range touch, which keeps
// the assignment cheap.

#include "Render3D.h"

#include "Profile.h"

#include <algorithm>
#include <cmath>

namespace ludifex::detail
{
namespace
{

struct ViewLight
{
    float CenterX = 0.0f; // view space: x right, y up, z forward (positive)
    float CenterY = 0.0f;
    float Depth = 0.0f;
    float Radius = 0.0f;
};

// The nearest and farthest slice a depth range touches. Slices are
// logarithmic: k = log(z / near) / log(far / near) * slices.
int SliceFor(float depth, float nearPlane, float scale, float bias, int slices)
{
    if (depth <= nearPlane)
    {
        return 0;
    }
    const int slice = static_cast<int>(std::log(depth) * scale + bias);
    return std::clamp(slice, 0, slices - 1);
}

float SliceDepth(int slice, float nearPlane, float farPlane, int slices)
{
    const float ratio = static_cast<float>(slice) / static_cast<float>(slices);
    return nearPlane * std::pow(farPlane / nearPlane, ratio);
}

// The view-space extent of one screen tile at a given depth. The frustum is
// symmetric, so a tile's bounds are a straight interpolation across it.
void TileBounds(int tile, int tileCount, float halfExtentAtDepth, float& minimum, float& maximum)
{
    const float low = static_cast<float>(tile) / static_cast<float>(tileCount) * 2.0f - 1.0f;
    const float high = static_cast<float>(tile + 1) / static_cast<float>(tileCount) * 2.0f - 1.0f;
    minimum = low * halfExtentAtDepth;
    maximum = high * halfExtentAtDepth;
}

float DistanceToRange(float value, float minimum, float maximum)
{
    if (value < minimum)
    {
        return minimum - value;
    }
    if (value > maximum)
    {
        return value - maximum;
    }
    return 0.0f;
}

} // namespace

void Renderer3D::BuildClusters(const FrameCamera& camera, float aspect)
{
    LUDIFEX_PROFILE("light clusters");

    m_ClusterRanges.assign(static_cast<size_t>(ClusterCount) * 2, 0u);
    m_ClusterIndices.clear();

    if (m_Lights.empty())
    {
        m_ClusterScale = 0.0f;
        m_ClusterBias = 0.0f;
        return;
    }

    const float nearPlane = std::max(camera.NearPlane, 1e-3f);
    const float farPlane = std::max(camera.FarPlane, nearPlane * 2.0f);

    // z = near * (far / near) ^ (k / slices), inverted for the shader.
    const float logRatio = std::log(farPlane / nearPlane);
    m_ClusterScale = static_cast<float>(ClusterSlices) / logRatio;
    m_ClusterBias = -m_ClusterScale * std::log(nearPlane);

    // The camera's own axes, so a world position can be put into view space
    // without carrying a matrix through.
    const Vec3 forward = NormalizeVector(camera.Forward);
    const Vec3 right = NormalizeVector(CrossProduct(forward, camera.Up));
    const Vec3 up = CrossProduct(right, forward);

    const float tangent = std::tan(camera.FieldOfViewRadians * 0.5f);

    // Each cluster keeps the lights that reach it.
    std::vector<std::vector<uint32_t>> cells(ClusterCount);

    for (uint32_t index = 0; index < m_Lights.size(); ++index)
    {
        const GpuPointLight& light = m_Lights[index];
        const Vec3 offset{ light.PositionRange[0] - camera.Position.X,
                           light.PositionRange[1] - camera.Position.Y,
                           light.PositionRange[2] - camera.Position.Z };

        ViewLight view;
        view.Depth = DotProduct(offset, forward);
        view.CenterX = DotProduct(offset, right);
        view.CenterY = DotProduct(offset, up);
        view.Radius = light.PositionRange[3];

        if (view.Depth + view.Radius < nearPlane || view.Depth - view.Radius > farPlane)
        {
            continue;
        }

        const int firstSlice =
            SliceFor(view.Depth - view.Radius, nearPlane, m_ClusterScale, m_ClusterBias, ClusterSlices);
        const int lastSlice =
            SliceFor(view.Depth + view.Radius, nearPlane, m_ClusterScale, m_ClusterBias, ClusterSlices);

        for (int slice = firstSlice; slice <= lastSlice; ++slice)
        {
            // The slab of depth this slice covers, and how wide the view is
            // across it. Using the far edge for the extent keeps the test
            // conservative: a cell is never missed, only occasionally
            // included when it did not need to be.
            const float sliceNear = SliceDepth(slice, nearPlane, farPlane, ClusterSlices);
            const float sliceFar = SliceDepth(slice + 1, nearPlane, farPlane, ClusterSlices);

            const float halfHeight = tangent * sliceFar;
            const float halfWidth = halfHeight * aspect;

            const float depthDistance = DistanceToRange(view.Depth, sliceNear, sliceFar);
            if (depthDistance > view.Radius)
            {
                continue;
            }

            // What is left of the radius once the depth has taken its share.
            const float remaining =
                std::sqrt(std::max(0.0f, view.Radius * view.Radius - depthDistance * depthDistance));

            for (int row = 0; row < static_cast<int>(ClusterRows); ++row)
            {
                float rowMinimum = 0.0f;
                float rowMaximum = 0.0f;
                TileBounds(row, static_cast<int>(ClusterRows), halfHeight, rowMinimum, rowMaximum);

                const float rowDistance = DistanceToRange(view.CenterY, rowMinimum, rowMaximum);
                if (rowDistance > remaining)
                {
                    continue;
                }

                const float acrossRow =
                    std::sqrt(std::max(0.0f, remaining * remaining - rowDistance * rowDistance));

                for (int column = 0; column < static_cast<int>(ClusterColumns); ++column)
                {
                    float columnMinimum = 0.0f;
                    float columnMaximum = 0.0f;
                    TileBounds(column, static_cast<int>(ClusterColumns), halfWidth, columnMinimum,
                               columnMaximum);

                    if (DistanceToRange(view.CenterX, columnMinimum, columnMaximum) > acrossRow)
                    {
                        continue;
                    }

                    const size_t cell = static_cast<size_t>(slice) * ClusterRows * ClusterColumns +
                                        static_cast<size_t>(row) * ClusterColumns +
                                        static_cast<size_t>(column);
                    if (cells[cell].size() < MaxLightsPerCluster)
                    {
                        cells[cell].push_back(index);
                    }
                }
            }
        }
    }

    // Flattened into one list, with each cell naming where its own slice of it
    // starts and how long it is.
    for (size_t cell = 0; cell < cells.size(); ++cell)
    {
        m_ClusterRanges[cell * 2] = static_cast<uint32_t>(m_ClusterIndices.size());
        m_ClusterRanges[cell * 2 + 1] = static_cast<uint32_t>(cells[cell].size());
        m_ClusterIndices.insert(m_ClusterIndices.end(), cells[cell].begin(), cells[cell].end());
    }

    // A buffer is never empty, so the shader always has something to bind.
    if (m_ClusterIndices.empty())
    {
        m_ClusterIndices.push_back(0);
    }
}

} // namespace ludifex::detail
