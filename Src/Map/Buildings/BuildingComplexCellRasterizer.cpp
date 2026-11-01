#include "BuildingComplexCellRasterizer.h"

#if defined(MODLOADER_CLIENT_BUILD)

#include "BuildingCache.h"
#include "BuildingSplinePolicy.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <tuple>

namespace MiniMapBuildingComplexCellRasterizer
{
    namespace
    {
        constexpr size_t kSide = B::kCellImageSizePixels;
        constexpr size_t kSamplesPerAxis = 2;
        constexpr size_t kSamplesPerPixel = kSamplesPerAxis * kSamplesPerAxis;
        constexpr double kPixelWorldSize = B::kCellWorldSizeCm / double(kSide);
        // Matches BuildingVisuals.cpp's fixed kSplineFillOpacity policy.
        constexpr uint8_t kMiniMapSplineFillOpacity = 255;
        // Fixed symmetric 2x2 coverage samples keep the raster deterministic
        // and identical on the shared texel lattice of every neighboring cell.
        constexpr std::array<std::array<double, 2>, kSamplesPerPixel> kSampleOffsets{{
            {{0.25, 0.25}}, {{0.75, 0.25}}, {{0.25, 0.75}}, {{0.75, 0.75}}
        }};

        struct LinearPixel
        {
            float R = 0, G = 0, B = 0, A = 0; // Unassociated linear RGB and coverage.
        };
        struct Vertex
        {
            double X = 0, Y = 0, U = 0, V = 0;
        };

        double SrgbToLinear(uint8_t channel) noexcept
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

        bool Fetch(const MiniMapBuildingCache::Sprite& image, int64_t x, int64_t y,
            LinearPixel& output) noexcept
        {
            if (x < 0 || y < 0 || uint64_t(x) >= image.Width || uint64_t(y) >= image.Height) return false;
            const size_t offset = (size_t(y) * image.Width + size_t(x)) * 4;
            if (offset > image.Rgba.size() || image.Rgba.size() - offset < 4) return false;
            output.R = float(SrgbToLinear(image.Rgba[offset]));
            output.G = float(SrgbToLinear(image.Rgba[offset + 1]));
            output.B = float(SrgbToLinear(image.Rgba[offset + 2]));
            output.A = float(image.Rgba[offset + 3]) / 255.0f;
            return true;
        }

        bool SampleSprite(const MiniMapBuildingCache::Sprite& image, double u, double v,
            LinearPixel& output) noexcept
        {
            if (!std::isfinite(u) || !std::isfinite(v) || !image.Width || !image.Height) return false;
            // Atlas gutters reproduce the edge texel outside the image content;
            // clamping here gives the same source-edge sampling without reading
            // adjacent atlas allocations.
            const double x = (std::clamp)(u, 0.0, 1.0) * image.Width - 0.5;
            const double y = (std::clamp)(v, 0.0, 1.0) * image.Height - 0.5;
            const double floorX = std::floor(x), floorY = std::floor(y);
            const int64_t x0 = static_cast<int64_t>(floorX), y0 = static_cast<int64_t>(floorY);
            const double tx = x - floorX, ty = y - floorY;
            const int64_t maxX = int64_t(image.Width) - 1, maxY = int64_t(image.Height) - 1;
            const int64_t ax = (std::clamp)(x0, int64_t(0), maxX);
            const int64_t bx = (std::clamp)(x0 + 1, int64_t(0), maxX);
            const int64_t ay = (std::clamp)(y0, int64_t(0), maxY);
            const int64_t by = (std::clamp)(y0 + 1, int64_t(0), maxY);
            LinearPixel p00, p10, p01, p11;
            if (!Fetch(image, ax, ay, p00) || !Fetch(image, bx, ay, p10) ||
                !Fetch(image, ax, by, p01) || !Fetch(image, bx, by, p11)) return false;
            auto interpolate = [&](float LinearPixel::* member) {
                const double top = double(p00.*member) * (1.0 - tx) + double(p10.*member) * tx;
                const double bottom = double(p01.*member) * (1.0 - tx) + double(p11.*member) * tx;
                return float(top * (1.0 - ty) + bottom * ty);
            };
            output = {interpolate(&LinearPixel::R), interpolate(&LinearPixel::G),
                interpolate(&LinearPixel::B), interpolate(&LinearPixel::A)};
            return true;
        }

        double Edge(const Vertex& a, const Vertex& b, double x, double y) noexcept
        { return (b.X - a.X) * (y - a.Y) - (b.Y - a.Y) * (x - a.X); }

        bool TopLeft(const Vertex& a, const Vertex& b) noexcept
        {
            const double dx = b.X - a.X, dy = b.Y - a.Y;
            // Cell image Y grows with world +Y. For positive (clockwise-on-
            // screen) winding, include top and left edges exactly once.
            return dy < 0 || (dy == 0 && dx > 0);
        }

        double GlobalPixel(int64_t cell, size_t pixel, double sample, double origin) noexcept
        {
            const long double index = static_cast<long double>(cell) * kSide +
                static_cast<long double>(pixel) + sample;
            return static_cast<double>(static_cast<long double>(origin) +
                index * static_cast<long double>(kPixelWorldSize));
        }

        template<class Shade>
        bool RasterTriangle(std::array<Vertex, 3> vertices, const B::CellSnapshot& snapshot,
            std::vector<std::array<float, 4>>& samples, Shade&& shade) noexcept
        {
            for (const auto& vertex : vertices)
                if (!std::isfinite(vertex.X) || !std::isfinite(vertex.Y) ||
                    !std::isfinite(vertex.U) || !std::isfinite(vertex.V)) return false;
            double area = Edge(vertices[0], vertices[1], vertices[2].X, vertices[2].Y);
            if (!std::isfinite(area)) return false;
            // MiniMap skips exact zero-area triangles. They are valid no-ops,
            // unlike nonfinite input, and must not make a cell Partial.
            if (area == 0) return true;
            const bool swapped = area < 0;
            if (swapped)
            {
                std::swap(vertices[1], vertices[2]);
                area = -area;
            }

            const long double cellX = snapshot.Key.X, cellY = snapshot.Key.Y;
            long double minX = std::numeric_limits<long double>::infinity();
            long double minY = minX, maxX = -minX, maxY = -minX;
            for (const auto& vertex : vertices)
            {
                const long double gx = (static_cast<long double>(vertex.X) - snapshot.Origin.X) / kPixelWorldSize - cellX * kSide;
                const long double gy = (static_cast<long double>(vertex.Y) - snapshot.Origin.Y) / kPixelWorldSize - cellY * kSide;
                minX = (std::min)(minX, gx); maxX = (std::max)(maxX, gx);
                minY = (std::min)(minY, gy); maxY = (std::max)(maxY, gy);
            }
            if (!std::isfinite(minX) || !std::isfinite(maxX) || !std::isfinite(minY) || !std::isfinite(maxY)) return false;
            if (maxX < 0 || maxY < 0 || minX > kSide || minY > kSide) return true;
            const auto low = [](long double value) {
                return int((std::max)(0.0L, std::floor((std::min)(value, static_cast<long double>(kSide))) - 1.0L));
            };
            const auto high = [](long double value) {
                return int((std::min)(static_cast<long double>(kSide - 1), std::ceil((std::max)(value, 0.0L)) + 1.0L));
            };
            const int x0 = low(minX), y0 = low(minY), x1 = high(maxX), y1 = high(maxY);
            if (x0 > x1 || y0 > y1) return true;
            const bool edge0Inclusive = TopLeft(vertices[1], vertices[2]);
            const bool edge1Inclusive = TopLeft(vertices[2], vertices[0]);
            const bool edge2Inclusive = TopLeft(vertices[0], vertices[1]);
            for (int py = y0; py <= y1; ++py)
                for (int px = x0; px <= x1; ++px)
                {
                    const size_t pixel = size_t(py) * kSide + size_t(px);
                    for (size_t sampleIndex = 0; sampleIndex < kSamplesPerPixel; ++sampleIndex)
                    {
                        const auto& offset = kSampleOffsets[sampleIndex];
                        const double worldX = GlobalPixel(snapshot.Key.X, size_t(px), offset[0], snapshot.Origin.X);
                        const double worldY = GlobalPixel(snapshot.Key.Y, size_t(py), offset[1], snapshot.Origin.Y);
                        if (!std::isfinite(worldX) || !std::isfinite(worldY)) return false;
                        const double e0 = Edge(vertices[1], vertices[2], worldX, worldY);
                        const double e1 = Edge(vertices[2], vertices[0], worldX, worldY);
                        const double e2 = Edge(vertices[0], vertices[1], worldX, worldY);
                        if (e0 < 0 || (e0 == 0 && !edge0Inclusive) ||
                            e1 < 0 || (e1 == 0 && !edge1Inclusive) ||
                            e2 < 0 || (e2 == 0 && !edge2Inclusive)) continue;
                        LinearPixel color;
                        if (swapped)
                        {
                            if (!shade(e0 / area, e2 / area, e1 / area, color)) return false;
                        }
                        else if (!shade(e0 / area, e1 / area, e2 / area, color)) return false;
                        const float alpha = (std::clamp)(color.A, 0.0f, 1.0f);
                        const float transmittance = 1.0f - alpha;
                        auto& destination = samples[pixel * kSamplesPerPixel + sampleIndex];
                        destination[0] = color.R * alpha + destination[0] * transmittance;
                        destination[1] = color.G * alpha + destination[1] * transmittance;
                        destination[2] = color.B * alpha + destination[2] * transmittance;
                        destination[3] *= transmittance;
                    }
                }
            return true;
        }

        bool RasterSolid(const std::array<std::array<double, 3>, 3>& triangle,
            const B::CellSnapshot& snapshot, std::vector<std::array<float, 4>>& samples,
            uint32_t rgb) noexcept
        {
            std::array<Vertex, 3> vertices{};
            for (size_t i = 0; i < 3; ++i) vertices[i] = {triangle[i][0], triangle[i][1], 0, 0};
            const LinearPixel color{
                float(SrgbToLinear(uint8_t((rgb >> 16) & 0xFF))),
                float(SrgbToLinear(uint8_t((rgb >> 8) & 0xFF))),
                float(SrgbToLinear(uint8_t(rgb & 0xFF))),
                float(kMiniMapSplineFillOpacity) / 255.0f};
            return RasterTriangle(vertices, snapshot, samples,
                [&](double, double, double, LinearPixel& output) { output = color; return true; });
        }

        bool RasterSprite(const B::SourcePayload& source, const MiniMapBuildingCache::Sprite& image,
            const B::CellSnapshot& snapshot, std::vector<std::array<float, 4>>& samples) noexcept
        {
            if (!source.SpriteQuadValid || !MiniMapBuildingCache::ValidSpatial(
                    image.Width, image.Height, image.Bounds, image.Mapping) ||
                image.Rgba.size() != size_t(image.Width) * image.Height * 4) return false;
            std::array<Vertex, 4> corners{};
            constexpr std::array<std::array<double, 2>, 4> uv{{{{0, 0}}, {{1, 0}}, {{1, 1}}, {{0, 1}}}};
            for (size_t i = 0; i < corners.size(); ++i)
                corners[i] = {source.SpriteQuadWorld[i][0], source.SpriteQuadWorld[i][1], uv[i][0], uv[i][1]};
            auto shade = [&](double a, double b, double c, LinearPixel& color) {
                const double u = corners[0].U * a + corners[1].U * b + corners[2].U * c;
                const double v = corners[0].V * a + corners[1].V * b + corners[2].V * c;
                return SampleSprite(image, u, v, color);
            };
            // The diagonal uses the same top-left rule as spline triangles, so
            // each source sample is covered once and UVs remain image-global.
            if (!RasterTriangle({corners[0], corners[1], corners[2]}, snapshot, samples, shade)) return false;
            auto shadeOther = [&](double a, double b, double c, LinearPixel& color) {
                const double u = corners[0].U * a + corners[2].U * b + corners[3].U * c;
                const double v = corners[0].V * a + corners[2].V * b + corners[3].V * c;
                return SampleSprite(image, u, v, color);
            };
            return RasterTriangle({corners[0], corners[2], corners[3]}, snapshot, samples, shadeOther);
        }

        uint16_t FloatToHalf(float value) noexcept
        {
            const uint32_t bits = std::bit_cast<uint32_t>(value);
            const uint32_t sign = (bits >> 16) & 0x8000;
            const uint32_t exponent = (bits >> 23) & 0xFF;
            uint32_t mantissa = bits & 0x7FFFFF;
            if (exponent == 0xFF) return uint16_t(sign | 0x7C00 | (mantissa ? 0x0200 : 0));
            int halfExponent = int(exponent) - 127 + 15;
            if (halfExponent >= 31) return uint16_t(sign | 0x7C00);
            if (halfExponent <= 0)
            {
                if (halfExponent < -10) return uint16_t(sign);
                mantissa |= 0x800000;
                const unsigned shift = unsigned(14 - halfExponent);
                uint32_t rounded = mantissa >> shift;
                const uint32_t remainder = mantissa & ((uint32_t(1) << shift) - 1);
                const uint32_t halfway = uint32_t(1) << (shift - 1);
                if (remainder > halfway || (remainder == halfway && (rounded & 1))) ++rounded;
                return uint16_t(sign | rounded);
            }
            uint32_t rounded = mantissa >> 13;
            const uint32_t remainder = mantissa & 0x1FFF;
            if (remainder > 0x1000 || (remainder == 0x1000 && (rounded & 1)))
            {
                ++rounded;
                if (rounded == 0x400)
                {
                    rounded = 0;
                    ++halfExponent;
                    if (halfExponent >= 31) return uint16_t(sign | 0x7C00);
                }
            }
            return uint16_t(sign | (uint32_t(halfExponent) << 10) | rounded);
        }

        void StoreHalf(std::vector<uint8_t>& bytes, size_t pixel, size_t channel, float value)
        {
            const uint16_t half = FloatToHalf(value);
            const size_t offset = pixel * 8 + channel * 2;
            bytes[offset] = uint8_t(half & 0xFF);
            bytes[offset + 1] = uint8_t(half >> 8);
        }

        const SpriteLease* FindSprite(const std::vector<SpriteLease>& sprites, B::MassKey key)
        {
            const auto found = std::lower_bound(sprites.begin(), sprites.end(), key,
                [](const SpriteLease& lease, B::MassKey value) { return lease.Building < value; });
            return found != sprites.end() && found->Building == key ? &*found : nullptr;
        }

        auto DrawKey(const B::BuildingRecord& record)
        {
            const auto& source = *record.Source;
            const std::optional<double> elevation = source.HasSplineSource ?
                (source.SplineSortZValid ? std::optional<double>(source.SplineSortZ) : std::nullopt) :
                std::optional<double>(source.Transform.Translation[2]);
            return std::tuple(source.HasSplineSource ? 0 : 1, elevation,
                source.Transform.Translation[1], source.Transform.Translation[0],
                record.Key.Serial, record.Key.Index);
        }

        bool ValidSnapshot(const B::CellSnapshot& snapshot)
        {
            return snapshot.WorldGeneration && snapshot.Origin.Valid &&
                std::isfinite(snapshot.Origin.X) && std::isfinite(snapshot.Origin.Y);
        }
    }

    State Compose(const B::CellSnapshot& snapshot, const std::vector<SpriteLease>& sprites,
        bool renderDroneRails, CellImage& output) noexcept
    {
        output = {};
        try
        {
            if (!ValidSnapshot(snapshot) || !std::is_sorted(sprites.begin(), sprites.end(),
                [](const SpriteLease& a, const SpriteLease& b) { return a.Building < b.Building; }))
                return State::Failed;
            CellImage result;
            result.Key = snapshot.Key;
            result.WorldGeneration = snapshot.WorldGeneration;
            result.SourceRevision = snapshot.SourceRevision;
            result.CellRevision = snapshot.CellRevision;
            result.DependencyRevision = snapshot.DependencyRevision;
            result.RenderDroneRails = renderDroneRails;
            result.Status = snapshot.HasUnresolvedContributor ? State::Partial : State::Complete;
            const size_t pixelCount = kSide * kSide;
            std::vector<std::array<float, 4>> samples(pixelCount * kSamplesPerPixel);
            for (auto& value : samples) value = {0, 0, 0, 1};

            std::vector<const B::BuildingRecord*> ordinary, splines, baseCore;
            ordinary.reserve(snapshot.Records.size());
            splines.reserve(snapshot.Records.size());
            baseCore.reserve(snapshot.Records.size());
            for (const auto& record : snapshot.Records)
            {
                if (!record || !record->Source) { result.Status = State::Partial; continue; }
                const auto& source = *record->Source;
                if (!source.InteriorKnown) { result.Status = State::Partial; continue; }
                if (source.InteriorOnly) continue;
                if (source.Support != B::SupportState::Supported) result.Status = State::Partial;
                if (source.BaseCoreOverlay) baseCore.push_back(record.get());
                else if (source.HasSplineSource) splines.push_back(record.get());
                else ordinary.push_back(record.get());
            }
            auto byDrawKey = [](const B::BuildingRecord* a, const B::BuildingRecord* b) { return DrawKey(*a) < DrawKey(*b); };
            std::stable_sort(ordinary.begin(), ordinary.end(), byDrawKey);
            std::stable_sort(splines.begin(), splines.end(), byDrawKey);
            std::stable_sort(baseCore.begin(), baseCore.end(), byDrawKey);

            auto drawSprites = [&](const std::vector<const B::BuildingRecord*>& records) {
                for (const auto* record : records)
                {
                    const auto& source = *record->Source;
                    const auto* lease = FindSprite(sprites, record->Key);
                    if (!lease || !lease->Image || lease->SourceRevision != source.SourceRevision ||
                        !source.SpriteQuadValid)
                    { result.Status = State::Partial; continue; }
                    if (!RasterSprite(source, *lease->Image, snapshot, samples))
                    { result.Status = State::Partial; continue; }
                    result.CaptureRevisions.push_back({record->Key, lease->SourceRevision});
                }
            };
            drawSprites(ordinary);

            for (const auto* record : splines)
            {
                const auto& source = *record->Source;
                if (MiniMapBuildingSplinePolicy::IsDroneRailDefinition(
                        true, source.CanonicalBuildingType) && !renderDroneRails) continue;
                if (source.SplinePreparation != B::SplinePreparationState::Complete ||
                    !record->SplineOcclusion ||
                    record->SplineOcclusion->WorldGeneration != snapshot.WorldGeneration ||
                    record->SplineOcclusion->SourceRevision != source.SourceRevision)
                { result.Status = State::Partial; continue; }
                if (record->SplineOcclusion->State != B::SplineOcclusionState::Complete)
                    result.Status = State::Partial;

                std::vector<const B::OccludedSplineSection*> derivedSections;
                derivedSections.reserve(record->SplineOcclusion->Sections.size());
                for (const auto& section : record->SplineOcclusion->Sections) derivedSections.push_back(&section);
                std::sort(derivedSections.begin(), derivedSections.end(),
                    [](const auto* a, const auto* b) { return a->Identity < b->Identity; });

                std::vector<const B::OccludedSplineSection*> orderedSections;
                orderedSections.reserve(source.SplineSections.size());
                for (const auto& sourceSection : source.SplineSections)
                {
                    const auto found = std::lower_bound(derivedSections.begin(), derivedSections.end(), sourceSection.Identity,
                        [](const auto* section, uint64_t identity) { return section->Identity < identity; });
                    if (found == derivedSections.end() || (*found)->Identity != sourceSection.Identity)
                    { result.Status = State::Partial; orderedSections.push_back(nullptr); continue; }
                    orderedSections.push_back(*found);
                }

                const uint32_t rgb = MiniMapBuildingSplinePolicy::RepresentativeRgb(
                    true, source.CanonicalBuildingType);
                for (const auto* derived : orderedSections)
                {
                    if (!derived) continue;
                    if (!derived->Complete) result.Status = State::Partial;
                    for (const auto& triangle : derived->VisibleTriangles)
                        if (!RasterSolid(triangle.Vertices, snapshot, samples, rgb)) result.Status = State::Partial;
                }
                if (MiniMapBuildingSplinePolicy::IsWalkwayDefinition(true, source.CanonicalBuildingType))
                {
                    for (const auto* derived : orderedSections)
                    {
                        if (!derived) continue;
                        for (const auto& triangle : derived->WalkwayInsetTriangles)
                            if (!RasterSolid(triangle.Vertices, snapshot, samples,
                                MiniMapBuildingSplinePolicy::kWalkwayInsetRgb)) result.Status = State::Partial;
                    }
                }
                else
                {
                    for (const auto* derived : orderedSections)
                        if (derived && !derived->WalkwayInsetTriangles.empty()) result.Status = State::Partial;
                }
            }

            // Base Core remains the final sprite pass and is never used as a
            // spline occluder by the source/index contract.
            drawSprites(baseCore);

            result.Rgba16F.resize(pixelCount * 8);
            for (size_t pixel = 0; pixel < pixelCount; ++pixel)
            {
                std::array<float, 4> resolved{};
                for (size_t sample = 0; sample < kSamplesPerPixel; ++sample)
                    for (size_t channel = 0; channel < 4; ++channel)
                        resolved[channel] += samples[pixel * kSamplesPerPixel + sample][channel] /
                            float(kSamplesPerPixel);
                for (size_t channel = 0; channel < 4; ++channel)
                {
                    if (!std::isfinite(resolved[channel]) || resolved[channel] < 0 || resolved[channel] > 1)
                        return State::Failed;
                    StoreHalf(result.Rgba16F, pixel, channel, resolved[channel]);
                }
            }
            output = std::move(result);
            return output.Status;
        }
        catch (...) { output = {}; return State::Failed; }
    }
}

#endif
