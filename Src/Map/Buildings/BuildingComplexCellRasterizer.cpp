#include "BuildingComplexCellRasterizer.h"

#if defined(MODLOADER_CLIENT_BUILD)

#include "BuildingCache.h"
#include "BuildingSplinePolicy.h"
#include "BuildingImageRasterizationCore.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <tuple>
#include <utility>

namespace MiniMapBuildingComplexCellRasterizer
{
    namespace
    {
        namespace Core = MiniMapBuildingImageRasterizationCore;
        constexpr size_t kSide = B::kCellImageSizePixels;
        constexpr size_t kSamplesPerPixel = Core::kSamplesPerPixel;
        constexpr double kPixelWorldSize = B::kCellWorldSizeCm / double(kSide);
        using LinearPixel = Core::Pixel;
        using Vertex = Core::Vertex;
        using Samples = Core::SampleBuffer;

        Core::Grid RasterGrid(const B::CellSnapshot& snapshot)
        { return {snapshot.Origin.X, snapshot.Origin.Y, snapshot.Key.X, snapshot.Key.Y, uint32_t(kSide), uint32_t(kSide), kPixelWorldSize}; }

        template<class Shade>
        bool RasterTriangle(std::array<Vertex, 3> vertices, const B::CellSnapshot& snapshot,
            Samples& samples, Shade&& shade) noexcept
        { return Core::RasterTriangle(vertices, RasterGrid(snapshot), samples, std::forward<Shade>(shade)); }

        bool RasterSprite(const B::SourcePayload& source, const MiniMapBuildingCache::Sprite& image,
            const B::CellSnapshot& snapshot, Samples& samples) noexcept
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
                return Core::SampleRgba8(image.Rgba.data(), image.Rgba.size(), image.Width, image.Height, {0, 0, double(image.Width), double(image.Height)}, u, v, color);
            };
            if (!RasterTriangle({corners[0], corners[1], corners[2]}, snapshot, samples, shade)) return false;
            auto shadeOther = [&](double a, double b, double c, LinearPixel& color) {
                const double u = corners[0].U * a + corners[2].U * b + corners[3].U * c;
                const double v = corners[0].V * a + corners[2].V * b + corners[3].V * c;
                return Core::SampleRgba8(image.Rgba.data(), image.Rgba.size(), image.Width, image.Height, {0, 0, double(image.Width), double(image.Height)}, u, v, color);
            };
            return RasterTriangle({corners[0], corners[2], corners[3]}, snapshot, samples, shadeOther);
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
            result.Validity = snapshot.Validity;
            result.Origin = snapshot.Origin;
            result.RenderDroneRails = renderDroneRails;
            result.Status = snapshot.HasUnresolvedContributor ? State::Partial : State::Complete;
            const size_t pixelCount = kSide * kSide;
            Samples samples(pixelCount * kSamplesPerPixel);
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
                    if (!lease || !lease->Image || !lease->ImageRevision || lease->SourceRevision != source.SourceRevision ||
                        !source.SpriteQuadValid)
                    { result.Status = State::Partial; continue; }
                    if (!RasterSprite(source, *lease->Image, snapshot, samples))
                    { result.Status = State::Partial; continue; }
                    result.CaptureRevisions.push_back({record->Key, lease->SourceRevision, lease->ImageRevision});
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
                        if (!Core::RasterSolidTriangle(triangle.Vertices, RasterGrid(snapshot), samples, rgb)) result.Status = State::Partial;
                }
                if (MiniMapBuildingSplinePolicy::IsWalkwayDefinition(true, source.CanonicalBuildingType))
                {
                    for (const auto* derived : orderedSections)
                    {
                        if (!derived) continue;
                        for (const auto& triangle : derived->WalkwayInsetTriangles)
                            if (!Core::RasterSolidTriangle(triangle.Vertices, RasterGrid(snapshot), samples,
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

            if (!Core::Resolve(samples, uint32_t(kSide), uint32_t(kSide), result.Rgba16F)) return State::Failed;
            output = std::move(result);
            return output.Status;
        }
        catch (...) { output = {}; return State::Failed; }
    }
}

#endif
