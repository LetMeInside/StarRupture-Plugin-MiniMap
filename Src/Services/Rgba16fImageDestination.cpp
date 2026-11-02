#include "Rgba16fImageDestination.h"

#include <algorithm>
#include <cmath>
#include <climits>
#include <limits>

namespace MiniMap::Services
{
    namespace Core = MiniMapBuildingImageRasterizationCore;
    namespace
    {
        bool ValidGrid(const ImageWorldGrid& g) noexcept
        {
            if (!g.Width || !g.Height || g.Width > INT_MAX || g.Height > INT_MAX ||
                !std::isfinite(g.WorldOriginX) || !std::isfinite(g.WorldOriginY) ||
                !std::isfinite(g.WorldUnitsPerPixel) || g.WorldUnitsPerPixel <= 0) return false;
            const size_t pixels = size_t(g.Width) * size_t(g.Height);
            if (pixels / g.Width != g.Height || pixels > (std::numeric_limits<size_t>::max)() / Core::kSamplesPerPixel) return false;
            const long double left = static_cast<long double>(g.WorldOriginX) + static_cast<long double>(g.CellX) * g.Width * g.WorldUnitsPerPixel;
            const long double bottom = static_cast<long double>(g.WorldOriginY) + static_cast<long double>(g.CellY) * g.Height * g.WorldUnitsPerPixel;
            return std::isfinite(left) && std::isfinite(bottom) &&
                std::isfinite(left + static_cast<long double>(g.Width) * g.WorldUnitsPerPixel) &&
                std::isfinite(bottom + static_cast<long double>(g.Height) * g.WorldUnitsPerPixel);
        }

        bool ValidCommand(const ImageDrawingCommand& c) noexcept
        {
            if (!c.Pixels || c.Format != ImagePixelFormat::Rgba8 || !c.Width || !c.Height) return false;
            const size_t pixels = size_t(c.Width) * size_t(c.Height);
            if (pixels / c.Width != c.Height || pixels > (std::numeric_limits<size_t>::max)() / 4 || c.Pixels->size() != pixels * 4) return false;
            const auto& r = c.SourceRegion;
            for (double v : r) if (!std::isfinite(v)) return false;
            return r[0] >= 0 && r[1] >= 0 && r[2] > r[0] && r[3] > r[1] && r[2] <= c.Width && r[3] <= c.Height;
        }

        bool IntersectsGrid(const ImageWorldGrid& g, const std::array<std::array<double, 2>, 4>& corners) noexcept
        {
            double minX = corners[0][0], maxX = minX, minY = corners[0][1], maxY = minY;
            for (const auto& p : corners)
            { minX = (std::min)(minX, p[0]); maxX = (std::max)(maxX, p[0]); minY = (std::min)(minY, p[1]); maxY = (std::max)(maxY, p[1]); }
            const long double left = static_cast<long double>(g.WorldOriginX) + static_cast<long double>(g.CellX) * g.Width * g.WorldUnitsPerPixel;
            const long double bottom = static_cast<long double>(g.WorldOriginY) + static_cast<long double>(g.CellY) * g.Height * g.WorldUnitsPerPixel;
            const long double right = left + static_cast<long double>(g.Width) * g.WorldUnitsPerPixel;
            const long double top = bottom + static_cast<long double>(g.Height) * g.WorldUnitsPerPixel;
            if (!std::isfinite(left) || !std::isfinite(bottom) || !std::isfinite(right) || !std::isfinite(top)) return false;
            return !(static_cast<long double>(maxX) < left || static_cast<long double>(minX) > right ||
                static_cast<long double>(maxY) < bottom || static_cast<long double>(minY) > top);
        }

        struct ShadeContext
        {
            const ImageDrawingCommand& Command;
            const std::array<Core::Vertex, 4>& Corners;
            bool Other = false;
        };

        bool Shade(const ShadeContext& ctx, double a, double b, double c, Core::Pixel& color) noexcept
        {
            const auto& q = ctx.Corners;
            const size_t i0 = 0, i1 = ctx.Other ? 2 : 1, i2 = ctx.Other ? 3 : 2;
            const double u = q[i0].U * a + q[i1].U * b + q[i2].U * c;
            const double v = q[i0].V * a + q[i1].V * b + q[i2].V * c;
            const size_t byteCount = ctx.Command.Pixels->size();
            return Core::SampleRgba8(ctx.Command.Pixels->data(), byteCount, ctx.Command.Width, ctx.Command.Height,
                ctx.Command.SourceRegion, u, v, color);
        }
    }

    Rgba16fImageDestination::Rgba16fImageDestination(ImageWorldGrid grid) noexcept : m_grid(grid)
    {
        if (!ValidGrid(grid)) return;
        try
        {
            m_rasterGrid = {grid.WorldOriginX, grid.WorldOriginY, grid.CellX, grid.CellY,
                grid.Width, grid.Height, grid.WorldUnitsPerPixel};
            const size_t count = size_t(grid.Width) * grid.Height * Core::kSamplesPerPixel;
            m_samples.resize(count);
            for (auto& sample : m_samples) sample = {0, 0, 0, 1};
            m_initialized = true;
        }
        catch (...) { m_samples.clear(); m_failed = true; }
    }

    ImageSubmission Rgba16fImageDestination::SubmitImage(const ImageDrawingCommand& command)
    {
        if (!m_initialized || m_failed) return ImageSubmission::Unavailable;
        if (!ValidCommand(command)) return ImageSubmission::InvalidGeometry;
        std::array<Core::Vertex, 4> corners{};
        constexpr std::array<std::array<double, 2>, 4> uv{{{{0, 0}}, {{1, 0}}, {{1, 1}}, {{0, 1}}}};
        for (size_t i = 0; i < corners.size(); ++i)
        {
            const double x = command.WorldCorners[i][0], y = command.WorldCorners[i][1];
            if (!std::isfinite(x) || !std::isfinite(y)) return ImageSubmission::InvalidGeometry;
            corners[i] = {x, y, uv[i][0], uv[i][1]};
        }
        if (!IntersectsGrid(m_grid, command.WorldCorners)) return ImageSubmission::Culled;
        const size_t before = m_samples.size();
        ShadeContext first{command, corners, false}, second{command, corners, true};
        bool okay = Core::RasterTriangle({corners[0], corners[1], corners[2]}, m_rasterGrid, m_samples,
            [&](double a, double b, double c, Core::Pixel& out) { return Shade(first, a, b, c, out); });
        if (okay) okay = Core::RasterTriangle({corners[0], corners[2], corners[3]}, m_rasterGrid, m_samples,
            [&](double a, double b, double c, Core::Pixel& out) { return Shade(second, a, b, c, out); });
        if (!okay || m_samples.size() != before)
        {
            m_failed = true;
            return ImageSubmission::Rejected;
        }
        return ImageSubmission::Submitted;
    }

    ImageSubmission Rgba16fImageDestination::SubmitSolidTriangle(
        const std::array<std::array<double, 3>, 3>& worldTriangle,
        uint32_t rgb, uint8_t opacity) noexcept
    {
        if (!m_initialized || m_failed) return ImageSubmission::Unavailable;
        for (const auto& vertex : worldTriangle)
            if (!std::isfinite(vertex[0]) || !std::isfinite(vertex[1]))
                return ImageSubmission::InvalidGeometry;

        const long double left = static_cast<long double>(m_grid.WorldOriginX) +
            static_cast<long double>(m_grid.CellX) * m_grid.Width * m_grid.WorldUnitsPerPixel;
        const long double bottom = static_cast<long double>(m_grid.WorldOriginY) +
            static_cast<long double>(m_grid.CellY) * m_grid.Height * m_grid.WorldUnitsPerPixel;
        const long double right = left + static_cast<long double>(m_grid.Width) * m_grid.WorldUnitsPerPixel;
        const long double top = bottom + static_cast<long double>(m_grid.Height) * m_grid.WorldUnitsPerPixel;
        long double minX = worldTriangle[0][0], maxX = minX, minY = worldTriangle[0][1], maxY = minY;
        for (const auto& vertex : worldTriangle)
        {
            minX = (std::min)(minX, static_cast<long double>(vertex[0]));
            maxX = (std::max)(maxX, static_cast<long double>(vertex[0]));
            minY = (std::min)(minY, static_cast<long double>(vertex[1]));
            maxY = (std::max)(maxY, static_cast<long double>(vertex[1]));
        }
        if (maxX < left || minX > right || maxY < bottom || minY > top)
            return ImageSubmission::Culled;

        const size_t before = m_samples.size();
        if (!Core::RasterSolidTriangle(worldTriangle, m_rasterGrid, m_samples, rgb, opacity) ||
            m_samples.size() != before)
        {
            m_failed = true;
            return ImageSubmission::Rejected;
        }
        return ImageSubmission::Submitted;
    }

    bool Rgba16fImageDestination::Finalize(std::vector<uint8_t>& output) const noexcept
    {
        output.clear();
        if (!m_initialized || m_failed) return false;
        try
        {
            if (Core::Resolve(m_samples, m_grid.Width, m_grid.Height, output)) return true;
            output.clear();
            return false;
        }
        catch (...) { output.clear(); return false; }
    }
}
