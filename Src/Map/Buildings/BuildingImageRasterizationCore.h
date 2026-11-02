#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

// Shared CPU rasterization primitives for building image composition.
namespace MiniMapBuildingImageRasterizationCore
{
    struct Grid
    {
        double OriginX = 0, OriginY = 0;
        int64_t CellX = 0, CellY = 0;
        uint32_t Width = 0, Height = 0;
        double WorldUnitsPerPixel = 0;
    };

    struct Vertex { double X = 0, Y = 0, U = 0, V = 0; };
    struct Pixel { float R = 0, G = 0, B = 0, A = 0; };
    using SampleBuffer = std::vector<std::array<float, 4>>;
    constexpr size_t kSamplesPerPixel = 4;

    inline double SrgbToLinear(uint8_t channel) noexcept
    {
        static const std::array<double, 256> lookup = [] {
            std::array<double, 256> values{};
            for (size_t i = 0; i < values.size(); ++i)
            {
                const double value = double(i) / 255.0;
                values[i] = value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
            }
            return values;
        }();
        return lookup[channel];
    }

    inline bool SampleRgba8(const uint8_t* bytes, size_t byteCount, uint32_t width, uint32_t height,
        const std::array<double, 4>& region, double u, double v, Pixel& output) noexcept
    {
        if (!bytes || !width || !height || !std::isfinite(u) || !std::isfinite(v)) return false;
        const double rw = region[2] - region[0], rh = region[3] - region[1];
        if (!std::isfinite(rw) || !std::isfinite(rh) || rw <= 0 || rh <= 0) return false;
        const double x = region[0] + (std::clamp)(u, 0.0, 1.0) * rw - 0.5;
        const double y = region[1] + (std::clamp)(v, 0.0, 1.0) * rh - 0.5;
        const double fx = std::floor(x), fy = std::floor(y);
        const int64_t x0 = static_cast<int64_t>(fx), y0 = static_cast<int64_t>(fy);
        const double tx = x - fx, ty = y - fy;
        const int64_t minX = static_cast<int64_t>(std::ceil(region[0] - 0.5));
        const int64_t minY = static_cast<int64_t>(std::ceil(region[1] - 0.5));
        const int64_t maxX = static_cast<int64_t>(std::ceil(region[2] - 0.5)) - 1;
        const int64_t maxY = static_cast<int64_t>(std::ceil(region[3] - 0.5)) - 1;
        auto fetch = [&](int64_t px, int64_t py, Pixel& color) {
            px = (std::clamp)(px, (std::max)(int64_t(0), minX), (std::min)(int64_t(width) - 1, maxX));
            py = (std::clamp)(py, (std::max)(int64_t(0), minY), (std::min)(int64_t(height) - 1, maxY));
            if (px < 0 || py < 0) return false;
            const size_t offset = (size_t(py) * width + size_t(px)) * 4;
            if (offset > byteCount || byteCount - offset < 4) return false;
            color = {float(SrgbToLinear(bytes[offset])), float(SrgbToLinear(bytes[offset + 1])),
                float(SrgbToLinear(bytes[offset + 2])), float(bytes[offset + 3]) / 255.0f};
            return true;
        };
        Pixel p00, p10, p01, p11;
        if (!fetch(x0, y0, p00) || !fetch(x0 + 1, y0, p10) ||
            !fetch(x0, y0 + 1, p01) || !fetch(x0 + 1, y0 + 1, p11)) return false;
        auto mix = [&](float Pixel::*member) {
            const double a = p00.*member * (1 - tx) + p10.*member * tx;
            const double b = p01.*member * (1 - tx) + p11.*member * tx;
            return float(a * (1 - ty) + b * ty);
        };
        output = {mix(&Pixel::R), mix(&Pixel::G), mix(&Pixel::B), mix(&Pixel::A)};
        return true;
    }

    inline double Edge(const Vertex& a, const Vertex& b, double x, double y) noexcept
    { return (b.X - a.X) * (y - a.Y) - (b.Y - a.Y) * (x - a.X); }
    inline bool TopLeft(const Vertex& a, const Vertex& b) noexcept
    {
        const double dx = b.X - a.X, dy = b.Y - a.Y;
        return dy < 0 || (dy == 0 && dx > 0);
    }
    inline double GlobalPixel(int64_t cell, size_t pixel, double sample, double origin,
        uint32_t side, double worldPerPixel) noexcept
    {
        const long double index = static_cast<long double>(cell) * side +
            static_cast<long double>(pixel) + sample;
        return static_cast<double>(static_cast<long double>(origin) +
            index * static_cast<long double>(worldPerPixel));
    }

    template<class Shade>
    bool RasterTriangle(std::array<Vertex, 3> vertices, const Grid& grid,
        SampleBuffer& samples, Shade&& shade) noexcept
    {
        constexpr std::array<std::array<double, 2>, 4> offsets{{
            {{0.25, 0.25}}, {{0.75, 0.25}}, {{0.25, 0.75}}, {{0.75, 0.75}}}};
        for (const auto& vertex : vertices)
            if (!std::isfinite(vertex.X) || !std::isfinite(vertex.Y) || !std::isfinite(vertex.U) || !std::isfinite(vertex.V)) return false;
        double area = Edge(vertices[0], vertices[1], vertices[2].X, vertices[2].Y);
        if (!std::isfinite(area)) return false;
        if (area == 0) return true;
        const bool swapped = area < 0;
        if (swapped) { std::swap(vertices[1], vertices[2]); area = -area; }
        const long double sideX = grid.Width, sideY = grid.Height;
        long double minX = std::numeric_limits<long double>::infinity(), minY = minX;
        long double maxX = -minX, maxY = -minX;
        for (const auto& vertex : vertices)
        {
            const long double gx = (static_cast<long double>(vertex.X) - grid.OriginX) / grid.WorldUnitsPerPixel -
                static_cast<long double>(grid.CellX) * sideX;
            const long double gy = (static_cast<long double>(vertex.Y) - grid.OriginY) / grid.WorldUnitsPerPixel -
                static_cast<long double>(grid.CellY) * sideY;
            minX = (std::min)(minX, gx); maxX = (std::max)(maxX, gx);
            minY = (std::min)(minY, gy); maxY = (std::max)(maxY, gy);
        }
        if (!std::isfinite(minX) || !std::isfinite(maxX) || !std::isfinite(minY) || !std::isfinite(maxY)) return false;
        if (maxX < 0 || maxY < 0 || minX > sideX || minY > sideY) return true;
        auto low = [](long double v, uint32_t extent) { return int((std::max)(0.0L, std::floor((std::min)(v, static_cast<long double>(extent))) - 1.0L)); };
        auto high = [](long double v, uint32_t extent) { return int((std::min)(static_cast<long double>(extent - 1), std::ceil((std::max)(v, 0.0L)) + 1.0L)); };
        const int x0 = low(minX, grid.Width), y0 = low(minY, grid.Height), x1 = high(maxX, grid.Width), y1 = high(maxY, grid.Height);
        if (x0 > x1 || y0 > y1) return true;
        const bool in0 = TopLeft(vertices[1], vertices[2]), in1 = TopLeft(vertices[2], vertices[0]), in2 = TopLeft(vertices[0], vertices[1]);
        for (int py = y0; py <= y1; ++py) for (int px = x0; px <= x1; ++px)
        {
            const size_t pixel = size_t(py) * grid.Width + size_t(px);
            for (size_t si = 0; si < offsets.size(); ++si)
            {
                const auto& off = offsets[si];
                const double wx = GlobalPixel(grid.CellX, size_t(px), off[0], grid.OriginX, grid.Width, grid.WorldUnitsPerPixel);
                const double wy = GlobalPixel(grid.CellY, size_t(py), off[1], grid.OriginY, grid.Height, grid.WorldUnitsPerPixel);
                if (!std::isfinite(wx) || !std::isfinite(wy)) return false;
                const double e0 = Edge(vertices[1], vertices[2], wx, wy), e1 = Edge(vertices[2], vertices[0], wx, wy), e2 = Edge(vertices[0], vertices[1], wx, wy);
                if (e0 < 0 || (e0 == 0 && !in0) || e1 < 0 || (e1 == 0 && !in1) || e2 < 0 || (e2 == 0 && !in2)) continue;
                Pixel color;
                if (swapped ? !shade(e0 / area, e2 / area, e1 / area, color) : !shade(e0 / area, e1 / area, e2 / area, color)) return false;
                const float a = (std::clamp)(color.A, 0.0f, 1.0f), t = 1.0f - a;
                auto& dst = samples[pixel * kSamplesPerPixel + si];
                dst[0] = color.R * a + dst[0] * t; dst[1] = color.G * a + dst[1] * t;
                dst[2] = color.B * a + dst[2] * t; dst[3] *= t;
            }
        }
        return true;
    }

    inline bool RasterSolidTriangle(const std::array<std::array<double, 3>, 3>& triangle,
        const Grid& grid, SampleBuffer& samples, uint32_t rgb, uint8_t opacity = 255) noexcept
    {
        std::array<Vertex, 3> vertices{};
        for (size_t i = 0; i < vertices.size(); ++i)
            vertices[i] = {triangle[i][0], triangle[i][1], 0, 0};
        const Pixel color{float(SrgbToLinear(uint8_t((rgb >> 16) & 0xFF))),
            float(SrgbToLinear(uint8_t((rgb >> 8) & 0xFF))),
            float(SrgbToLinear(uint8_t(rgb & 0xFF))), float(opacity) / 255.0f};
        return RasterTriangle(vertices, grid, samples,
            [&](double, double, double, Pixel& output) { output = color; return true; });
    }

    inline uint16_t FloatToHalf(float value) noexcept
    {
        const uint32_t bits = std::bit_cast<uint32_t>(value), sign = (bits >> 16) & 0x8000;
        const uint32_t exponent = (bits >> 23) & 0xFF; uint32_t mantissa = bits & 0x7FFFFF;
        if (exponent == 0xFF) return uint16_t(sign | 0x7C00 | (mantissa ? 0x0200 : 0));
        int he = int(exponent) - 127 + 15;
        if (he >= 31) return uint16_t(sign | 0x7C00);
        if (he <= 0)
        {
            if (he < -10) return uint16_t(sign);
            mantissa |= 0x800000; const unsigned shift = unsigned(14 - he);
            uint32_t rounded = mantissa >> shift, rem = mantissa & ((uint32_t(1) << shift) - 1), halfway = uint32_t(1) << (shift - 1);
            if (rem > halfway || (rem == halfway && (rounded & 1))) ++rounded;
            return uint16_t(sign | rounded);
        }
        uint32_t rounded = mantissa >> 13; const uint32_t rem = mantissa & 0x1FFF;
        if (rem > 0x1000 || (rem == 0x1000 && (rounded & 1)))
        { if (++rounded == 0x400) { rounded = 0; if (++he >= 31) return uint16_t(sign | 0x7C00); } }
        return uint16_t(sign | (uint32_t(he) << 10) | rounded);
    }
    inline void StoreHalf(std::vector<uint8_t>& bytes, size_t pixel, size_t channel, float value)
    {
        const uint16_t h = FloatToHalf(value); const size_t at = pixel * 8 + channel * 2;
        bytes[at] = uint8_t(h & 0xFF); bytes[at + 1] = uint8_t(h >> 8);
    }
    inline bool Resolve(const SampleBuffer& samples, uint32_t width, uint32_t height, std::vector<uint8_t>& output)
    {
        const size_t pixels = size_t(width) * height;
        if (samples.size() != pixels * kSamplesPerPixel || pixels > (std::numeric_limits<size_t>::max)() / 8) return false;
        output.resize(pixels * 8);
        for (size_t p = 0; p < pixels; ++p) for (size_t c = 0; c < 4; ++c)
        {
            float v = 0;
            for (size_t s = 0; s < kSamplesPerPixel; ++s) v += samples[p * kSamplesPerPixel + s][c] / float(kSamplesPerPixel);
            if (!std::isfinite(v) || v < 0 || v > 1) return false;
            StoreHalf(output, p, c, v);
        }
        return true;
    }
}
