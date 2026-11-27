// Implements only native cache-key translation; actual lookup/persistence stays in TerrainService.
#include "StarRuptureTerrainCacheAssociation.h"
#if defined(MODLOADER_CLIENT_BUILD)
#include "TerrainSource.h"
#include <stdexcept>
#include <utility>

namespace MiniMapTerrainSource
{
    /** Captures strong configuration/capability ownership without a borrowed callback context. */
    Services::Map::Terrain::TerrainService::NativeCacheMapping
        StarRuptureTerrainCacheAssociation::CreateMapping(std::shared_ptr<StarRuptureTerrainProvider> provider)
    {
        if (!provider) throw std::invalid_argument("Native terrain cache mapping requires a provider.");
        return [provider = std::move(provider)](const auto& query, const auto& description, const auto& inspection)
        {
            return MapKey(*provider, query, description, inspection);
        };
    }

    /** The provider's native BC1 inspection enforces format 5; no game enum enters the generic service. */
    std::optional<Services::Map::Terrain::TerrainService::NativeCacheKey>
        StarRuptureTerrainCacheAssociation::MapKey(const StarRuptureTerrainProvider& provider,
            const Services::Map::Terrain::ITerrainDataProvider::SourceQuery& query,
            const Services::Map::Terrain::ITerrainDataProvider::SourceDescription& description,
            const Services::Map::Terrain::ITerrainSourceRetrieval::Inspection& inspection)
    {
        using Retrieval = Services::Map::Terrain::ITerrainSourceRetrieval;
        if (query.WorldId != provider.configuration_.WorldId || !query.RegionId.empty() ||
            inspection.State != Retrieval::Status::Available || inspection.Encoding != Retrieval::SourceEncoding::Bc1)
            throw std::invalid_argument("Native cache mapping requires current inspected BC1 source evidence.");
        SourceTile tile;
        if (!StarRuptureTerrainProvider::ResolveSource(provider.configuration_, query.SamplePosition, tile) ||
            description.Source != inspection.Source ||
            StarRuptureTerrainProvider::GetIdentity(provider.configuration_, tile) != inspection.Source)
            throw std::runtime_error("Native terrain source changed before cache association.");
        return Services::Map::Terrain::TerrainService::NativeCacheKey{
            tile.GridX, tile.GridY, tile.RadiationLevel, static_cast<std::int32_t>(inspection.WidthPixels),
            static_cast<std::int32_t>(inspection.HeightPixels), 5, inspection.SRgb};
    }
}
#endif
