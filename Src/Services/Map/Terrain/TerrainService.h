// Owns terrain registrations, physical selection, explicitly advanced BC1 acquisition and CPU image delivery.
// Client builds also route opaque capture requests through the retained private streaming session.
#pragma once

#include "ITerrainDataProvider.h"
#include "TerrainImageDelivery.h"
#if defined(MODLOADER_CLIENT_BUILD)
#include "Capture/PrivateStreamingSession.h"
#endif
#include <deque>
#include <functional>
#include <list>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
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
        /** Requires private native shutdown, then withdraws CPU requests and releases registrations/cache configuration. */
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
        /** Submits one internal region for the current gameplay generation through an admitted batch.
         * Stable capture residency inputs are converted privately; no capture/image request is submitted.
         * This service retains the native session independently of image requests.
         * A rejected native submission can still require AdvancePrivateStreaming/ShutdownPrivateStreaming.
         */
        bool RequestPrivateStreaming(const ITerrainDataProvider::CaptureDefinition& definition, SDK::UWorld* gameplayWorld,
            MiniMap::Services::INativeHooks& hooks);
        /** Admits one image over compatible retained residency without exposing its private UWorld. */
        bool RequestPrivateCapture(const ITerrainDataProvider::CaptureDefinition& definition);
        /** Transfers completed immutable output after one cache attempt; cache failure does not discard the image. */
        std::shared_ptr<const TerrainImage> TakePrivateCaptureImage();
        /** Reports the last capture outcome independently of private native retirement progress. */
        Capture::PrivateStreamingSession::CaptureResultStatus GetPrivateCaptureResultStatus() const noexcept;
        /** Returns the retained terminal failure explanation for the last capture operation. */
        std::string GetPrivateCaptureResultReason() const;
        /** Advances existing streaming or capture work on the admitted game thread, without starting new work. */
        void AdvancePrivateStreaming();
        /** Withdraws native demand without discarding asynchronous ownership. */
        void RetirePrivateStreaming();
        /** Closes private partition registration before the enclosing gameplay partition unregisters. */
        bool BeforeGameplayWorldEnd(SDK::UWorld* world);
        /** Refuses shutdown while native resources, callbacks, fences or the admission hook retain ownership. */
        bool ShutdownPrivateStreaming();
        /** Exposes positive session state only; Ready does not claim final image/visual readiness. */
        Capture::PrivateStreamingSession::Status GetPrivateStreamingStatus() const noexcept;
        /** Exposes a logical failure/cancellation explanation separately from retirement state. */
        std::string GetPrivateStreamingReason() const;

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

        /** Reports the exact persistence outcome for the most recently consumed private capture image. */
        CacheResult GetPrivateCaptureCacheResult() const noexcept;

        /** Reports logical capture-batch progress separately from private-world retirement status. */
        enum class CaptureBatchStatus { Idle, Running, Completed, Failed, Cancelled };
        /** Describes whether one exact capture output may become readable through the active batch. */
        enum class CaptureOutputState { AwaitingBatchAdmission, Generating, CacheReady, Unavailable };
        /** Copies batch progress; a completed image remains available if its persistence did not complete. */
        struct CaptureBatchDelivery
        {
            CaptureBatchStatus Status = CaptureBatchStatus::Idle;
            std::size_t CompletedImages = 0;
            std::size_t TotalImages = 0;
            std::size_t MissingCacheImages = 0;
            std::size_t InvalidCacheRecords = 0;
            CacheResult Cache;
            std::string Error;
            std::shared_ptr<const TerrainImage> UnpersistedImage;
        };
        /** Admits a finite cache-first collection; native context is used only if images are missing. */
        bool RequestPrivateCaptureBatch(
            const std::vector<ITerrainDataProvider::CaptureDefinition>& definitions,
            SDK::UWorld* gameplayWorld, MiniMap::Services::INativeHooks& hooks);
        /** Copies batch progress without advancing cache or native work. */
        CaptureBatchDelivery GetPrivateCaptureBatchDelivery() const;
        /** Reports when an exact definition can be read from cache or may still be generated by this session's batch. */
        CaptureOutputState GetPrivateCaptureOutputState(
            const ITerrainDataProvider::CaptureDefinition& definition) const noexcept;
        /** Resolves pre-admission capture demand when the application cannot submit its one batch attempt. */
        void RejectPrivateCaptureBatchAdmission(
            const std::vector<ITerrainDataProvider::CaptureDefinition>& definitions,
            std::string reason);

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
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS && defined(MODLOADER_CLIENT_BUILD)
        /** Enables optional export after a validated capture completes; disabled by default. */
        void ConfigureCaptureDiagnosticExport(bool enabled) noexcept;
#endif
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
#if defined(MODLOADER_CLIENT_BUILD)
        /** Builds the existing page-cache identity from stable capture output metadata. */
        bool CreateCaptureCacheIdentity(const ITerrainDataProvider::CaptureDefinition& definition,
            Cache::TerrainCacheIdentity& identity) const;
        /** Confirms every immutable image field against the definition admitted for that capture. */
        bool MatchesCaptureDefinition(const TerrainImage& image,
            const ITerrainDataProvider::CaptureDefinition& definition) const;
        /** Persists only a complete matching canonical page and preserves its exact cache result. */
        CacheResult PersistCaptureImage(const TerrainImage& image,
            const ITerrainDataProvider::CaptureDefinition& definition);
        /** Revalidates newly completed output and invokes optional diagnostics before cache-failure handling. */
        bool ValidateCompletedCaptureImage(const TerrainImage& image,
            const ITerrainDataProvider::CaptureDefinition& definition) const;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        /** Exports validated immutable capture bytes and their canonical BC1 decode for diagnostics. */
        void ExportCompletedCapture(const TerrainImage& image,
            const ITerrainDataProvider::CaptureDefinition& definition) const noexcept;
        /** Exports transient pre-BC1 and supersampled RGB for one newly completed image. */
        void ExportPreCompressionCapture(const Capture::TerrainCaptureDiagnosticPixels& pixels,
            const ITerrainDataProvider::CaptureDefinition& definition) const noexcept;
        /** Builds the existing Diagnostics directory and safe page basename without touching the cache. */
        bool GetCaptureDiagnosticPath(const ITerrainDataProvider::CaptureDefinition& definition,
            std::filesystem::path& directory, std::wstring& baseName) const;
        /** Writes one complete diagnostic file and logs open, write or close failure. */
        static bool WriteCaptureDiagnosticFile(const std::filesystem::path& path,
            std::span<const std::uint8_t> bytes, const std::string& sourceId,
            const char* format) noexcept;
        /** Converts borrowed packed RGB to opaque RGBA and saves it through the existing WIC PNG encoder. */
        static bool WriteCaptureDiagnosticRgbPng(const std::filesystem::path& path,
            std::span<const std::uint8_t> rgb, std::uint32_t width, std::uint32_t height,
            const std::string& sourceId) noexcept;
        /** Saves opaque RGBA through the existing WIC PNG encoder. */
        static bool WriteCaptureDiagnosticRgbaPng(const std::filesystem::path& path,
            std::span<const std::uint8_t> rgba, std::uint32_t width, std::uint32_t height,
            const std::string& sourceId) noexcept;
        /** Decodes canonical BC1 and saves the exact opaque RGBA presentation as a PNG. */
        static bool WriteCaptureDiagnosticPng(const std::filesystem::path& path,
            const TerrainImage& image, const std::string& sourceId) noexcept;
#endif
#endif

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
        /** Tracks the single finite cache-first capture collection through the existing session owner. */
        enum class CaptureBatchPhase { Idle, AwaitingResidency, Capturing, AwaitingCaptureRetirement };
        /** Owns definitions and indices only; retains at most one unpersisted completed result image. */
        struct CaptureBatchState
        {
            CaptureBatchStatus Status = CaptureBatchStatus::Idle;
            CaptureBatchPhase Phase = CaptureBatchPhase::Idle;
            std::vector<ITerrainDataProvider::CaptureDefinition> Definitions;
            std::vector<std::size_t> MissingDefinitionIndices;
            std::size_t NextMissingIndex = 0;
            std::size_t CompletedImages = 0;
            std::size_t TotalImages = 0;
            std::size_t MissingCacheImages = 0;
            std::size_t InvalidCacheRecords = 0;
            CacheResult Cache;
            std::string Error;
            std::shared_ptr<const TerrainImage> UnpersistedImage;
            Session Context;
            SDK::UWorld* GameplayWorld = nullptr;
            bool OwnsStreamingSession = false;
        };
        /** Validates every output, page filename and shared residency contract before native work. */
        bool ValidatePrivateCaptureBatch(
            const std::vector<ITerrainDataProvider::CaptureDefinition>& definitions,
            std::string& error) const;
        /** Validates one capture's supported geometry, cache identity and existing filename contract. */
        bool ValidatePrivateCaptureDefinition(
            const ITerrainDataProvider::CaptureDefinition& definition,
            Cache::TerrainCacheIdentity& identity, std::wstring& filename) const;
        /** Compares page filenames in the existing Windows case-insensitive namespace. */
        static bool SameCaptureCacheFilename(std::wstring left, std::wstring right);
        /** Checks each cache tile and classifies missing/corrupt records separately from IO failures. */
        CacheResult CheckPrivateCapturePage(const Cache::TerrainCacheIdentity& identity,
            bool& complete, bool& invalidRecord) const;
        /** Checks every validated output page before permitting any private-world submission. */
        bool CheckPrivateCaptureBatchCache();
        /** Logs the completed cache-first admission counts after every page was inspected. */
        void LogPrivateCaptureBatchCacheSummary() const;
        /** Completes the finite request without constructing or loading a private world. */
        void CompletePrivateCaptureBatchFromCache();
        /** Starts the sole private session only after a genuine cache miss has been found. */
        bool SubmitPrivateCaptureResidency(
            const ITerrainDataProvider::CaptureDefinition& definition,
            SDK::UWorld* gameplayWorld, MiniMap::Services::INativeHooks& hooks);
        /** Advances only the current batch using existing private-streaming status and retirement. */
        void AdvancePrivateCaptureBatch();
        /** Waits for positive retained-session readiness before submitting the next missing capture. */
        void AdvanceCaptureBatchResidency();
        /** Consumes a ready immutable image or records the existing capture failure. */
        void AdvanceCaptureBatchImage();
        /** Submits exactly the next missing image after positive residency and prior capture retirement. */
        bool SubmitNextPrivateCapture();
        /** Consumes one completed image, preserves a failed-write result, and waits for native retirement. */
        void ConsumePrivateCaptureBatchImage();
        /** Completes the collection after the last capture retires, or submits the next missing definition. */
        void ContinuePrivateCaptureBatch();
        /** Ends further submissions, preserving any prior persistent pages and native owner. */
        void FailPrivateCaptureBatch(std::string reason, CacheResult cache = {},
            std::shared_ptr<const TerrainImage> unpersistedImage = nullptr);
        /** Withdraws batch progression while leaving native retirement to the existing session owner. */
        void CancelPrivateCaptureBatch(std::string reason);
        /** Compares only stable inputs that determine retained residency, excluding output metadata. */
        static bool HasSameCaptureResidency(const ITerrainDataProvider::CaptureDefinition& left,
            const ITerrainDataProvider::CaptureDefinition& right) noexcept;
        /** Compares the complete pixel-affecting output and shared residency definitions. */
        static bool HasSameCaptureOutput(const ITerrainDataProvider::CaptureDefinition& left,
            const ITerrainDataProvider::CaptureDefinition& right) noexcept;
        CaptureBatchState privateCaptureBatch_;
        bool advancingCaptureBatch_ = false;
        std::unique_ptr<Cache::TerrainFileCache> fileCache_;
        std::unique_ptr<Capture::PrivateStreamingSession> privateStreaming_;
        std::optional<ITerrainDataProvider::CaptureDefinition> privateCaptureDefinition_;
        CacheResult privateCaptureCacheResult_;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        bool exportCaptureDiagnostics_ = false;
#endif
#endif
    };
}
