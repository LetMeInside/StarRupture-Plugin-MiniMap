// Retains one private GamePreview residency set and serializes fresh captures through its native lifecycle.
#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "PrivateStreamingDefinition.h"
#include "PrivateStreamingLevels.h"
#include "PrivateWorldNative.h"
#include "WorldPartitionAdmissionHook.h"
#include "TerrainImageCapture.h"
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
#include <chrono>
#include <string_view>
#endif
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace SDK { class UWorld; class UObject; class UPackage; class UWorldPartition;
    class UWorldPartitionSubsystem; class AActor; class UWorldPartitionStreamingSourceComponent;
    class UDataLayerInstance; }

namespace Services::Map::Terrain::Capture
{
    class PackageLoadOperation;
    /** Owns one private residency session and at most one fresh image capture at a time.
     * All methods run on the serialized game thread; a Ready session stays resident until explicit retirement.
     */
    class PrivateStreamingSession final
    {
    public:
        /** Describes native progression; logical cancellation/failure is reported separately in the reason. */
        enum class Status { Inactive, LoadingRoot, Streaming, SubmittingDeferredWork, Ready, Retiring, Retired };
        /** Reports the last admitted capture's outcome independently from its native retirement state. */
        enum class CaptureResultStatus { None, Pending, ImageAvailable, Completed, Failed };
        /** Creates inert ownership; neither loads a package nor installs a hook. */
        PrivateStreamingSession();
        /** Requires acknowledged retirement and stopped hook before releasing owners. */
        ~PrivateStreamingSession();
        /** Keeps callbacks, fences and hook ownership stationary. */
        PrivateStreamingSession(const PrivateStreamingSession&) = delete;
        /** Prevents replacing native ownership. */
        PrivateStreamingSession& operator=(const PrivateStreamingSession&) = delete;
        /** Starts a configured asynchronous root load; false can leave explicit retirement debt. */
        bool Submit(PrivateStreamingDefinition definition, SDK::UWorld* gameplayWorld,
            std::uint64_t generation, const ITerrainDataProvider::CaptureDefinition& residencyDefinition,
            MiniMap::Services::INativeHooks& hooks);
        /** Admits one fresh image capture over the unchanged, positively ready residency set. */
        bool RequestCapture(const ITerrainDataProvider::CaptureDefinition& definition,
            std::uint64_t generation, bool retainDiagnosticPixels);
        /** Transfers completed immutable image ownership while capture issuance remains open. */
        std::shared_ptr<const TerrainImage> TakeCompletedImage();
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        /** Transfers temporary raw RGB samples for immediate opt-in diagnostic export. */
        TerrainCaptureDiagnosticPixels TakeCompletedDiagnosticPixels() noexcept;
#endif
        /** Reports pending work, available output, completed delivery or terminal capture failure. */
        CaptureResultStatus GetCaptureResultStatus() const noexcept { return captureResultStatus_; }
        /** Reports whether the prior capture's native owner has reached its retirement boundary. */
        bool IsCaptureRetired() const noexcept { return capture_ == nullptr; }
        /** Returns the retained terminal capture failure explanation, if any. */
        const std::string& GetCaptureResultReason() const noexcept { return captureResultReason_; }
        /** Advances streaming or one capture step; never runs gameplay Tick/BeginPlay or progresses both together. */
        void Advance();
        /** Cancels logical demand; owners remain until native retirement. */
        void RequestRetirement();
        /** Closes registration synchronously before the containing gameplay partition unregisters.
         * Root loading before registration can continue draining after this acknowledgement.
         */
        bool BeforeGameplayWorldEnd(SDK::UWorld* world);
        /** Performs only request-specific waits at explicit shutdown; false preserves all outstanding debt. */
        bool TryShutdown();
        /** Reports a passive state without progressing native work. */
        Status GetStatus() const noexcept { return status_; }
        /** Describes a logical failure/cancellation without implying successful resource retirement. */
        const std::string& GetReason() const noexcept { return reason_; }
    private:
        /** Balances the native allocation returned by the private manager's layer enumeration. */
        struct DataLayerList;
        /** Requires complete bindings, the native game thread and no native GC/purge. */
        bool CanAdvance() const;
        /** Submits under reentrancy exclusion and converts interrupted native submission into retirement debt. */
        bool SubmitRoot();
        /** Excludes reentrant shutdown while request-specific waits execute native callbacks. */
        bool DrainOwnedRequests();
        /** Waits for owned package and visibility transitions before private world cleanup. */
        bool DrainStreamingRequests();
        /** Validates external values before any hook/load/ownership is admitted. */
        static bool IsDefinitionValid(const PrivateStreamingDefinition& definition);
        /** Acquires a native object reference before any operation that may progress teardown. */
        void Retain(SDK::UObject* object);
        /** Consumes a fully retired root callback and validates a separate private graph. */
        bool AdoptRoot();
        /** Initializes the original cooked graph once under exact-world selective admission. */
        bool InitializeWorld();
        /** Applies explicit per-private-world Data Layer requirements and observes effective values. */
        bool ConfigureDataLayers();
        /** Finds an asset's instance only through this private partition's manager and owned layer graph. */
        const SDK::UDataLayerInstance* ResolveDataLayer(const StreamingDataLayer& requirement) const;
        /** Creates a generic source actor/root/component and uses native component registration. */
        bool CreateSource();
        /** Creates, retains, registers and positions the root owned by the private source actor. */
        bool CreateSourceRoot(SDK::AActor* actor);
        /** Configures the unregistered native source before enabling its registration lifecycle. */
        bool ConfigureSource(SDK::AActor* actor);
        /** Advances package loading asynchronously, then initializes and establishes regional demand. */
        void AdvanceRoot();
        /** Advances native policy/levels and checks positive required-cell and completed-registration evidence. */
        void AdvanceStreaming();
        /** Routes registered actors and fences deferred work once the required residency is ready. */
        void AdvanceResidencyReadiness();
        /** Routes visible, registered private levels and reports native completion or interruption. */
        bool AdvanceResidentLevelActorInitialization();
        /** Advances one registered level and reports whether its native routing is complete. */
        bool AdvanceLevelActorInitialization(SDK::ULevel* level);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        /** Logs the first complete routing census of the capture-ready candidate. */
        void LogActorInitializationDiagnostics() const;
        /** Reports phase changes promptly and throttles repeated observations of the same phase. */
        void ReportProgress(std::string_view phase);
        /** Adds one level's native counts to the caller's aggregate totals. */
        void AccumulateActorInitializationDiagnostics(SDK::ULevel* level,
            PrivateWorldNative::ActorInitializationDiagnostics& totals) const;
#endif
        /** Requires all nonempty configured cells to be activated, privately owned and registered. */
        bool HasRequiredCells() const;
        /** Validates one loaded private cell package/world/level and completed native registration. */
        bool HasCellEvidence(const RequiredStreamingCell& cell, const SDK::ULevelStreaming* stream) const;
        /** Compares stable residency inputs without resolving native names or generated private identities. */
        bool HasCompatibleResidency(const ITerrainDataProvider::CaptureDefinition& definition) const noexcept;
        /** Advances the sole active capture and retains its immutable output before asynchronous retirement. */
        void AdvanceCapture();
        /** Retires capture ownership asynchronously and reports whether native debt is fully gone. */
        bool RetireCapture();
        /** Attempts synchronous retirement at a safe before-EndPlay or explicit shutdown boundary. */
        bool RetireCaptureSynchronously();
        /** Unregisters the source/partitions after retaining all currently observable private levels. */
        bool CloseRegistration();
        /** Drains level transitions, deferred work, cleanup and the final native fence. */
        void AdvanceRetirement();
        /** Releases retained objects and the retired package operation only after native acknowledgements. */
        void FinishRetirement();
        /** Records logical failure while preserving native ownership for retirement. */
        void Fail(const char* reason);

        PrivateStreamingDefinition definition_;
        std::string originalWorldPackage_;
        std::vector<std::string> cookedCellPackages_;
        std::vector<std::string> targetGridNames_;
        std::vector<std::string> activatedDataLayerAssets_;
        ITerrainDataProvider::Bounds streamingBounds_;
        std::unique_ptr<PackageLoadOperation> rootLoad_;
        std::unique_ptr<TerrainImageCapture> capture_;
        std::shared_ptr<const TerrainImage> completedImage_;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        TerrainCaptureDiagnosticPixels completedDiagnosticPixels_;
#endif
        CaptureResultStatus captureResultStatus_ = CaptureResultStatus::None;
        std::string captureResultReason_;
        WorldPartitionAdmissionHook hook_;
        PrivateStreamingLevels levels_;
        PrivateWorldNative native_;
        std::vector<SDK::UObject*> owners_;
        std::vector<const SDK::UDataLayerInstance*> dataLayers_; // Borrowed from owners_, in definition order.
        SDK::UWorld* gameplayWorld_ = nullptr; // Identity only; no gameplay object is attached to the private world.
        SDK::UWorld* world_ = nullptr;
        SDK::UPackage* package_ = nullptr; // Native reference is owned solely by owners_ after callback transfer.
        SDK::UWorldPartition* partition_ = nullptr;
        SDK::UWorldPartitionSubsystem* subsystem_ = nullptr;
        SDK::UWorldPartitionStreamingSourceComponent* source_ = nullptr;
        std::uint64_t generation_ = 0;
        Status status_ = Status::Inactive;
        std::string reason_;
        bool initialized_ = false;
        bool unregistered_ = true;
        bool cleaned_ = false;
        bool advancing_ = false;
        bool retirementRequested_ = false;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        std::chrono::steady_clock::time_point actorInitializationStarted_{};
        std::chrono::steady_clock::time_point lastProgressReport_{};
        std::string_view lastProgressPhase_; // Borrows static phase names.
        std::uint64_t actorRoutingCalls_ = 0;
        std::chrono::steady_clock::duration actorRoutingDuration_{};
        bool actorInitializationStartedValid_ = false;
        bool actorInitializationReported_ = false;
#endif
    };
}
#endif
