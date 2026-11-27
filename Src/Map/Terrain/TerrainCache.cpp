// Translates game metadata and diagnostics into TerrainService cache calls; performs no cache IO.
#if defined(MODLOADER_CLIENT_BUILD)
#include "TerrainCache.h"
#include "StarRuptureTerrainCacheAssociation.h"
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
        Service::CacheTrace GetTrace() noexcept { return {this, &Notify}; }
        /** Reports admission so closed terrain-cache attempts do not change hit/miss/failure counters. */
        bool WasAdmitted() const noexcept { return scopes_[0].has_value() || scopes_[1].has_value(); }

    private:
        /** Starts/ends the original scopes; root timing includes application result reporting. */
        static void Notify(void* context, Service::CacheActivity activity, bool entering) noexcept
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

    /** Resolves only the configured root; directory creation belongs exclusively to the service. */
    bool GetCacheRoot(std::filesystem::path& root)
    {
        return TerrainCache::service_ && TerrainCache::service_->GetTerrainCacheRoot(root);
    }

    /** Forwards clearing under service-owned exclusion without acquiring application root-cache locks. */
    bool Clear()
    {
        const bool cleared = TerrainCache::service_ && TerrainCache::service_->ClearTerrainCache();
        if (!cleared) LOG_ERROR("MiniMap: TerrainCache: failed to clear terrain cache");
        return cleared;
    }

    /** Retains application diagnostics and existing entry-point sequencing around service-owned reads. */
    bool TryLoadChunk(const MiniMapTerrainSource::SourceTile& tile, int sourceWidth, int sourceHeight,
        uint8_t pixelFormat, bool sRGB, int chunkX, int chunkY, std::vector<uint8_t>& compressedChunk)
    {
        if (!TerrainCache::service_) return false;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        TerrainCache::CacheProfile profile;
        const auto trace = profile.GetTrace();
        const auto* observer = &trace;
#else
        const TerrainCache::Service::CacheTrace* observer = nullptr;
#endif
        const auto key = MiniMapTerrainSource::StarRuptureTerrainCacheAssociation::DescribeSource(
            tile, sourceWidth, sourceHeight, pixelFormat, sRGB);
        if (!key) return false;
        const auto result = TerrainCache::service_->TryLoadCacheTile(*key, chunkX, chunkY, compressedChunk, observer);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        if (profile.WasAdmitted())
#endif
        TerrainCache::ReportRead(result, tile, chunkX, chunkY);
        return result.Status == TerrainCache::Service::CacheStatus::Hit;
    }

    /** Forwards a complete source tile; failure remains independent of usable native terrain data. */
    bool StoreSourceTile(const MiniMapTerrainSource::SourceTile& tile, int sourceWidth, int sourceHeight,
        uint8_t pixelFormat, bool sRGB, const uint8_t* sourceData, size_t sourceDataSize)
    {
        if (!TerrainCache::service_) return false;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        TerrainCache::CacheProfile profile;
        const auto trace = profile.GetTrace();
        const auto* observer = &trace;
#else
        const TerrainCache::Service::CacheTrace* observer = nullptr;
#endif
        const auto key = MiniMapTerrainSource::StarRuptureTerrainCacheAssociation::DescribeSource(
            tile, sourceWidth, sourceHeight, pixelFormat, sRGB);
        if (!key) return false;
        const auto result = TerrainCache::service_->StoreCachePage(*key, sourceData, sourceDataSize, observer);
        if (result.Status == TerrainCache::Service::CacheStatus::DirectoryFailure)
        {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
#endif
            LOG_ERROR("MiniMap: TerrainCache: failed to create cache directory");
        }
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        if (result.Status == TerrainCache::Service::CacheStatus::Stored)
        {
            MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
            LOG_INFO("MiniMap: TerrainCache: stored page R%d grid=(%d, %d)",
                tile.RadiationLevel, tile.GridX, tile.GridY);
        }
#endif
        return result.Status == TerrainCache::Service::CacheStatus::Stored;
    }

    /** Maps service status to the preexisting game-specific messages and hit/miss counters. */
    void TerrainCache::ReportRead(const Service::CacheResult& result,
        const MiniMapTerrainSource::SourceTile& tile, int chunkX, int chunkY)
    {
        using Status = Service::CacheStatus;
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
                tile.RadiationLevel, tile.GridX, tile.GridY);
#else
            if (result.Error) LOG_WARN(
                "MiniMap: TerrainCache: cache existence query failed for R%d grid=(%d, %d) error=%d category=%s",
                tile.RadiationLevel, tile.GridX, tile.GridY,
                result.Error.value(), result.Error.category().name());
#endif
            break;
        case Status::Hit:
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            LOG_INFO("MiniMap: TerrainCache: HIT R%d grid=(%d, %d) chunk=(%d, %d) bytes=%u",
                tile.RadiationLevel, tile.GridX, tile.GridY, chunkX, chunkY, 32768u);
#endif
            break;
        default:
            ReportReadFailure(result.Status, tile, chunkX, chunkY);
            break;
        }
    }

    /** Keeps failure messages outside the generic service and leaves invalid/missing data as a false return. */
    void TerrainCache::ReportReadFailure(Service::CacheStatus status,
        const MiniMapTerrainSource::SourceTile& tile, int chunkX, int chunkY)
    {
        using Status = Service::CacheStatus;
        switch (status)
        {
        case Status::DirectoryFailure:
            LOG_ERROR("MiniMap: TerrainCache: failed to create cache directory");
            break;
        case Status::InvalidFileSize:
            LOG_WARN("MiniMap: TerrainCache: invalid cache file size for R%d grid=(%d, %d)",
                tile.RadiationLevel, tile.GridX, tile.GridY);
            break;
        case Status::OpenFailure:
            LOG_WARN("MiniMap: TerrainCache: failed to open cache file for R%d grid=(%d, %d)",
                tile.RadiationLevel, tile.GridX, tile.GridY);
            break;
        case Status::HeaderMismatch:
            LOG_WARN("MiniMap: TerrainCache: cache header mismatch for R%d grid=(%d, %d)",
                tile.RadiationLevel, tile.GridX, tile.GridY);
            break;
        case Status::ReadFailure:
            LOG_WARN("MiniMap: TerrainCache: failed to read chunk=(%d, %d) for R%d grid=(%d, %d)",
                chunkX, chunkY, tile.RadiationLevel, tile.GridX, tile.GridY);
            break;
        default:
            break;
        }
    }
}
#endif
