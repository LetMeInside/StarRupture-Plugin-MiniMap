// Presents immutable BC1 deliveries through the application's existing texture ledger and canvas.
// Owns independent image slots, upload receipts and shared viewport geometry; never acquires sources or caches.
#pragma once

#include "TerrainImageDelivery.h"
#include "Services/Viewport.h"
#include <list>
#include <atomic>

namespace Services::Map::Terrain
{
    /** Independent presentation owner. Caller serializes all methods and keeps the
     * supplied texture service alive until all receipts are returned. CompleteRenderFrame alone may
     * run concurrently with other methods; it only advances an atomic CPU counter. Upload/drain run on the game
     * thread outside rendering; Draw runs in the existing canvas callback. No callbacks are installed.
     * Reentrant demand/delivery invalidation during native operations is supported; recursive upload
     * or drain is rejected. Drawing and destruction must not reenter a native operation.
     * Before destruction: Withdraw, stop/detach drawing, then DrainRetirements until true.
     * Unresolved ownership requires retaining this object; destruction with receipt debt terminates.
     */
    class TerrainViewport final
    {
    public:
        /** Defines visible world coverage independently of player/provider selection. The affine basis
         * maps normalized viewport edges into world XY; pixel coordinates have an upper-left origin.
         * Rotation, reflection and shear are represented by the existing canvas image-quad operation.
         */
        struct View
        {
            TerrainImage::Registration WorldFromViewport;
            float X = 0.0f;
            float Y = 0.0f;
            float Width = 0.0f;
            float Height = 0.0f;

            /** Rejects nonfinite, empty or unrepresentable pixel rectangles and invalid world bases. */
            bool IsValid() const noexcept;
        };

        /** Reports snapshot admission only; Pending/Failed are CPU availability, never GPU job states. */
        enum class DeliveryResult { Stale, Invalid, Pending, Accepted, Withdrawn, Failed };
        /** Reports one explicit synchronous upload attempt, without inventing GPU completion tickets. */
        enum class UploadResult
        {
            NoImage, Unchanged, AlreadyAttempted, Busy, Unavailable, Rejected, Retained, Superseded, Published
        };

        /** Borrows an application-owned generic texture capability; performs no native work. */
        explicit TerrainViewport(MiniMap::Services::ITextures& textures) noexcept;
        /** Prevents copying authoritative receipt ownership or viewport demand. */
        TerrainViewport(const TerrainViewport&) = delete;
        /** Prevents overwriting receipt debt through assignment. */
        TerrainViewport& operator=(const TerrainViewport&) = delete;
        /** Enforces explicit draining; never silently drops unresolved receipts or invokes native teardown. */
        ~TerrainViewport();

        /** Sets shared geometry and authoritative world/session/selection. RequestGeneration is ignored
         * here; exact request evidence belongs to each slot. Invalid input leaves state unchanged.
         * A selection change withdraws all slots immediately. Pan/zoom preserves images and receipts.
         */
        bool SetDemand(const TerrainImageDelivery::Stamp& selection, const View& view);
        /** Declares or updates an application-defined slot, with exact delivery stamp and region.
         * Requires the current selection and nonzero request generation. Changed region withdraws its
         * presentation; changed request retains it until replacement succeeds. Lower order draws first;
         * equal orders use lexicographic slot identity. Re-declare retained slots or explicitly withdraw
         * removed slots; this operation does not remove other slots. After withdrawal, use fresh request
         * evidence when redeclaring an identity so old deliveries cannot match. No grid policy is inferred.
         */
        bool DeclareSlot(std::string slotId, const TerrainImageDelivery::Stamp& context,
            std::string regionId, std::int64_t order);
        /** Accepts only a declared slot's exact stamp and region. Pending/Failed retain presentation;
         * Unavailable withdraws that slot, requiring explicit declaration before any further delivery.
         */
        DeliveryResult AcceptDelivery(const std::string& slotId, const TerrainImageDelivery& delivery);
        /** Attempts at most one image decode/upload for this declared slot, outside rendering.
         * Unchanged images are reused; failed/uncertain attempts are not retried for the same admitted
         * image. A new request allows deliberate retry. Temporary opaque logical RGBA uses the existing
         * 64 MiB decode bound and UploadRGBA API; no persistent RGBA or color reinterpretation.
         * Allocation failures propagate before native entry. The application controls admission timing.
         */
        UploadResult Upload(const std::string& slotId);
        /** Removes one slot immediately without losing its outstanding receipt obligations. */
        void WithdrawSlot(const std::string& slotId) noexcept;
        /** Draws all current affine image corners in declared order with normalized logical UVs
         * under the viewport clip.
         * Does no decoding, upload, widget mutation or native resource acquisition. Returns whether drawn.
         */
        bool Draw(MiniMap::Services::ICanvas& canvas) const;
        /** Advances the existing three-completed-render-callback grace period, including empty callbacks.
         * Caller invokes exactly once after each serialized terrain render callback, never as a GPU fence.
         * May run during an excluded upload/drain; it never traverses presentation or receipt state.
         */
        void CompleteRenderFrame() noexcept;
        /** Withdraws demand and presentation immediately; retains all outstanding receipt obligations. */
        void Withdraw() noexcept;
        /** Returns eligible receipts through their issuing ledger. true means no viewport receipt debt,
         * not GPU retirement completion. drawsDetached acknowledges all prior CPU draw uses are detached
         * and bypasses callback grace at shutdown. Uncertain operations are never replayed.
         */
        bool DrainRetirements(bool drawsDetached = false) noexcept;
        /** Reports retained receipts, including the current presentation; safe to inspect without native work. */
        bool HasReceipts() const noexcept;

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        /** Optional call-local presentation diagnostics; no texture ownership is passed to observers.
         * Callbacks must be noexcept and must not reenter. Released reports API result and whether
         * receipt ownership was actually returned. Timing and aggregation remain caller-owned.
         */
        struct PresentationTrace
        {
            /** Identifies only operations that need separate elapsed-time observation. */
            enum class Activity { Decode, Upload, Release };
            void* Context = nullptr;
            void (*Notify)(void*, Activity, bool entering) noexcept = nullptr;
            void (*Released)(void*, MiniMap::Services::ReleaseState, bool returned) noexcept = nullptr;
            /** Balances one synchronous notification across early returns and exceptions. */
            class Scope final
            {
            public:
                /** Borrows the trace only through the enclosing observed operation. */
                Scope(const PresentationTrace* trace, Activity activity) noexcept
                    : trace_(trace), activity_(activity)
                {
                    if (trace_ && trace_->Notify) trace_->Notify(trace_->Context, activity_, true);
                }
                /** Prevents duplicate end notifications through copied observation scopes. */
                Scope(const Scope&) = delete;
                /** Prevents replacing an active borrowed notification. */
                Scope& operator=(const Scope&) = delete;
                /** Closes the notification without performing any native work. */
                ~Scope() { End(); }
                /** Ends once, so the next phase never inherits this elapsed interval. */
                void End() noexcept
                {
                    if (trace_ && trace_->Notify) trace_->Notify(trace_->Context, activity_, false);
                    trace_ = nullptr;
                }
            private:
                const PresentationTrace* trace_;
                Activity activity_;
            };
        };
        /** Observes one unchanged upload call; retains no callback/context beyond return. */
        UploadResult Upload(const std::string& slotId, const PresentationTrace& trace);
        /** Observes actual release attempts only, including their authoritative ownership outcomes. */
        bool DrainRetirements(bool drawsDetached, const PresentationTrace& trace) noexcept;
#endif

    private:
        /** Stable authoritative receipt slot. Retired/uncertain slots remain owned until explicit return. */
        struct Presentation
        {
            std::shared_ptr<const TerrainImage> Image;
            MiniMap::Services::OwnedTexture Owner;
            bool Retired = false;
            bool Uncertain = false;
            std::uint64_t RetireAfterRenderFrame = 0;
        };

        /** Holds one declared demand and nonowning current-presentation pointer. Shared lifetime keeps
         * reentrant withdrawal safe during upload; receipt ownership stays solely in presentations_.
         */
        struct ImageSlot
        {
            std::string Id;
            TerrainImageDelivery::Stamp Context;
            std::string RegionId;
            std::int64_t Order = 0;
            std::shared_ptr<const TerrainImage> DesiredImage;
            bool UploadAttempted = false;
            Presentation* Current = nullptr;
        };

        /** Compares existing world/session/selection evidence without request generations. */
        static bool SameSelection(const TerrainImageDelivery::Stamp& left,
            const TerrainImageDelivery::Stamp& right) noexcept;
        /** Compares all existing stamp fields without a second generation system. */
        static bool SameStamp(const TerrainImageDelivery::Stamp& left,
            const TerrainImageDelivery::Stamp& right) noexcept;
        /** Orders slots outside Draw by application order and stable identity. */
        static bool SlotPrecedes(const std::shared_ptr<ImageSlot>& left,
            const std::shared_ptr<ImageSlot>& right) noexcept;
        /** Finds a declared slot without creating demand; returns null for withdrawn identities. */
        std::shared_ptr<ImageSlot> FindSlot(const std::string& slotId) const noexcept;
        /** Compares content, dimensions, registration and color interpretation without copying pixels. */
        static bool SameImage(const TerrainImage& left, const TerrainImage& right) noexcept;
        /** Publishes only if the same slot instance, demand and image survive native entry. */
        UploadResult FinishUpload(Presentation& candidate, MiniMap::Services::UploadState result,
            const std::shared_ptr<ImageSlot>& slot, const TerrainImageDelivery::Stamp& context,
            const std::string& regionId) noexcept;
        /** Removes a slot's draw identity and schedules its receipt for explicit retirement. */
        void RetirePresentation(ImageSlot& slot) noexcept;
        /** Projects this image's logical corners through the shared viewport, rejecting overflow. */
        bool BuildQuad(const TerrainImage& image, MiniMap::Services::Quad& positions) const noexcept;

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        const PresentationTrace* presentationTrace_ = nullptr;
#endif
        MiniMap::Services::ITextures& textures_;
        std::optional<TerrainImageDelivery::Stamp> demand_;
        View view_;
        bool nativeOperationInProgress_ = false;
        std::atomic<std::uint64_t> completedRenderFrames_{0};
        std::list<std::shared_ptr<ImageSlot>> slots_;
        std::list<Presentation> presentations_;
    };
}
