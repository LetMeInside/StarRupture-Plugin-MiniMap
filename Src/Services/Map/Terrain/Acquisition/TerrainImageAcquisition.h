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
        /** Executes one source/IO/CPU phase, checking service admission after external calls. */
        void Advance(TerrainService& service);
        /** Withdraws CPU ownership without native calls or allocation; also safe during external reentry. */
        void Withdraw() noexcept;
        /** Records a visible failure and releases transient CPU/capability ownership. */
        void Fail(std::string error);

    private:
        friend class Services::Map::Terrain::TerrainService;
        /** Private sequencing only; these phases do not add public availability or native-job states. */
        enum class Phase { Describe, Inspect, AssociateCache, ReadCache, Retrieve, Store, Admit, ValidateDelivery, Done };
        /** Synchronous ingestion sink that admits exactly one validated owned BC1 image. */
        class SourceIngestion;
        /** Describes the selected provider using physical and sample coordinates independently. */
        void Describe(TerrainService& service);
        /** Inspects before memory/disk reuse; validates packed BC1 metadata and the requested rectangle. */
        void Inspect(TerrainService& service);
        /** Associates supported pages with complete generic inspected identity and affine placement. */
        void AssociateCache(TerrainService& service);
        /** Reads only a requested canonical cache tile; misses proceed to full-source retrieval. */
        void ReadCache(TerrainService& service);
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
        /** Re-inspects staged source evidence immediately before publication, without further file operations. */
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
