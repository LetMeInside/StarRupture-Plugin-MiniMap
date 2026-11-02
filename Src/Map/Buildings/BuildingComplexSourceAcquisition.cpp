#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingComplexSourceAcquisition.h"
#include "BuildingCollector.h"
#include "BuildingSplineGeometry.h"
#include "BuildingSplineEnvelope.h"
#include "BuildingSplineOcclusionAdapter.h"
#include "BuildingVisuals.h"
#include "BuildingSplinePolicy.h"
#include "Map/MapTransform.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace
{
    namespace B = MiniMapBuildingComplexIndex;
    namespace C = MiniMapBuildingCollector;
    namespace S = MiniMapBuildingSplineGeometry;
    namespace E = MiniMapBuildingSplineEnvelope;
    namespace A = MiniMapBuildingSplineOcclusionAdapter;
    namespace O = MiniMapBuildingSplineOcclusion;

    bool PrepareSplineGeometryOwned(B::MassKey key, const C::Record& record, B::SourcePayload& source)
    {
        source.SplineSections.clear();
        source.PreparedSplineSupport = {};
        source.HasSplineSource = record.HasSpline;
        source.CanonicalBuildingType = record.BuildingId;
        source.BlueprintIdentity = record.CanonicalBlueprint;
        source.InteriorKnown = record.InteriorKnown;
        source.InteriorOnly = record.InteriorOnly;
        source.Transform = {record.Position, record.Scale, record.Rotation};
        source.SplineCurveCount = record.Curve ? record.Curve->Segments.size() : 0;
        source.SplineProfileCount = record.DefinitionShape && record.DefinitionShape->Profiles ?
            record.DefinitionShape->Profiles->Profiles.size() : 0;
        source.SplinePreparation = source.HasSplineSource ? B::SplinePreparationState::Invalid :
            B::SplinePreparationState::NotPresent;
        source.SplineCategory = uint8_t(MiniMapBuildingSplinePolicy::Classify(record.DefinitionValid, source.CanonicalBuildingType));
        source.SplineSortZValid = false;
        if (source.InteriorKnown && source.InteriorOnly)
        {
            // The collector omits InteriorOnly records before visual planning.
            source.SplinePreparation = B::SplinePreparationState::NotPresent;
            return true;
        }
        if (!source.HasSplineSource) return true;
        if (!record.Curve || record.Curve->Segments.empty() || key.Index < 0 || key.Serial <= 0 || !source.SourceGeneration ||
            !MiniMapBuildingGeometryTypes::Valid(source.Transform)) return true;

        if (!record.Curve->Segments.empty())
        {
            const double startZ = record.Curve->Segments.front().Controls[0][2];
            const double endZ = record.Curve->Segments.back().Controls[3][2];
            if (std::isfinite(startZ) && std::isfinite(endZ))
            {
                source.SplineSortZ = (std::max)(startZ, endZ);
                source.SplineSortZValid = true;
            }
        }

        S::SamplingPolicy policy;
        policy.PixelsPerUnit = 1.0 / (100.0 * MiniMapMap::MinimumMetersPerPixel);
        auto sampled = std::make_shared<const S::PreparedCurve>(S::Prepare(*record.Curve, policy));
        if (sampled->SourceIssue != C::GeometryIssue::None ||
            sampled->PreparationIssue != C::GeometryIssue::None ||
            sampled->Sections.size() != record.Curve->Segments.size())
        {
            source.SplinePreparation = B::SplinePreparationState::Invalid;
            return true;
        }
        const bool thin = MiniMapBuildingSplinePolicy::IsDroneRailDefinition(true, source.CanonicalBuildingType);
        const auto envelope = E::PrepareOwned(record, sampled, {}, thin);
        const auto* selected = envelope ? MiniMapBuildingVisuals::Detail::SelectSpline(record, *envelope) : nullptr;
        if (!selected)
        {
            source.SplinePreparation = B::SplinePreparationState::Unsupported;
            return true;
        }

        source.SplineSections.reserve(selected->Sections.size());
        bool complete = true;
        size_t expectedSections = 0;
        for (size_t segmentIndex = 0; segmentIndex < sampled->Sections.size(); ++segmentIndex)
        {
            const auto& sampledSegment = sampled->Sections[segmentIndex];
            const auto& sourceSegment = record.Curve->Segments[segmentIndex];
            if (sampledSegment.Discontinuous || sourceSegment.Mode == S::Constant) continue;
            if (sampledSegment.Count < 2) { complete = false; continue; }
            expectedSections += sampledSegment.Count - 1;
        }
        constexpr uint32_t representationOnlyReasons = E::NativeSectionsUnknown | E::JoinsAndCapsUnknown |
            E::RibbonApproximation | E::Discontinuity | E::SamplingLimited;
        if (selected->Reasons & ~representationOnlyReasons) complete = false;

        for (size_t index = 0; index < selected->Sections.size(); ++index)
        {
            const auto& section = selected->Sections[index];
            O::Input input;
            if (!A::SectionInput(*envelope, *selected, index, input))
            {
                complete = false;
                continue;
            }
            if (source.SplineSections.size() == (std::numeric_limits<uint64_t>::max)())
                return false;
            B::SplineSectionSource prepared;
            prepared.Identity = uint64_t(source.SplineSections.size());
            prepared.SourceSegment = section.SourceSegment;
            prepared.SyntheticThin = thin;
            prepared.ProfileIndex = thin ? (std::numeric_limits<uint64_t>::max)() : selected->SourceProfile;
            prepared.StartT = section.StartT;
            prepared.EndT = section.EndT;
            const auto& samples = sampled->Sections[section.SourceSegment];
            const auto begin = sampled->Samples.begin() + samples.First;
            const auto sample = std::lower_bound(begin, begin + samples.Count, section.StartT,
                [](const auto& value, double t) { return value.T < t; });
            if (sample == begin + samples.Count || sample->T != section.StartT)
            {
                complete = false;
                continue;
            }
            prepared.SampleOrdinal = uint64_t(sample - begin);
            prepared.NumericalInputs = {section.StartT, section.EndT};
            bool trianglesValid = true;
            for (const auto& triangle : input.Triangles)
            {
                B::SplineTriangleSource copied;
                for (size_t vertex = 0; vertex < 3; ++vertex)
                {
                    copied.Vertices[vertex] = triangle.Vertices[vertex];
                    const auto& point = triangle.Vertices[vertex];
                    if (!std::isfinite(point[0]) || !std::isfinite(point[1]) || !std::isfinite(point[2]))
                    { trianglesValid = false; break; }
                    if (!prepared.Bounds.Valid)
                    {
                        prepared.Bounds = {point[0], point[1], point[0], point[1], true};
                    }
                    else
                    {
                        prepared.Bounds.MinX = (std::min)(prepared.Bounds.MinX, point[0]);
                        prepared.Bounds.MinY = (std::min)(prepared.Bounds.MinY, point[1]);
                        prepared.Bounds.MaxX = (std::max)(prepared.Bounds.MaxX, point[0]);
                        prepared.Bounds.MaxY = (std::max)(prepared.Bounds.MaxY, point[1]);
                    }
                }
                if (!trianglesValid) break;
                prepared.Triangles.push_back(copied);
            }
            if (!trianglesValid || !prepared.Bounds.Valid || prepared.Triangles.empty())
            {
                complete = false;
                continue;
            }
            if (MiniMapBuildingSplinePolicy::IsWalkwayDefinition(true, source.CanonicalBuildingType))
            {
                std::array<O::XY, 4> left{}, right{};
                if (A::WalkwayInsetBands(section,
                    MiniMapBuildingSplinePolicy::kWalkwayInsetFractionPerSide, left, right))
                {
                    prepared.WalkwayInsetBands[0].Vertices = left;
                    prepared.WalkwayInsetBands[1].Vertices = right;
                    prepared.HasWalkwayInsetBands = true;
                }
                else complete = false;
            }
            if (!source.PreparedSplineSupport.Valid)
                source.PreparedSplineSupport = prepared.Bounds;
            else
            {
                source.PreparedSplineSupport.MinX = (std::min)(source.PreparedSplineSupport.MinX, prepared.Bounds.MinX);
                source.PreparedSplineSupport.MinY = (std::min)(source.PreparedSplineSupport.MinY, prepared.Bounds.MinY);
                source.PreparedSplineSupport.MaxX = (std::max)(source.PreparedSplineSupport.MaxX, prepared.Bounds.MaxX);
                source.PreparedSplineSupport.MaxY = (std::max)(source.PreparedSplineSupport.MaxY, prepared.Bounds.MaxY);
            }
            source.SplineSections.push_back(std::move(prepared));
        }
        if (source.SplineSections.size() != expectedSections) complete = false;
        source.SplinePreparation = source.SplineSections.empty() ? B::SplinePreparationState::Unsupported :
            complete ? B::SplinePreparationState::Complete : B::SplinePreparationState::Partial;
        return true;
    }
}

namespace MiniMapBuildingComplexSourceAcquisition
{
    bool PrepareSplineGeometry(B::MassKey key, const C::Record& record, B::SourcePayload& source) noexcept
    {
        try { return PrepareSplineGeometryOwned(key, record, source); }
        catch (...) { return false; }
    }
}
#endif
