// Validates and copies cooked-cell capture definitions without executing acquisition or streaming.
// Output identity, physical coverage, residency demand and image registration remain separate values.
#include "TerrainCellImageProvider.h"
#include <stdexcept>

namespace Services::Map::Terrain
{
    /** Retains private immutable copies; construction has no external side effects. */
    TerrainCellImageProvider::TerrainCellImageProvider(
        const CaptureDefinition& definition, const Coverage& coverage)
        : definition_(definition)
        , coverage_(coverage)
    {
        ValidateDefinition(definition_);
        if (!coverage_.IsValid())
            throw std::invalid_argument("Cell image provider requires valid independent coverage.");
    }

    /** Rejects absent text and embedded terminators without interpreting application identities. */
    bool TerrainCellImageProvider::IsValidText(const std::string& value)
    {
        return !value.empty() && value.find('\0') == std::string::npos;
    }

    /** Rejects malformed or repeated entries; an empty explicit activated-layer list is permitted. */
    bool TerrainCellImageProvider::AreValidNames(const std::vector<std::string>& names, bool packagePaths)
    {
        for (std::size_t index = 0; index < names.size(); ++index)
        {
            const auto& name = names[index];
            if (!IsValidText(name) || (packagePaths && (name.front() != '/' || name.size() < 2)))
                return false;
            for (std::size_t earlier = 0; earlier < index; ++earlier)
            {
                if (names[earlier] == name)
                    return false;
            }
        }
        return true;
    }

    /** Validates value geometry only; native availability, readiness and bounded-Z rendering remain unproven. */
    void TerrainCellImageProvider::ValidateDefinition(const CaptureDefinition& definition)
    {
        if (!definition.Source.IsValid() || !IsValidText(definition.Source.SourceId) ||
            !IsValidText(definition.Source.ContentRevision) || !IsValidText(definition.Page.CollectionKey))
            throw std::invalid_argument("Cell image definition requires stable output, revision and page identities.");
        if (!AreValidNames({definition.OriginalWorldPackage}, true) ||
            definition.CookedCellPackages.empty() || !AreValidNames(definition.CookedCellPackages, true) ||
            definition.TargetGrids.empty() || !AreValidNames(definition.TargetGrids, false) ||
            !AreValidNames(definition.ActivatedDataLayerAssets, true))
            throw std::invalid_argument("Cell image definition requires original cooked packages and regional inputs.");
        if (!definition.StreamingBounds.IsValid() || !definition.CaptureBounds.IsValid() ||
            !definition.ImageRegistration.IsValid() ||
            !TerrainImage::IsValidSourceDimensions(definition.WidthPixels, definition.HeightPixels))
            throw std::invalid_argument("Cell image definition requires finite volumes, affine registration and dimensions.");
    }

    /** Returns physical coverage unchanged, independently of capture and registration geometry. */
    ITerrainDataProvider::Coverage TerrainCellImageProvider::DescribeCoverage() const
    {
        return coverage_;
    }

    /** Maps only service-selected world/region identity; sampling never changes this output's identity. */
    ITerrainDataProvider::SourceDescription TerrainCellImageProvider::DescribeSource(const SourceQuery& query)
    {
        if (query.WorldId != coverage_.WorldId)
            return {};
        if (coverage_.Role == CoverageRole::Default)
        {
            if (!query.RegionId.empty())
                return {};
        }
        else
        {
            for (const auto& region : coverage_.Regions)
            {
                if (region.Id == query.RegionId)
                    return {definition_.Source, definition_, definition_.Page};
            }
            return {};
        }
        return {definition_.Source, definition_, definition_.Page};
    }
}
