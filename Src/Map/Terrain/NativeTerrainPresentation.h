// Adapts native chunk demand and application lifecycle to the generic terrain service and viewport.
// Owns request handles only; TerrainViewport remains the sole owner of native-route texture receipts.
#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "Services/Map/Terrain/TerrainService.h"
#include "Services/Map/Terrain/TerrainViewport.h"
#include "Map/MapTransform.h"
#include <list>

namespace MiniMapTerrainSource
{
    struct SourceTile;
    /** Application-owned native route. The terrain render mutex protects publication access and the
     * progression gate, but is never held across acquisition, decoding, upload or release. Render and
     * visibility callbacks skip/defer viewport mutation while progression is active. Explicit shutdown
     * requires detached callbacks and returned receipts; retain this owner and issuer on failure.
     */
    class NativeTerrainPresentation final
    {
    public:
        /** Associates the existing application cache/acquisition service at quiescent plugin startup. */
        static void BindService(Services::Map::Terrain::TerrainService& service) noexcept;
        /** Reads the running build once per preparation and starts the current application world session.
         * Runs on the game thread outside render exclusion; empty build identity rejects preparation.
         */
        static bool PrepareSession();
        /** Applies native demand cadence and current shared geometry on the existing terrain tick. */
        static void AdvanceTick(const MiniMapMap::PresentationSample& sample,
            std::uint64_t revision, float deltaSeconds);
        /** Completes deferred CPU withdrawals and eligible receipt draining on an unadmitted/FE tick. */
        static void ProcessWithdrawals();
        /** Borrows the active owner. Caller holds the terrain mutex or owns quiescent lifecycle control. */
        static NativeTerrainPresentation* Active() noexcept;
        /** Stops/drains the active owner. Deletes only after both callback detachment and receipt return. */
        static bool StopActive(bool callbacksDetached);
        /** Borrows the session/cache service and issuing texture ledger; creates no native resources. */
        NativeTerrainPresentation(Services::Map::Terrain::TerrainService& service,
            MiniMap::Services::ITextures& textures);
        /** Starts/replaces the provider session using the running game's build identity. Caller holds
         * the terrain mutex; configuration/registration are CPU-only. False preserves closed admission.
         */
        bool StartSession(std::string worldId, std::uint64_t generation, std::string contentRevision);
        /** Advances one service phase and at most one eligible upload outside rendering. Reconciliation
         * preserves the existing native radius/grid equations and runs only when requested by cadence.
         * Caller holds no render lock. Native and file operations remain synchronous/non-preemptible.
         */
        void Advance(const MiniMapMap::PresentationSample& sample, std::uint64_t revision,
            const MiniMapMap::Transform& transform, bool reconcile);
        /** Immediately suppresses drawing; defers CPU request cleanup during active progression.
         * Caller holds the terrain render mutex. Performs no native operation.
         */
        void Withdraw() noexcept;
        /** Draws only admitted native imagery using the supplied shared transform. Caller holds mutex. */
        void Draw(MiniMap::Services::ICanvas& canvas, const MiniMapMap::Transform& transform,
            float x, float y, float width, float height);
        /** Counts exactly one completed terrain callback, including empty/busy/FE frames. */
        void CompleteRenderFrame() noexcept;
        /** Stops the session and drains after tick/render detachment. False requires retaining this
         * owner and its issuer. Caller holds no render lock; performs no destructor-driven operations.
         */
        bool StopAndDrain(bool callbacksDetached);

    private:
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        /** Stack-only adapter from generic observations into the existing TerrainProfile aggregate. */
        class Diagnostics;
        /** Observes actual Debug releases without adding a wrapper call in Client Release. */
        bool DrainViewport(bool drawsDetached = false) noexcept;
#endif
        /** Tracks one demanded chunk without duplicating canonical images or GPU ownership. */
        struct ChunkRequest
        {
            std::string SlotId;
            Services::Map::Terrain::TerrainService::ImageRequest Request;
            bool Demanded = false;
            bool DeliveryAccepted = false;
            bool UploadPending = false;
            bool FailureReported = false;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            bool ProfilePublished = false;
#endif
        };
        /** Restores the progression gate on normal/exceptional return; never performs native cleanup. */
        class ProgressScope final
        {
        public:
            /** Attempts exclusive progression against render/visibility access using the existing mutex. */
            explicit ProgressScope(NativeTerrainPresentation& owner);
            /** Publishes deferred withdrawal and reopens render access after the operation exits. */
            ~ProgressScope();
            bool Entered = false;
            bool NeedsRequestWithdrawal = false;
        private:
            NativeTerrainPresentation& owner_;
            int exceptionsOnEntry_ = 0;
        };
        /** Checks lifecycle and presentation evidence under the existing terrain mutex. */
        bool IsCurrent(const MiniMapMap::PresentationSample& sample, std::uint64_t revision) const;
        /** Withdraws every service handle and slot; no IO or GPU receipt release occurs. */
        void ClearRequests();
        /** Uses physical character XYZ for generic provider selection and shared view admission. */
        bool UpdateSelection(const MiniMapMap::PresentationSample& sample,
            const MiniMapMap::Transform& transform, bool& reconcile);
        /** Computes unchanged rotation-safe visible native bounds and reconciles the requested set. */
        void Reconcile(const MiniMapMap::PresentationSample& sample, std::uint64_t revision,
            const MiniMapMap::Transform& transform);
        /** Resolves a chunk center with the native X/Y grid exchange; no borrowed texture is retained. */
        void DemandChunk(const SDK::FVector& tracking, double playerChunkX, double playerChunkY,
            int globalX, int globalY, std::int64_t order);
        /** Declares a new/existing source-and-variant-specific chunk and its exact service handle stamp. */
        void RequestChunk(const SourceTile& tile, const SDK::FVector& sample,
            int chunkX, int chunkY, std::int64_t order);
        /** Submits a fresh strictly increasing request, preserving a completed same-slot presentation. */
        void SubmitRequest(ChunkRequest& chunk,
            Services::Map::Terrain::TerrainService::ImageRequestParameters parameters);
        /** Routes changed availability and exact delivery evidence; removes unavailable requests. */
        void ObserveDeliveries();
        /** Attempts the first eligible image only; leaves unavailable upload admission pending. */
        void UploadNextImage();
        /** Copies service-issued selection evidence without inventing another generation system. */
        Services::Map::Terrain::TerrainImageDelivery::Stamp SelectionStamp() const;
        /** Inverts the existing shared map transform into normalized viewport world edges. */
        static Services::Map::Terrain::TerrainViewport::View BuildView(
            const MiniMapMap::Transform& transform, float x, float y, float width, float height);
        /** Preserves floor division and positive local chunk indices at negative coordinates. */
        static int FloorDiv(int value);
        /** Returns the original nonnegative native chunk coordinate within an eight-chunk source. */
        static int LocalChunk(int value);

        static Services::Map::Terrain::TerrainService* boundService_;
        static NativeTerrainPresentation* active_;
        Services::Map::Terrain::TerrainService& service_;
        Services::Map::Terrain::TerrainViewport viewport_;
        Services::Map::Terrain::TerrainService::Session session_;
        Services::Map::Terrain::TerrainService::Registration registration_;
        std::optional<Services::Map::Terrain::TerrainService::Selection> selection_;
        std::list<ChunkRequest> requests_;
        std::uint64_t requestGeneration_ = 0;
        bool progressing_ = false;
        bool withdrawRequested_ = true;
        bool clearRequestsPending_ = true;
    };
}
#endif
