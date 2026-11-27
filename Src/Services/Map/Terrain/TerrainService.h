// Owns externally supplied terrain registrations, validated coverage and physical-position selection.
// Also owns explicitly advanced BC1 acquisition, private page caching and immutable CPU delivery.
#pragma once

#include "ITerrainDataProvider.h"
#include "TerrainImageDelivery.h"
#include <deque>
#include <functional>
#include <list>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <filesystem>
#include <system_error>
#include <utility>

namespace Services::Map::Terrain
{
    namespace Cache { class TerrainFileCache; struct TerrainCacheIdentity; }
    namespace Acquisition { class TerrainImageAcquisition; }
    /** Coordinates registrations and acquisition on one serialized calling thread, with application-scoped cache configuration.
     * Cache IO, clearing and close/drain use independent per-cache exclusion; configuration requires quiescent startup.
     * External provider calls may reenter session/selection/request invalidation. Destruction requires
     * quiescent acquisition and file operations; application supplies gameplay admission before advancement.
     */
    class TerrainService final
    {
    private:
        /** Gives one registration unique ownership identity without a global registry or native pointer. */
        struct RegistrationIdentity final {};

    public:
        /** Non-owning registration handle; expires immediately on unregister/session replacement/service destruction. */
        class Registration final
        {
        public:
            /** Creates an invalid handle, also returned when registration is rejected. */
            Registration() noexcept = default;

            /** Reports whether the issuing service still owns this registration's identity. */
            bool IsRegistered() const noexcept { return !identity_.expired(); }

            /** Compares issuing-registration identities, including expired handles, without dereferencing owners. */
            bool operator==(const Registration& other) const noexcept
            {
                return !identity_.owner_before(other.identity_) && !other.identity_.owner_before(identity_);
            }

        private:
            friend class TerrainService;

            /** Borrows the identity held by the issuing service; does not retain a provider. */
            explicit Registration(const std::shared_ptr<const RegistrationIdentity>& identity)
                : identity_(identity)
            {
            }

            std::weak_ptr<const RegistrationIdentity> identity_;
        };

        /** Identifies an application gameplay world incarnation, not source content or a native UWorld. */
        struct Session
        {
            std::string WorldId;
            std::uint64_t Generation = 0;

            /** Requires an explicit world identity and nonzero application generation. */
            bool IsValid() const noexcept { return !WorldId.empty() && Generation != 0; }
            /** Compares complete application world/session identity. */
            bool operator==(const Session&) const = default;
        };

        /** Supplies current physical character coordinates independently of tracking/view demand. */
        struct PhysicalPlayerSample
        {
            Session Context;
            ITerrainDataProvider::Position WorldPosition;
        };

        /** Copies selection evidence; a snapshot is not continued authorization after its handle expires. */
        struct Selection
        {
            Session Context;
            Registration Provider;
            std::string RegionId;
            std::uint64_t Revision = 0;
        };

        /** Passive, call-scoped observation of the service's frozen registered coverage.
         * Callers serialize capture/consumption against selection mutation and reject stale Context.
         * SelectedBounds is present only for Alternate selection; no image readiness is implied.
         */
        struct SelectionCoverage
        {
            Selection Current;
            std::optional<ITerrainDataProvider::Bounds> SelectedBounds;
            std::vector<ITerrainDataProvider::Bounds> AlternateBounds;

            /** Tests retained finite XYZ against the selected Alternate, or excludes all Alternates
             * for Default. Uses only copied coverage, without querying/reselecting a provider.
             */
            bool ContainsMarker(ITerrainDataProvider::Position position) const noexcept;
        };

        /** Creates an inert service; constructs no provider and registers no runtime callbacks. */
        TerrainService();
        /** Prevents duplicating registration identity/session ownership. */
        TerrainService(const TerrainService&) = delete;
        /** Prevents replacing a service's registrations through assignment. */
        TerrainService& operator=(const TerrainService&) = delete;
        /** Keeps the issuing service and its single-threaded associations stationary. */
        TerrainService(TerrainService&&) = delete;
        /** Prevents transferring registrations into another service's storage. */
        TerrainService& operator=(TerrainService&&) = delete;
        /** Withdraws CPU requests and releases registrations/cache configuration; callers finish operations first. */
        ~TerrainService();

        /** Admits exact packed BC1 without conversion; moves owned bytes only after metadata validation.
         * Invalid metadata/layout returns no image. Successful admission preserves every compressed byte.
         * Caller relinquishes mutable aliases as required by TerrainImage; allocation exceptions propagate.
         */
        static std::optional<TerrainImage> IngestBc1(TerrainImage::Identity identity,
            std::uint32_t width, std::uint32_t height, TerrainImage::Registration registration,
            bool sRgb, std::vector<std::uint8_t>&& blocks);
        /** Consumes borrowed RGB8 rows synchronously, encoding opaque BC1 before constructing an image.
         * Explicit byte stride includes row padding; final row needs only its logical RGB bytes.
         * Returns no image on invalid metadata/layout; retains no raw pixels or borrowed references.
         */
        static std::optional<TerrainImage> IngestRgb(TerrainImage::Identity identity,
            std::uint32_t width, std::uint32_t height, TerrainImage::Registration registration,
            bool sRgb, std::span<const std::uint8_t> pixels, std::size_t rowStride);
        /** Extracts an in-bounds logical rectangle with a 4-pixel-aligned origin, preserving registration.
         * Width/height must be positive multiples of 256; output covers exactly that canonical extent.
         * Whole-image requests share immutable storage. Other requests copy blocks without recompression.
         * Unaligned/out-of-bounds/empty requests return no image; allocation exceptions propagate.
         */
        static std::optional<TerrainImage> ExtractImage(const TerrainImage& image,
            std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height);

#if defined(MODLOADER_CLIENT_BUILD)
        /** Application-supplied absolute Terrain subtree; all record naming remains private to the service.
         * Application assigns this subtree to one service owner and routes subtree deletion through that owner.
         */
        struct CacheConfiguration
        {
            std::filesystem::path Root;
            /** Allows idempotent startup association without changing an active cache. */
            bool operator==(const CacheConfiguration&) const = default;
        };

        /** Distinguishes ordinary misses/admission failures from existing file-validation failures. */
        enum class CacheStatus
        {
            Unavailable, InvalidInput, DirectoryFailure, Miss, ExistenceFailure,
            InvalidFileSize, OpenFailure, HeaderMismatch, ReadFailure, WriteFailure, Hit, Stored
        };

        /** Contains no image ownership or jobs; error preserves filesystem existence-query diagnostics. */
        struct CacheResult
        {
            CacheStatus Status = CacheStatus::Unavailable;
            std::error_code Error;
        };

        /** Synchronous optional diagnostic phases; observers never participate in cache policy or IO. */
        enum class CacheActivity { Load, Store, Path, Header, Read, Extraction };

        /** Borrows call-local diagnostic state only until the cache method returns; callbacks must not reenter.
         * Notification must be noexcept. No pointer is retained by the service or private cache owner.
         */
        struct CacheTrace
        {
            void* Context = nullptr;
            void (*Notify)(void* context, CacheActivity activity, bool entering) noexcept = nullptr;
        };

        /** Associates one cache at quiescent application startup, before cache callbacks are admitted.
         * Repeating identical settings closes admission for startup validation; replacing settings is rejected.
         * Session changes do not clear files. No root identity/readiness is checked.
         */
        bool ConfigureTerrainCache(const CacheConfiguration& configuration);
        /** Copies configured root without IO; false before configuration. */
        bool GetTerrainCacheRoot(std::filesystem::path& root) const;
        /** Drains active terrain IO and closes read/write admission until explicit post-invalidation opening.
         * Application shutdown calls this before exit clearing; does not inspect root metadata or delete files.
         */
        void CloseTerrainCache();
        /** Opens the configured cache only after application root identity/invalidation has succeeded. */
        void OpenTerrainCache();
        /** Clears only the configured subtree under service-owned exclusion, waiting for active terrain IO.
         * Does not reopen a closed cache or consult application root-cache management.
         */
        bool ClearTerrainCache();
        /** Reads one canonical tile under subtree exclusion; preserves output if this cache instance is closed. */
        CacheResult TryLoadCacheTile(const Cache::TerrainCacheIdentity& key, int chunkX, int chunkY,
            std::vector<std::uint8_t>& compressedChunk, const CacheTrace* trace = nullptr) const;
        /** Stores one complete BC1 page under subtree exclusion; failure leaves caller source bytes untouched. */
        CacheResult StoreCachePage(const Cache::TerrainCacheIdentity& key,
            const std::uint8_t* sourceData, std::size_t sourceDataSize,
            const CacheTrace* trace = nullptr) const;
#endif

        /** Identifies one demanded logical pixel rectangle in the externally selected source.
         * Context must match GetSelection; RequestGeneration is application supplied and strictly
         * increases across submissions in one session. Pixel origins must be BC1-block aligned.
         * SamplePosition resolves a source; it never changes physical provider selection.
         */
        struct ImageRequestParameters
        {
            TerrainImageDelivery::Stamp Context;
            Registration Provider;
            std::string RegionId;
            ITerrainDataProvider::Position SamplePosition;
            std::uint32_t PixelX = 0;
            std::uint32_t PixelY = 0;
            std::uint32_t WidthPixels = 0;
            std::uint32_t HeightPixels = 0;
        };

        /** Shared observation handle for service-owned work, not a provider/native resource handle.
         * A copied handle observes withdrawal immediately; copied delivery snapshots remain passive.
         * Consumers and the service share immutable images. Withdraw handles no longer in demand.
         */
        class ImageRequest final
        {
        public:
            /** Creates an empty handle; observing it returns Unavailable. */
            ImageRequest() noexcept = default;
            /** Copies a passive availability snapshot without advancing native work or cache IO. */
            TerrainImageDelivery GetDelivery() const;
        private:
            friend class TerrainService;
            /** Shares the stable request owner retained by the service until withdrawal/invalidation. */
            explicit ImageRequest(std::shared_ptr<Acquisition::TerrainImageAcquisition> state)
                : state_(std::move(state)) {}
            std::shared_ptr<Acquisition::TerrainImageAcquisition> state_;
        };

        /** Queues BC1-only acquisition without source/IO operations. Invalid input returns a Failed handle.
         * Existing registration and selection identity are mandatory. No providers are constructed.
         */
        ImageRequest RequestImage(ImageRequestParameters parameters);
        /** Admits a new demand before withdrawing this service's old handle. Failed admission preserves old work. */
        ImageRequest ReplaceImageRequest(const ImageRequest& previous, ImageRequestParameters parameters);
        /** Withdraws this service's request and returns its exact Unavailable stamp/region snapshot.
         * Invalid/foreign handles are not modified. No files or GPU resources are removed.
         */
        TerrainImageDelivery WithdrawImageRequest(const ImageRequest& request);
        /** Completes available work for one request, on the application's admitted game-thread progression.
         * The application controls invocation frequency independently of Building Map Service timing.
         * No time allowance is imposed here. Synchronous native/provider and file operations are
         * non-preemptible; no hard execution-time guarantee is made.
         * No callbacks/workers are installed. Reentrant invalidation is supported; recursive advance
         * is ignored. Exceptions mark current work Failed and propagate to the application.
         */
        void AdvanceImageRequests();

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS && defined(MODLOADER_CLIENT_BUILD)
        /** Optional call-local CPU diagnostics. Callbacks are noexcept, observation-only and must not
         * reenter the service. Cache.Context is shared by all callbacks. Nothing is retained after
         * the observed advancement returns; no provider, plugin or timing implementation is required.
         */
        struct ImageRequestTrace
        {
            CacheTrace Cache;
            void (*ReadResult)(void*, const CacheResult&) noexcept = nullptr;
            void (*Extraction)(void*, bool entering) noexcept = nullptr;
            void (*Progress)(void*, bool cpuReady) noexcept = nullptr;
        };
        /** Runs the same request advancement with borrowed Debug observation; no scheduling policy changes. */
        void AdvanceImageRequests(const ImageRequestTrace& trace);
#endif

        /** Begins valid session, replacing prior registrations; repeating the current session is a no-op.
         * Returns false for invalid identity. No provider is constructed or registered automatically.
         */
        bool BeginSession(const Session& session);

        /** Stops only the matching current session and immediately releases its registration references. */
        bool StopSession(const Session& session);

        /** Copies/validates provider coverage and retains a strong shared provider reference.
         * Rejects null/duplicate providers, IDs, defaults, wrong worlds and invalid/overlapping regions.
         * Returns an invalid handle on rejection; provider/allocation exceptions propagate without admission.
         */
        Registration RegisterProvider(std::shared_ptr<ITerrainDataProvider> provider);

        /** Removes only this service's matching registration; returns false for foreign/removed handles.
         * Immediately recomputes selection; external provider owners and snapshots remain independent.
         */
        bool UnregisterProvider(const Registration& registration);

        /** Copies strong ownership only if registration currently belongs to this service; otherwise returns null. */
        std::shared_ptr<ITerrainDataProvider> GetProvider(const Registration& registration) const;

        /** Updates selection for current-session finite coordinates; invalid current coordinates withdraw it.
         * Returns false for invalid/stale samples; stale sessions leave current session/selection untouched.
         */
        bool UpdatePlayerPosition(const PhysicalPlayerSample& sample);

        /** Returns current selection, or no value until a valid physical sample selects registered coverage. */
        std::optional<Selection> GetSelection() const;
        /** Copies current identity and registered bounds, never calling a provider or progressing work.
         * Requires the same serialized calling contract as GetSelection; null means no valid selection.
         */
        std::optional<SelectionCoverage> GetSelectionCoverage() const;

    private:
        friend class Acquisition::TerrainImageAcquisition;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS && defined(MODLOADER_CLIENT_BUILD)
        const ImageRequestTrace* imageRequestTrace_ = nullptr;
#endif
        /** Checks existing session, selected registration and delivery stamp before/after external operations. */
        bool IsRequestCurrent(const Acquisition::TerrainImageAcquisition& request) const noexcept;
        /** Withdraws all CPU requests and the single retained full-source image, with no IO/native calls. */
        void InvalidateImageRequests() noexcept;

        /** One bounded adjacent-request reuse slot; never an open-ended resident tile cache. */
        struct RetainedSource
        {
            Registration Provider;
            std::shared_ptr<const TerrainImage> Image;
            ITerrainSourceRetrieval::Inspection Inspection;
        };
        /** Owns one provider and the immutable copied coverage/identity used by the sole registry. */
        struct Entry
        {
            std::shared_ptr<ITerrainDataProvider> Provider;
            ITerrainDataProvider::Coverage Coverage;
            std::shared_ptr<const RegistrationIdentity> Identity;
        };

        /** Finds a live local registration without treating matching provider pointers as handle identity. */
        const Entry* FindEntry(const Registration& registration) const noexcept;
        /** Checks duplicate object/ID/default and cross-registration region identity/intersection. */
        bool CanRegister(const ITerrainDataProvider* provider,
            const ITerrainDataProvider::Coverage& coverage) const noexcept;
        /** Compares alternate regions across two already internally validated coverage values. */
        static bool HasRegionConflict(const ITerrainDataProvider::Coverage& left,
            const ITerrainDataProvider::Coverage& right) noexcept;
        /** Selects the unique alternate match, otherwise default, using the last valid physical sample. */
        void RefreshSelection();
        /** Changes selection only when its registration/region changes, advancing observation revision. */
        void SetSelection(Registration registration, const std::string& regionId);
        /** Advances revision without allowing silent generation reuse at integer exhaustion. */
        void AdvanceSelectionRevision();
        /** Clears metadata and registration ownership without calling provider acquisition/lifecycle methods. */
        void ClearSession() noexcept;

        std::optional<Session> session_;
        std::vector<Entry> entries_;
        std::optional<ITerrainDataProvider::Position> physicalPosition_;
        Registration selectedProvider_;
        std::string selectedRegionId_;
        std::uint64_t selectionRevision_ = 0;
        std::list<std::shared_ptr<Acquisition::TerrainImageAcquisition>> imageRequests_;
        std::deque<std::shared_ptr<Acquisition::TerrainImageAcquisition>> pendingImageRequests_;
        std::optional<TerrainImageDelivery::Stamp> lastRequestStamp_;
        RetainedSource retainedSource_;
        bool advancingImages_ = false;
#if defined(MODLOADER_CLIENT_BUILD)
        std::unique_ptr<Cache::TerrainFileCache> fileCache_;
#endif
    };
}
