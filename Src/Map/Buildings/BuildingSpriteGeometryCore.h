#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

// Pure mapping from owned building/capture metadata to the world-space
// correspondence of a processed sprite rectangle. Capture bounds and crop
// remain required inputs; callers must not infer them from a transform alone.
namespace MiniMapBuildingSpriteGeometryCore
{
    using Point = std::array<double, 3>;

    inline bool BuildWorldQuad(const Point& position, const Point& scale,
        const std::array<double, 4>& rotation, const Point& captureMin,
        const Point& captureMax, const std::array<double, 4>& sourceRect,
        std::array<Point, 4>& corners)
    {
        double norm = 0;
        for (double q : rotation)
        {
            if (!std::isfinite(q)) return false;
            norm += q * q;
        }
        if (!std::isfinite(norm) || !(norm > 0)) return false;
        const double inverse = 1 / std::sqrt(norm);
        const double x = rotation[0] * inverse, y = rotation[1] * inverse,
            z = rotation[2] * inverse, w = rotation[3] * inverse;

        std::array<double, 3> center{}, extent{};
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            if (!std::isfinite(position[axis]) || !std::isfinite(scale[axis]) ||
                !std::isfinite(captureMin[axis]) || !std::isfinite(captureMax[axis]) ||
                captureMax[axis] < captureMin[axis]) return false;
            center[axis] = captureMin[axis] * 0.5 + captureMax[axis] * 0.5;
            extent[axis] = captureMax[axis] * 0.5 - captureMin[axis] * 0.5;
        }
        if (!(extent[0] > 0 && extent[1] > 0)) return false;

        // Match the float OrthoWidth actually assigned by the capture setup.
        const double halfWidth = double(float(2 * (std::max)(extent[0], extent[1]) * 1.1)) * 0.5;
        if (!std::isfinite(halfWidth) || !(halfWidth > 0)) return false;
        for (double value : sourceRect) if (!std::isfinite(value)) return false;
        if (sourceRect[0] < 0 || sourceRect[1] < 0 || sourceRect[0] >= 256 ||
            sourceRect[1] >= 256 || sourceRect[2] <= sourceRect[0] ||
            sourceRect[3] <= sourceRect[1]) return false;

        const double left = sourceRect[0], top = sourceRect[1];
        const double right = sourceRect[2], bottom = sourceRect[3];
        const std::array<std::array<double, 2>, 4> source{{
            {left, top}, {right, top}, {right, bottom}, {left, bottom}}};
        std::array<Point, 4> result{};
        for (std::size_t i = 0; i < source.size(); ++i)
        {
            // Capture rotation (-90,-90,0): image right=+X, image down=+Y.
            // Source pixel boundaries preserve asymmetric crop and actor-origin offset.
            const Point v{{
                (center[0] + halfWidth * (2 * source[i][0] / 256 - 1)) * scale[0],
                (center[1] + halfWidth * (2 * source[i][1] / 256 - 1)) * scale[1],
                center[2] * scale[2]}};
            for (double value : v) if (!std::isfinite(value)) return false;
            const double tx = 2 * (y * v[2] - z * v[1]);
            const double ty = 2 * (z * v[0] - x * v[2]);
            const double tz = 2 * (x * v[1] - y * v[0]);
            result[i] = {{
                position[0] + v[0] + w * tx + y * tz - z * ty,
                position[1] + v[1] + w * ty + z * tx - x * tz,
                position[2] + v[2] + w * tz + x * ty - y * tx}};
            for (double value : result[i]) if (!std::isfinite(value)) return false;
        }
        corners = result;
        return true;
    }
}
