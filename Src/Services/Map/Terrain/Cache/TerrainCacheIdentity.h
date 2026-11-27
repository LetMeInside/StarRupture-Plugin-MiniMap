// Defines complete, provider-neutral evidence for one privately persisted terrain page.
#pragma once
#include "../ITerrainSourceRetrieval.h"

namespace Services::Map::Terrain::Cache
{
    /** Value-only page identity; contains no provider capability, native pointer or request lifetime. */
    struct TerrainCacheIdentity final
    {
        static constexpr std::uint32_t TileDimension = 256;
        static constexpr std::uint64_t TileBytes = TileDimension / 4u * (TileDimension / 4u) * 8u;

        TerrainImage::Identity Source;
        std::string Locator;
        std::uint32_t Width = 0;
        std::uint32_t Height = 0;
        ITerrainSourceRetrieval::SourceEncoding Encoding = ITerrainSourceRetrieval::SourceEncoding::Bc1;
        bool SRgb = false;
        TerrainImage::Registration Registration;
        TerrainPageIdentity Page;

        /** Requires a supported square packed-BC1 page and complete finite identity/placement evidence. */
        bool IsValid() const noexcept
        {
            return Source.IsValid() && !Locator.empty() && Registration.IsValid() &&
                Encoding == ITerrainSourceRetrieval::SourceEncoding::Bc1 && Width == Height &&
                (Width == TileDimension || Width == 512 || Width == 1024 || Width == 2048);
        }
    };
}
