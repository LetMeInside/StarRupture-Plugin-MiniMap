// Describes one immutable cooked-cell output image and its independent physical applicability.
// Construction copies metadata only; this provider owns no native, acquisition or presentation resources.
#pragma once

#include "ITerrainDataProvider.h"

namespace Services::Map::Terrain
{
    /** Externally constructed definition provider for one output, potentially selected by several regions.
     * Separate providers may name the same cooked cells while retaining distinct output identities.
     * Callers supply persistent content/configuration revisions and original names, never private instances.
     */
    class TerrainCellImageProvider final : public ITerrainDataProvider
    {
    public:
        /** Copies definition and coverage; throws invalid_argument for invalid metadata.
         * Performs no native access, registration, pixel retrieval or IO.
         */
        TerrainCellImageProvider(const CaptureDefinition& definition, const Coverage& coverage);
        /** Returns copied physical applicability without deriving it from capture or image bounds. */
        Coverage DescribeCoverage() const override;
        /** Returns copied capture metadata for the selected world/region, otherwise unavailable.
         * Neither query position is classified here; physical selection belongs to the service.
         */
        SourceDescription DescribeSource(const SourceQuery& query) override;

    private:
        /** Rejects malformed identities, geometry, dimensions and required streaming inputs. */
        static void ValidateDefinition(const CaptureDefinition& definition);
        /** Requires nonempty distinct textual names; package paths additionally require a rooted path. */
        static bool AreValidNames(const std::vector<std::string>& names, bool packagePaths);
        /** Checks stable nonempty text without embedded NULs; performs no name interning or resolution. */
        static bool IsValidText(const std::string& value);

        const CaptureDefinition definition_;
        const Coverage coverage_;
    };
}
