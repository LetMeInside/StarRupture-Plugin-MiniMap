#pragma once

#include "../plugin.h"
#include "plugin_interface.h"

#include <cstdint>

namespace MiniMapFingerprints
{
    struct ResolvedAddresses
    {
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
        uintptr_t getPlayerPawn = 0;
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

        uintptr_t getPlatformData = 0;
        uintptr_t getBulkDataSize = 0;
        uintptr_t canLoadFromDisk = 0;
        uintptr_t getBulkDataCopy = 0;
        uintptr_t memoryFree = 0;
    };

    bool Resolve(
        IPluginSelf* self,
        IPluginHookScanner* scanner,
        ResolvedAddresses& addresses);
}