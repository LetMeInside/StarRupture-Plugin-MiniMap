// Supplies complete source evidence for dormant legacy cache forwarding; owns no files or cache policy.
#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "StarRuptureTerrainProvider.h"
#include "Services/Map/Terrain/Cache/TerrainCacheIdentity.h"
#include <optional>

namespace MiniMapTerrainSource
{
    /** Application-only bridge sharing native source identity/placement with the active provider. */
    class StarRuptureTerrainCacheAssociation final
    {
    public:
        /** Copies native identity and inspected mip layout for legacy forwarding; no cache IO or retrieval.
         * Requires the admitted game thread and a current native source after existing readiness checks.
         * Registered/FE sources remain on their existing nonpersistent route.
         */
        static std::optional<Services::Map::Terrain::Cache::TerrainCacheIdentity> DescribeSource(
            const SourceTile& tile, int width, int height, std::uint8_t pixelFormat, bool sRgb);
    };
}
#endif
