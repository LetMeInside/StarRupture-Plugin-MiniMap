// Translates game metadata and diagnostics into TerrainService cache calls; performs no cache IO.
#if defined(MODLOADER_CLIENT_BUILD)
#include "TerrainCache.h"
#include "TerrainProfile.h"
#include "plugin_helpers.h"
#include <array>
#include <optional>

namespace MiniMapTerrainCache
{
    // The application binds its stationary plugin-lifetime service before cache admission.
    Services::Map::Terrain::TerrainService* TerrainCache::service_ = nullptr;

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
    /** Adapts synchronous neutral phase notifications to existing thread-local diagnostic scopes. */
    class TerrainCache::CacheProfile final
    {
    public:
        /** Supplies call-local observer state; no callback is retained after the synchronous cache call. */
        Service::NativeCacheTrace GetTrace() noexcept { return {this, &Notify}; }
        /** Reports admission so closed terrain-cache attempts do not change hit/miss/failure counters. */
        bool WasAdmitted() const noexcept { return scopes_[0].has_value() || scopes_[1].has_value(); }

    private:
        /** Starts/ends the original scopes; root timing includes application result reporting. */
        static void Notify(void* context, Service::NativeCacheActivity activity, bool entering) noexcept
        {
            auto& profile = *static_cast<CacheProfile*>(context);
            const auto index = static_cast<std::size_t>(activity);
            if (entering) profile.scopes_[index].emplace(Phases[index]);
            else if (index > 1) profile.scopes_[index].reset();
        }

        static constexpr std::array<MiniMapTerrainProfile::Phase, 6> Phases{
            MiniMapTerrainProfile::Phase::Cache, MiniMapTerrainProfile::Phase::CacheWrite,
            MiniMapTerrainProfile::Phase::CachePath, MiniMapTerrainProfile::Phase::CacheHeader,
            MiniMapTerrainProfile::Phase::CacheRead, MiniMapTerrainProfile::Phase::StoreExtraction};
        std::array<std::optional<MiniMapTerrainProfile::Scope>, 6> scopes_;
    };
#endif

    /** Associates the plugin-lifetime owner at quiescent startup; never takes ownership or creates providers. */
    void TerrainCache::BindService(Services::Map::Terrain::TerrainService& service) noexcept
    {
        service_ = &service;
    }

    /** Translates game-specific fields to neutral, ordered compatibility metadata. */
    TerrainCache::Service::NativeCacheKey TerrainCache::MakeKey(int radiationLevel, int gridX, int gridY,
        int sourceWidth, int sourceHeight, uint8_t pixelFormat, bool sRGB)
    {
        return {gridX, gridY, radiationLevel, sourceWidth, sourceHeight, pixelFormat, sRGB};
    }

    /** Resolves only the configured root; directory creation belongs exclusively to the service. */
    bool GetCacheRoot(std::filesystem::path& root)
    {
        return TerrainCache::service_ && TerrainCache::service_->GetNativeCacheRoot(root);
    }

    /** Forwards clearing under service-owned exclusion without acquiring application root-cache locks. */
    bool Clear()
    {
        const bool cleared = TerrainCache::service_ && TerrainCache::service_->ClearNativeCache();
        if (!cleared) LOG_ERROR("MiniMap: TerrainCache: failed to clear terrain cache");
        return cleared;
    }

    /** Retains application diagnostics and existing entry-point sequencing around service-owned reads. */
    bool TryLoadChunk(int radiationLevel, int gridX, int gridY, int sourceWidth, int sourceHeight,
        uint8_t pixelFormat, bool sRGB, int chunkX, int chunkY, std::vector<uint8_t>& compressedChunk)
    {
        if (!TerrainCache::service_) return false;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        TerrainCache::CacheProfile profile;
        const auto trace = profile.GetTrace();
        const auto* observer = &trace;
#else
        const TerrainCache::Service::NativeCacheTrace* observer = nullptr;
#endif
        const auto key = TerrainCache::MakeKey(radiationLevel, gridX, gridY,
            sourceWidth, sourceHeight, pixelFormat, sRGB);
        const auto result = TerrainCache::service_->TryLoadNativeChunk(key, chunkX, chunkY, compressedChunk, observer);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        if (profile.WasAdmitted())
#endif
        TerrainCache::ReportRead(result, key, chunkX, chunkY);
        return result.Status == TerrainCache::Service::NativeCacheStatus::Hit;
    }

    /** Forwards a complete source tile; failure remains independent of usable native terrain data. */
    bool StoreSourceTile(int radiationLevel, int gridX, int gridY, int sourceWidth, int sourceHeight,
        uint8_t pixelFormat, bool sRGB, const uint8_t* sourceData, size_t sourceDataSize)
    {
        if (!TerrainCache::service_) return false;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        TerrainCache::CacheProfile profile;
        const auto trace = profile.GetTrace();
        const auto* observer = &trace;
#else
        const TerrainCache::Service::NativeCacheTrace* observer = nullptr;
#endif
        const auto key = TerrainCache::MakeKey(radiationLevel, gridX, gridY,
            sourceWidth, sourceHeight, pixelFormat, sRGB);
        const auto result = TerrainCache::service_->StoreNativeSourceTile(key, sourceData, sourceDataSize, observer);
        if (result.Status == TerrainCache::Service::NativeCacheStatus::DirectoryFailure)
        {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
#endif
            LOG_ERROR("MiniMap: TerrainCache: failed to create cache directory");
        }
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        if (result.Status == TerrainCache::Service::NativeCacheStatus::Stored)
        {
            MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
            LOG_INFO("MiniMap: TerrainCache: stored R%d grid=(%d, %d) chunks=%u bytes=%llu",
                radiationLevel, gridX, gridY, 64u, 2097212ull);
        }
#endif
        return result.Status == TerrainCache::Service::NativeCacheStatus::Stored;
    }

    /** Maps service status to the preexisting game-specific messages and hit/miss counters. */
    void TerrainCache::ReportRead(const Service::NativeCacheResult& result,
        const Service::NativeCacheKey& key, int chunkX, int chunkY)
    {
        using Status = Service::NativeCacheStatus;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        MiniMapTerrainProfile::CacheResult counter;
        if (result.Status == Status::Hit) counter.Result = MiniMapTerrainProfile::Counter::Hit;
        if (result.Status == Status::Miss) counter.Result = MiniMapTerrainProfile::Counter::Miss;
#endif
        if (result.Status == Status::Unavailable || result.Status == Status::InvalidInput) return;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
#endif
        switch (result.Status)
        {
        case Status::Miss:
        case Status::ExistenceFailure:
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            LOG_INFO("MiniMap: TerrainCache: MISS R%d grid=(%d, %d)",
                key.SourceState, key.TileIndex0, key.TileIndex1);
#else
            if (result.Error) LOG_WARN(
                "MiniMap: TerrainCache: cache existence query failed for R%d grid=(%d, %d) error=%d category=%s",
                key.SourceState, key.TileIndex0, key.TileIndex1,
                result.Error.value(), result.Error.category().name());
#endif
            break;
        case Status::Hit:
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            LOG_INFO("MiniMap: TerrainCache: HIT R%d grid=(%d, %d) chunk=(%d, %d) bytes=%u",
                key.SourceState, key.TileIndex0, key.TileIndex1, chunkX, chunkY, 32768u);
#endif
            break;
        default:
            ReportReadFailure(result.Status, key, chunkX, chunkY);
            break;
        }
    }

    /** Keeps failure messages outside the generic service and leaves invalid/missing data as a false return. */
    void TerrainCache::ReportReadFailure(Service::NativeCacheStatus status,
        const Service::NativeCacheKey& key, int chunkX, int chunkY)
    {
        using Status = Service::NativeCacheStatus;
        switch (status)
        {
        case Status::DirectoryFailure:
            LOG_ERROR("MiniMap: TerrainCache: failed to create cache directory");
            break;
        case Status::InvalidFileSize:
            LOG_WARN("MiniMap: TerrainCache: invalid cache file size for R%d grid=(%d, %d)",
                key.SourceState, key.TileIndex0, key.TileIndex1);
            break;
        case Status::OpenFailure:
            LOG_WARN("MiniMap: TerrainCache: failed to open cache file for R%d grid=(%d, %d)",
                key.SourceState, key.TileIndex0, key.TileIndex1);
            break;
        case Status::HeaderMismatch:
            LOG_WARN("MiniMap: TerrainCache: cache header mismatch for R%d grid=(%d, %d)",
                key.SourceState, key.TileIndex0, key.TileIndex1);
            break;
        case Status::ReadFailure:
            LOG_WARN("MiniMap: TerrainCache: failed to read chunk=(%d, %d) for R%d grid=(%d, %d)",
                chunkX, chunkY, key.SourceState, key.TileIndex0, key.TileIndex1);
            break;
        default:
            break;
        }
    }
}
#endif
