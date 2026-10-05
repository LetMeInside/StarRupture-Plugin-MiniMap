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