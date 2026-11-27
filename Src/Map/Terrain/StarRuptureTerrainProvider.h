// Adapts native StarRupture terrain to the generic M1 source and ingestion contracts.
// Owns copied configuration only; source capabilities retain no world, texture, cache or image resources.
#pragma once
#if defined(MODLOADER_CLIENT_BUILD)

#include "Services/Map/Terrain/ITerrainDataProvider.h"

namespace MiniMapTerrainSource
{
    struct SourceTile;
    class StarRuptureTerrainCacheAssociation;

    /** Externally constructed native default provider. Construction is inert and never registers itself.
     * DescribeSource and all returned retrieval operations require the game thread, outside rendering,
     * while existing map/native lifecycle admission is open. Application replaces this provider per
     * MiniMapMap world generation; old shared capabilities reject the replacement session.
     */
    class StarRuptureTerrainProvider final : public Services::Map::Terrain::ITerrainDataProvider
    {
    public:
        /** Immutable application identity, supplied without reading world/cache state in construction.
         * WorldId is the current native world's SDK name; WorldGeneration is MiniMapMap's existing
         * generation. ContentRevision must identify the authoritative native asset build/content,
         * changing when those bytes change; a session counter is not a persistent content revision.
         */
        struct Configuration
        {
            std::string WorldId;
            std::uint64_t WorldGeneration = 0;
            std::string ContentRevision;
        };

        /** Copies/validates immutable identity; throws invalid_argument for missing identity values.
         * Performs no native access, acquisition, file IO, callbacks, registration or GPU operations.
         */
        explicit StarRuptureTerrainProvider(Configuration configuration);
        /** Returns one default with no finite selection boxes and no Forgotten Engine policy. */
        Coverage DescribeCoverage() const override;
        /** Resolves only SamplePosition through native terrain, independently of physical selection.
         * Returns unavailable for stale session, sparse-grid holes or unsupported native placement.
         * Successful descriptions strongly own a source-specific, pointer-free retrieval capability.
         */
        SourceDescription DescribeSource(const SourceQuery& query) override;

    private:
        friend class StarRuptureTerrainCacheAssociation;
        /** Shared source-specific retrieval implementation; independent of the provider object's lifetime. */
        class SourceRetrieval;
        /** Checks the existing game thread, map admission, native world name and world-generation identity. */
        static bool IsCurrentSession(const Configuration& configuration);
        /** Resolves a finite sample through the existing native resolver and rejects reentrant session changes. */
        static bool ResolveSource(const Configuration& configuration, Position sample, SourceTile& tile);
        /** Copies native origin/axis placement, rejecting dimensions incompatible with the active 125 m grid. */
        static Services::Map::Terrain::TerrainImage::Registration GetRegistration(const SourceTile& tile);
        /** Combines existing world/grid, radiation/variant and asset identity with application content revision. */
        static Services::Map::Terrain::TerrainImage::Identity GetIdentity(
            const Configuration& configuration, const SourceTile& tile);

        const Configuration configuration_;
    };
}
#endif
