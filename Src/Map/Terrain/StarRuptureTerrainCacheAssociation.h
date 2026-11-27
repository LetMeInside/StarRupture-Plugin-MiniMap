// Supplies game-specific scalar translation to the existing generic service-owned cache workflow.
// Never reads/writes cache files, chooses cache policy or takes ownership of terrain resources.
#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "StarRuptureTerrainProvider.h"
#include "Services/Map/Terrain/TerrainService.h"

namespace MiniMapTerrainSource
{
    /** Application-side mapping for a strongly owned native provider. Its callback only revalidates
     * source identity and translates copied scalar metadata; the service schedules every cache operation.
     */
    class StarRuptureTerrainCacheAssociation final
    {
    public:
        /** Returns a callback retaining provider ownership; throws invalid_argument for a null provider.
         * Construction performs no native access or registration. Invocation requires the same admitted
         * game-thread context as provider inspection, and occurs only after successful inspection.
         */
        static Services::Map::Terrain::TerrainService::NativeCacheMapping CreateMapping(
            std::shared_ptr<StarRuptureTerrainProvider> provider);
    private:
        /** Re-resolves copied source evidence through M4a.1, then maps grid/state and inspected layout.
         * Source changes throw a visible mapping failure rather than silently disabling compatible caching.
         */
        static std::optional<Services::Map::Terrain::TerrainService::NativeCacheKey> MapKey(
            const StarRuptureTerrainProvider& provider,
            const Services::Map::Terrain::ITerrainDataProvider::SourceQuery& query,
            const Services::Map::Terrain::ITerrainDataProvider::SourceDescription& description,
            const Services::Map::Terrain::ITerrainSourceRetrieval::Inspection& inspection);
    };
}
#endif
