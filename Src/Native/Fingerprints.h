// Stores the single resolved native-address contract for common and capture prerequisites.
#pragma once

#include "../Services/RequiredScanner.h"

#include <cstdint>
#include <span>

namespace MiniMapFingerprints
{
    struct ResolvedAddresses
    {
        uintptr_t commonUIWidgetNativeOnActivated = 0;
        uintptr_t commonUIWidgetNativeOnDeactivated = 0;
        uintptr_t buildingOnCreate = 0, buildingOnDestroy = 0;
        uintptr_t textureCreateTransient = 0, bulkLock = 0, bulkUnlock = 0;
        uintptr_t textureUpdateResource = 0, textureGetResource = 0;

        uintptr_t softObjectLoadSynchronous = 0;
        uintptr_t getBrushResourceAsTexture2D = 0;

        uintptr_t setForceMipLevelsToBeResident = 0;
        uintptr_t waitForStreaming = 0;
        uintptr_t getNumResidentMips = 0;
        uintptr_t getNumMipsAllowed = 0;
        uintptr_t getNumMips = 0;

        uintptr_t streamIn = 0;
        uintptr_t waitForPendingInitOrStreaming = 0;
        uintptr_t getFirstPlayerController = 0;
        uintptr_t getControlRotation = 0;
        uintptr_t isPlayerInForgottenEngine = 0;
        uintptr_t getComponentLocation = 0;

        uintptr_t getAllActorsOfClass = 0;
        uintptr_t pointOfInterestStaticClass = 0;
        uintptr_t findPOIMarkerCategoryData = 0;
        uintptr_t getMapMenuMarkerFilterStatus = 0;
        uintptr_t isAbandonBaseCompleted = 0;
        uintptr_t findFoundableMarkerCategoryData = 0;

        uintptr_t getMassEntitySubsystem = 0;
        uintptr_t massQueryConstruct = 0;
        uintptr_t massQueryDestruct = 0;
        uintptr_t massForEachEntityChunk = 0;
        uintptr_t massExecutionContextConstruct = 0;
        uintptr_t massExecutionContextDestruct = 0;
        uintptr_t addInventoryRequirement = 0;
        uintptr_t addTransformRequirement = 0;
        uintptr_t addSplineRequirement = 0;
        uintptr_t addAPRequirement = 0;
        uintptr_t addBuildingParametersRequirement = 0;
        uintptr_t addEnemyStateRequirement = 0;
        uintptr_t enemyStateFragmentStaticStruct = 0;
        uintptr_t enemyTagStaticStruct = 0;
        uintptr_t neutralTagStaticStruct = 0;
        uintptr_t addTagRequirement = 0;
        uintptr_t addFoundableParametersRequirement = 0;
        uintptr_t addFoundableTagRequirement = 0;
        uintptr_t getMatchingEntityHandles = 0;
        uintptr_t getMassFragmentDataPtr = 0;
        uintptr_t getMassConstSharedFragmentPtr = 0;
        uintptr_t transformFragmentStaticStruct = 0;
        uintptr_t inventoryFragmentStaticStruct = 0;
        uintptr_t foundableParametersStaticStruct = 0;
        // Mandatory Client spline-payload value-copy, cleanup and initialization metadata.
        uintptr_t getSplineActorData = 0;
        uintptr_t destroySplineActorPayloadData = 0;
        uintptr_t payloadHandleCopyConstruct = 0;
        uintptr_t payloadHandleDestruct = 0;
        uintptr_t splineActorPayloadStaticStruct = 0;

        uintptr_t getPlatformData = 0;
        uintptr_t getBulkDataSize = 0;
        uintptr_t canLoadFromDisk = 0;
        uintptr_t getBulkDataCopy = 0;
        uintptr_t memoryFree = 0;

        uintptr_t activeGarbageCollection = 0;
        uintptr_t exitPurge = 0;
        uintptr_t createWorld = 0;
        uintptr_t initWorld = 0;
        uintptr_t updateWorldComponents = 0;
        uintptr_t cleanupWorld = 0;
        uintptr_t isGarbageCollectingAndLocking = 0;
        uintptr_t isIncrementalPurgePending = 0;
        uintptr_t isIncrementalReachabilityPending = 0;
        uintptr_t destroyActor = 0;

        uintptr_t objectArray = 0;
        uintptr_t nameEqualsWide = 0;

        uintptr_t readbackFromValidEName = 0;
        uintptr_t readbackSubmit = 0;
        uintptr_t readbackConstruct = 0;
        uintptr_t readbackDestroy = 0;
        uintptr_t readbackCopy = 0;
        uintptr_t readbackLock = 0;
        uintptr_t readbackUnlock = 0;
        uintptr_t readbackMarkForDelete = 0;
        uintptr_t readbackMalloc = 0;
        uintptr_t readbackPipe = 0;
        uintptr_t readbackGpuCount = 0;

        uintptr_t spawnObject = 0;
        uintptr_t setObjectFlags = 0;
        uintptr_t retainObject = 0;
        uintptr_t releaseObject = 0;
        uintptr_t registerComponent = 0;
        uintptr_t unregisterComponent = 0;
        uintptr_t destroyComponent = 0;
        uintptr_t initRenderTarget = 0;
        uintptr_t getRenderTargetResource = 0;
        uintptr_t transitionResources = 0;
        uintptr_t isGameThread = 0;
        uintptr_t renderFenceConstruct = 0;
        uintptr_t renderFenceDestroy = 0;
        uintptr_t renderFenceBegin = 0;
        uintptr_t renderFencePoll = 0;

        // Published together by ResolveCaptureAdmission; excluded from common admission.
        uintptr_t captureShouldCreateSubsystem = 0;
        uintptr_t captureIsGameWorld = 0;
        uintptr_t captureIsPreviewWorld = 0;
        uintptr_t captureFindWorldInPackage = 0;
        uintptr_t captureDelegateAllocate = 0;
        uintptr_t captureGenerateHandle = 0;
        uintptr_t captureReadersTls = 0;
        uintptr_t captureDestructionTls = 0;
        uintptr_t captureReallocate = 0;
        uintptr_t captureQuantizeSize = 0;
        uintptr_t captureResizeHeapAllocation = 0;
        uintptr_t captureResizeAlignedHeapAllocation = 0;
        uintptr_t captureUpdatePartition = 0;
        uintptr_t captureUpdateLevels = 0;
        uintptr_t captureSendEndOfFrame = 0;
        uintptr_t captureVisibilityPending = 0;
        uintptr_t captureCanStream = 0;
        uintptr_t captureServerStreamingEnabled = 0;
        uintptr_t captureServerStreamingOutEnabled = 0;
        uintptr_t captureFlushVisibility = 0;
        uintptr_t captureWaitRenderFence = 0;
        uintptr_t captureWorldTypePreloadMap = 0;
        uintptr_t captureHasEndOfFrameUpdates = 0;
        uintptr_t captureHashNameEntry = 0;
        uintptr_t captureInvalidSetCount = 0;
        uintptr_t captureCheckFailure = 0;
        uintptr_t captureInvalidArrayCount = 0;
        uintptr_t captureMakePackagePath = 0;
        uintptr_t captureLoadPackage = 0;
        uintptr_t captureFlushRequest = 0;
        uintptr_t captureLoadingSuspended = 0;
        uintptr_t captureIsPartitionInitialized = 0;
        uintptr_t captureUninitializePartition = 0;
        uintptr_t captureEnumeratePartitions = 0;
        uintptr_t captureForceUnload = 0;
    };

    bool Resolve(
        MiniMap::Services::IRequiredScanner& scanner,
        ResolvedAddresses& addresses);

    /** Defines the independent mandatory admission-prerequisite group in the existing resolver.
     * No scanner or executable-address ownership survives the borrowed load callback.
     */
    class Fingerprints final
    {
    public:
        /** Borrows stable feature keys for every implemented admission prerequisite. */
        static std::span<const char* const> CaptureAdmissionKeys() noexcept;

        /** Resolves every prerequisite; publishes no partial group on failure. */
        static bool ResolveCaptureAdmission(MiniMap::Services::IRequiredScanner& scanner,
            ResolvedAddresses& addresses);

    private:
        using AddressField = uintptr_t ResolvedAddresses::*;
        /** Borrows patterns paired with the feature's keys; all are mandatory. */
        static std::span<const char* const> CaptureAdmissionPatterns() noexcept;
        /** Borrows destinations for atomic publication of the resolved feature. */
        static std::span<const AddressField> CaptureAdmissionFields() noexcept;
        /** Decodes the verified startup MOV's RIP-relative function-pointer destination. */
        static uintptr_t DecodeLoadingSuspendedGlobal(uintptr_t anchor);
        /** Decodes the native PostLoad map-data load back to the genuine enclosing preload map. */
        static uintptr_t DecodeWorldTypePreloadMap(uintptr_t anchor);
    };
}
