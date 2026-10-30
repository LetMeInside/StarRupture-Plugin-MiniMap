#pragma once

#include "../Services/RequiredScanner.h"

#include <cstdint>

namespace MiniMapFingerprints
{
    struct ResolvedAddresses
    {
        uintptr_t commonUIWidgetNativeOnActivated = 0;
        uintptr_t commonUIWidgetNativeOnDeactivated = 0;

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
        uintptr_t addInventoryRequirement = 0;
        uintptr_t addTransformRequirement = 0;
        uintptr_t isMassEntityValid = 0;
        uintptr_t isMassEntityBuilt = 0;
        uintptr_t gridForEachCellInRadius = 0; // XY collector traversal.
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
        uintptr_t splineActorPayloadStaticStruct = 0;

        // Mandatory collector grid subsystem resolution.
        uintptr_t getEntityGridSubsystem = 0;

        uintptr_t getPlatformData = 0;
        uintptr_t getBulkDataSize = 0;
        uintptr_t canLoadFromDisk = 0;
        uintptr_t getBulkDataCopy = 0;
        uintptr_t memoryFree = 0;

        uintptr_t activeGarbageCollection = 0;
        uintptr_t engineStorage = 0;
        uintptr_t exitPurge = 0;
        uintptr_t createWorld = 0;
        uintptr_t initWorld = 0;
        uintptr_t updateWorldComponents = 0;
        uintptr_t cleanupWorld = 0;
        uintptr_t getWorldContextFromWorld = 0;
        uintptr_t getSubsystemInternal = 0;
        uintptr_t isGarbageCollectingAndLocking = 0;
        uintptr_t isIncrementalPurgePending = 0;
        uintptr_t isIncrementalReachabilityPending = 0;
        uintptr_t getTickableTickType = 0;
        uintptr_t isVisibilityRequestPending = 0;
        uintptr_t destroyActor = 0;

        uintptr_t objectArray = 0;
        uintptr_t nameEqualsWide = 0;

        uintptr_t readbackFromValidEName = 0;
        uintptr_t readbackSubmit = 0;
        uintptr_t readbackImmediate = 0;
        uintptr_t readbackCreate = 0;
        uintptr_t readbackConstruct = 0;
        uintptr_t readbackDestroy = 0;
        uintptr_t readbackCopy = 0;
        uintptr_t readbackLock = 0;
        uintptr_t readbackUnlock = 0;
        uintptr_t readbackPoll = 0;
        uintptr_t readbackMarkForDelete = 0;
        uintptr_t readbackMalloc = 0;
        uintptr_t readbackFree = 0;
        uintptr_t readbackDynamicRHI = 0;
        uintptr_t readbackPipe = 0;
        uintptr_t readbackThreaded = 0;
        uintptr_t readbackMultithreaded = 0;
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
    };

    bool Resolve(
        MiniMap::Services::IRequiredScanner& scanner,
        ResolvedAddresses& addresses);
}
