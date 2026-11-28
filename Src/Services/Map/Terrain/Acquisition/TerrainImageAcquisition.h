// Owns one private, explicitly advanced BC1 request and its transient ingestion state.
// Provider/native borrows never enter this owner; all public results remain TerrainImageDelivery.
#pragma once
#include "../TerrainService.h"
#include "../Cache/TerrainCacheIdentity.h"

namespace Services::Map::Terrain::Acquisition
{
    /** Stable service-owned request slot. Synchronous external calls retain local strong capabilities
     * so reentrant withdrawal cannot destroy an in-flight operation. Application serializes mutation.
     */
    class TerrainImageAcquisition final
    {
    public:
        /** Copies request evidence and retains the selected provider without executing source operations. */
        TerrainImageAcquisition(TerrainService::ImageRequestParameters parameters,
            std::shared_ptr<ITerrainDataProvider> provider, ITerrainDataProvider::Position physicalPosition);
        /** Borrows the passive result while the stable request owner is alive. */
        const TerrainImageDelivery& GetDelivery() const noexcept;
        /** Reports whether another bounded phase is required, including temporarily unavailable inspection. */
        bool IsPending() const noexcept;
        /** Completes available work for one request; unavailable readiness yields and admission is checked throughout. */
        void Advance(TerrainService& service);
        /** Withdraws CPU ownership without native calls or allocation; also safe during external reentry. */
        void Withdraw() noexcept;
        /** Records a visible failure and releases transient CPU/capability ownership. */
        void Fail(std::string error);

    private:
        friend class Services::Map::Terrain::TerrainService;
        /** Private sequencing only; these phases do not add public availability or native-job states. */
        enum class Phase { Describe, Inspect, AssociateCache, WaitForCaptureBatch, ReadCache, Retrieve, Store, Admit, ValidateDelivery, Done };
        /** Synchronous ingestion sink that admits exactly one validated owned BC1 image. */
        class SourceIngestion;
        /** Dispatches one phase within one request advancement. */
        void AdvancePhase(TerrainService& service);
        /** Describes the selected provider using physical and sample coordinates independently. */
        void Describe(TerrainService& service);
        /** Inspects before memory/disk reuse; validates packed BC1 metadata and the requested rectangle. */
        void Inspect(TerrainService& service);
        /** Associates retrieval evidence or a stable capture definition with the existing page cache. */
        void AssociateCache(TerrainService& service);
        /** Reads retrieval chunks or capture pages; only retrieval misses may continue to retrieval. */
        void ReadCache(TerrainService& service);
        /** Waits on existing batch state without rereading files while the exact output is pending. */
        void WaitForCaptureBatch(TerrainService& service);
        /** Uses direct tile lookup for aligned chunks and page extraction for wider requests. */
        void ReadCaptureCache(TerrainService& service,
            const ITerrainDataProvider::CaptureDefinition& definition);
        /** Completes a cache-only capture request with a visible unavailable reason. */
        void ReportCaptureCacheUnavailable(std::string reason);
        /** Preserves capture demand only while admission is pending or this exact output is generating. */
        void WaitForCaptureOutputOrReportUnavailable(TerrainService& service,
            const ITerrainDataProvider::CaptureDefinition& definition, std::string reason);
#if defined(MODLOADER_CLIENT_BUILD)
        /** Loads one aligned 256-square request through the ordinary direct tile lookup path. */
        void ReadCaptureCacheChunk(TerrainService& service,
            const ITerrainDataProvider::CaptureDefinition& definition);
        /** Loads the complete page only when request geometry spans more than one cache tile. */
        void ReadCaptureCachePage(TerrainService& service,
            const ITerrainDataProvider::CaptureDefinition& definition);
        /** Copies every cache tile into tightly packed canonical page layout without decoding. */
        bool LoadCaptureCacheTiles(TerrainService& service,
            const ITerrainDataProvider::CaptureDefinition& definition,
            std::vector<std::uint8_t>& page, TerrainService::CacheResult& result);
#endif
        /** Consumes one full-source retrieval synchronously, or shares the single validated reuse slot. */
        void Retrieve(TerrainService& service);
        /** Attempts one complete eligible page write; failure does not invalidate usable CPU data. */
        void Store(TerrainService& service);
        /** Uses canonical extraction/admission, preserving logical extent and affine registration. */
        void Admit();
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS && defined(MODLOADER_CLIENT_BUILD)
        /** Balances optional extraction observation around the unchanged canonical admission operation. */
        void AdmitObserved(TerrainService& service);
#endif
        /** Revalidates source or capture evidence immediately before publication. */
        void ValidateDelivery(TerrainService& service);
        /** Publishes only still-current immutable data; releases all request-local source capabilities. */
        void Publish(TerrainService& service, std::shared_ptr<const TerrainImage> image);
        /** Releases transient ownership after completion, failure or withdrawal. */
        void ReleaseSource() noexcept;
        /** Validates exact packed BC1 interpretation and in-bounds aligned pixel demand. */
        bool IsValidInspection() const noexcept;
        /** Maps provider status to existing Pending/Unavailable/Failed semantics without fallback selection. */
        bool AcceptStatus(ITerrainSourceRetrieval::Status status);
        /** Compares all source evidence supplied to the synchronous ingestion sink. */
        static bool SameInspection(const ITerrainSourceRetrieval::Inspection& left,
            const ITerrainSourceRetrieval::Inspection& right) noexcept;
        /** Derives requested logical registration from the full image's normalized affine basis. */
        TerrainImage::Registration GetRequestedRegistration() const noexcept;
        /** Matches immutable content, interpretation, placement and originating registration for bounded reuse. */
        bool CanReuseSource(const TerrainService& service) const noexcept;
#if defined(MODLOADER_CLIENT_BUILD)
        /** Allows direct cache reads only for one exactly aligned canonical tile. */
        bool IsCacheChunk() const noexcept;
        std::optional<Cache::TerrainCacheIdentity> cacheKey_;
#endif
        TerrainService::ImageRequestParameters parameters_;
        std::shared_ptr<ITerrainDataProvider> provider_;
        ITerrainDataProvider::SourceQuery query_;
        ITerrainDataProvider::SourceDescription description_;
        ITerrainSourceRetrieval::Inspection inspection_;
        std::shared_ptr<const TerrainImage> sourceImage_;
        std::shared_ptr<const TerrainImage> preparedImage_;
        std::string cacheWarning_;
        TerrainImageDelivery delivery_;
        Phase phase_ = Phase::Describe;
    };
}
