// Preserves game-facing cache entry points as scalar/diagnostic forwarding to TerrainService.
#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include <cstddef>
#include <cstdint>
#include <vector>
#include <filesystem>
#include "Services/Map/Terrain/TerrainService.h"
#include "TerrainSource.h"

namespace MiniMapTerrainCache
{
    /** Compatibility bridge to the stationary application-owned service; never owns files or cache policy. */
    class TerrainCache final
    {
    public:
        using Service = Services::Map::Terrain::TerrainService;
        /** Binds before callbacks/admission; the service must outlive all bridge calls, including exit clearing. */
        static void BindService(Service& service) noexcept;

    private:
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        /** Call-local adapter for existing timing scopes; the generic cache retains no profiling state. */
        class CacheProfile;
#endif
        friend bool GetCacheRoot(std::filesystem::path&);
        friend bool Clear();
        friend bool TryLoadChunk(const MiniMapTerrainSource::SourceTile&, int, int, uint8_t, bool, int, int, std::vector<uint8_t>&);
        friend bool StoreSourceTile(const MiniMapTerrainSource::SourceTile&, int, int, uint8_t, bool, const uint8_t*, size_t);
        /** Preserves game-facing read diagnostics from neutral service results. */
        static void ReportRead(const Service::CacheResult& result,
            const MiniMapTerrainSource::SourceTile& tile, int chunkX, int chunkY);
        /** Logs validation failures without modifying or deleting invalid cache records. */
        static void ReportReadFailure(Service::CacheStatus status,
            const MiniMapTerrainSource::SourceTile& tile, int chunkX, int chunkY);
        static Service* service_;
    };

    /** Copies the configured service root; performs no IO. */
    bool GetCacheRoot(std::filesystem::path&);
    /** Clears through the service's own subtree exclusion; application decides when invalidation is required. */
    bool Clear();
    /** Retrieves compatible compressed bytes after the caller's existing native readiness checks. */
    bool TryLoadChunk(
        const MiniMapTerrainSource::SourceTile& tile,
        int sourceWidth,
        int sourceHeight,
        uint8_t pixelFormat,
        bool sRGB,
        int chunkX,
        int chunkY,
        std::vector<uint8_t>& compressedChunk);

    /** Attempts compatible persistence without taking ownership of or modifying the source tile. */
    bool StoreSourceTile(
        const MiniMapTerrainSource::SourceTile& tile,
        int sourceWidth,
        int sourceHeight,
        uint8_t pixelFormat,
        bool sRGB,
        const uint8_t* sourceData,
        size_t sourceDataSize);
}

#endif
