// Reuses provider-owned game identity conventions for legacy cache callers, without native-key mapping.
#include "StarRuptureTerrainCacheAssociation.h"
#if defined(MODLOADER_CLIENT_BUILD)
#include "TerrainSource.h"
#include "Map/Map.h"
#include "SDK/Engine_classes.hpp"

namespace MiniMapTerrainSource
{
    /** Rejects unavailable or registered sources; the generic service receives only copied opaque evidence. */
    std::optional<Services::Map::Terrain::Cache::TerrainCacheIdentity>
        StarRuptureTerrainCacheAssociation::DescribeSource(const SourceTile& tile,
            int width, int height, std::uint8_t pixelFormat, bool sRgb)
    {
        if (tile.RegisteredImage || !tile.Texture || width <= 0 || height <= 0 || pixelFormat != 5) return {};
        const auto* world = MiniMapMap::GetWorld();
        if (!world) return {};
        StarRuptureTerrainProvider::Configuration configuration{world->GetName(),
            MiniMapMap::GetWorldGeneration(), {}};
        if (!StarRuptureTerrainProvider::IsCurrentSession(configuration)) return {};
        configuration.ContentRevision = SDK::UKismetSystemLibrary::GetBuildVersion().ToString();
        if (configuration.ContentRevision.empty()) return {};
        auto source = StarRuptureTerrainProvider::GetIdentity(configuration, tile);
        Services::Map::Terrain::Cache::TerrainCacheIdentity identity{source, source.SourceId,
            static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
            Services::Map::Terrain::ITerrainSourceRetrieval::SourceEncoding::Bc1, sRgb,
            StarRuptureTerrainProvider::GetRegistration(tile),
            StarRuptureTerrainProvider::GetPageIdentity(configuration, tile)};
        if (!identity.IsValid()) return {};
        return identity;
    }
}
#endif
