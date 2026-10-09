#if defined(MODLOADER_CLIENT_BUILD)

#include "NativeApi.h"
#include "Fingerprints.h"
#include "plugin_helpers.h"

namespace
{
    MiniMapNative::NativeApi g_nativeApi = {};
    bool g_initialized = false;
}


namespace MiniMapNative
{
    bool Initialize(
        const MiniMapFingerprints::ResolvedAddresses& addresses)
    {
        Shutdown();

        if (addresses.softObjectLoadSynchronous == 0 ||
            addresses.getBrushResourceAsTexture2D == 0 ||
            addresses.setForceMipLevelsToBeResident == 0 ||
            addresses.waitForStreaming == 0 ||
            addresses.getNumResidentMips == 0 ||
            addresses.getNumMipsAllowed == 0 ||
            addresses.getNumMips == 0 ||
            addresses.streamIn == 0 ||
            addresses.waitForPendingInitOrStreaming == 0 ||
            addresses.getFirstPlayerController == 0 ||
            addresses.getPlayerPawn == 0 ||
            addresses.getControlRotation == 0 ||
            addresses.isPlayerInForgottenEngine == 0 ||
            addresses.getComponentLocation == 0 ||
            addresses.getAllActorsOfClass == 0 ||
            addresses.pointOfInterestStaticClass == 0 ||
            addresses.findPOIMarkerCategoryData == 0 ||
            addresses.getMapMenuMarkerFilterStatus == 0 ||
            addresses.isAbandonBaseCompleted == 0 ||
            addresses.findFoundableMarkerCategoryData == 0 ||
            addresses.getMassEntitySubsystem == 0 ||
            addresses.massQueryConstruct == 0 ||
            addresses.massQueryDestruct == 0 ||
            addresses.addInventoryRequirement == 0 ||
            addresses.addTransformRequirement == 0 ||
            addresses.addEnemyStateRequirement == 0 ||
            addresses.enemyStateFragmentStaticStruct == 0 ||
            addresses.enemyTagStaticStruct == 0 ||
            addresses.neutralTagStaticStruct == 0 ||
            addresses.addTagRequirement == 0 ||
            addresses.addFoundableParametersRequirement == 0 ||
            addresses.addFoundableTagRequirement == 0 ||
            addresses.getMatchingEntityHandles == 0 ||
            addresses.getMassFragmentDataPtr == 0 ||
            addresses.getMassConstSharedFragmentPtr == 0 ||
            addresses.transformFragmentStaticStruct == 0 ||
            addresses.inventoryFragmentStaticStruct == 0 ||
            addresses.foundableParametersStaticStruct == 0 ||
            addresses.getPlatformData == 0 ||
            addresses.getBulkDataSize == 0 ||
            addresses.canLoadFromDisk == 0 ||
            addresses.getBulkDataCopy == 0 ||
            addresses.memoryFree == 0)
        {
            return false;
        }

        g_nativeApi.asset.loadSynchronous =
            reinterpret_cast<AssetApi::LoadSynchronousFn>(
                addresses.softObjectLoadSynchronous);

        g_nativeApi.player.getFirstPlayerController =
            reinterpret_cast<PlayerApi::GetFirstPlayerControllerFn>(
                addresses.getFirstPlayerController);

        g_nativeApi.player.getPlayerPawn =
            reinterpret_cast<PlayerApi::GetPlayerPawnFn>(
                addresses.getPlayerPawn);

        g_nativeApi.player.getControlRotation =
            reinterpret_cast<PlayerApi::GetControlRotationFn>(
                addresses.getControlRotation);

        g_nativeApi.player.isPlayerInForgottenEngine =
            reinterpret_cast<PlayerApi::IsPlayerInForgottenEngineFn>(
                addresses.isPlayerInForgottenEngine);

        g_nativeApi.player.getComponentLocation =
            reinterpret_cast<PlayerApi::GetComponentLocationFn>(
                addresses.getComponentLocation);

        g_nativeApi.pointsOfInterest.getAllActorsOfClass =
            reinterpret_cast<PointsOfInterestApi::GetAllActorsOfClassFn>(
                addresses.getAllActorsOfClass);

        g_nativeApi.pointsOfInterest.pointOfInterestStaticClass =
            reinterpret_cast<PointsOfInterestApi::POIStaticClassFn>(
                addresses.pointOfInterestStaticClass);

        g_nativeApi.pointsOfInterest.findPOIMarkerCategoryData =
            reinterpret_cast<
            PointsOfInterestApi::FindPOIMarkerCategoryDataFn>(
                addresses.findPOIMarkerCategoryData);

        g_nativeApi.pointsOfInterest.getMarkerFilterStatus =
            reinterpret_cast<PointsOfInterestApi::GetMarkerFilterStatusFn>(
                addresses.getMapMenuMarkerFilterStatus);

        g_nativeApi.pointsOfInterest.isAbandonBaseCompleted =
            reinterpret_cast<PointsOfInterestApi::IsAbandonBaseCompletedFn>(
                addresses.isAbandonBaseCompleted);

        g_nativeApi.pointsOfInterest.findFoundableMarkerCategoryData =
            reinterpret_cast<
            PointsOfInterestApi::FindFoundableMarkerCategoryDataFn>(
                addresses.findFoundableMarkerCategoryData);

        g_nativeApi.mass.getMassEntitySubsystem =
            reinterpret_cast<MassApi::GetMassEntitySubsystemFn>(
                addresses.getMassEntitySubsystem);

        g_nativeApi.mass.queryConstruct =
            reinterpret_cast<MassApi::QueryConstructFn>(
                addresses.massQueryConstruct);

        g_nativeApi.mass.queryDestruct =
            reinterpret_cast<MassApi::QueryDestructFn>(
                addresses.massQueryDestruct);

        g_nativeApi.mass.addEnemyStateRequirement =
            reinterpret_cast<MassApi::AddEnemyStateRequirementFn>(
                addresses.addEnemyStateRequirement);

        g_nativeApi.mass.enemyStateFragmentStaticStruct =
            reinterpret_cast<MassApi::StaticStructFn>(
                addresses.enemyStateFragmentStaticStruct);

        g_nativeApi.mass.enemyTagStaticStruct =
            reinterpret_cast<MassApi::StaticStructFn>(
                addresses.enemyTagStaticStruct);

        g_nativeApi.mass.neutralTagStaticStruct =
            reinterpret_cast<MassApi::StaticStructFn>(
                addresses.neutralTagStaticStruct);

        g_nativeApi.mass.addTagRequirement =
            reinterpret_cast<MassApi::AddTagRequirementFn>(
                addresses.addTagRequirement);

        g_nativeApi.mass.addInventoryRequirement =
            reinterpret_cast<MassApi::AddFragmentRequirementFn>(
                addresses.addInventoryRequirement);

        g_nativeApi.mass.addTransformRequirement =
            reinterpret_cast<MassApi::AddFragmentRequirementFn>(
                addresses.addTransformRequirement);

        g_nativeApi.mass.addFoundableParametersRequirement =
            reinterpret_cast<MassApi::AddSharedRequirementFn>(
                addresses.addFoundableParametersRequirement);

        g_nativeApi.mass.addFoundableTagRequirement =
            reinterpret_cast<MassApi::AddSharedRequirementFn>(
                addresses.addFoundableTagRequirement);

        g_nativeApi.mass.getMatchingEntityHandles =
            reinterpret_cast<MassApi::GetMatchingEntityHandlesFn>(
                addresses.getMatchingEntityHandles);

        g_nativeApi.mass.getFragmentDataPtr =
            reinterpret_cast<MassApi::GetFragmentDataPtrFn>(
                addresses.getMassFragmentDataPtr);

        g_nativeApi.mass.getConstSharedFragmentPtr =
            reinterpret_cast<MassApi::GetConstSharedFragmentPtrFn>(
                addresses.getMassConstSharedFragmentPtr);

        g_nativeApi.mass.transformFragmentStaticStruct =
            reinterpret_cast<MassApi::StaticStructFn>(
                addresses.transformFragmentStaticStruct);

        g_nativeApi.mass.inventoryFragmentStaticStruct =
            reinterpret_cast<MassApi::StaticStructFn>(
                addresses.inventoryFragmentStaticStruct);

        g_nativeApi.mass.foundableParametersStaticStruct =
            reinterpret_cast<MassApi::StaticStructFn>(
                addresses.foundableParametersStaticStruct);

        g_nativeApi.texture.getBrushTexture =
            reinterpret_cast<TextureApi::GetBrushTextureFn>(
                addresses.getBrushResourceAsTexture2D);

        g_nativeApi.texture.setForceMipLevelsToBeResident =
            reinterpret_cast<
            TextureApi::SetForceMipLevelsToBeResidentFn>(
                addresses.setForceMipLevelsToBeResident);

        g_nativeApi.texture.waitForStreaming =
            reinterpret_cast<TextureApi::WaitForStreamingFn>(
                addresses.waitForStreaming);

        g_nativeApi.texture.getNumResidentMips =
            reinterpret_cast<TextureApi::GetNumResidentMipsFn>(
                addresses.getNumResidentMips);

        g_nativeApi.texture.getNumMipsAllowed =
            reinterpret_cast<TextureApi::GetNumMipsAllowedFn>(
                addresses.getNumMipsAllowed);

        g_nativeApi.texture.getNumMips =
            reinterpret_cast<TextureApi::GetNumMipsFn>(
                addresses.getNumMips);

        g_nativeApi.texture.streamIn =
            reinterpret_cast<TextureApi::StreamInFn>(
                addresses.streamIn);

        g_nativeApi.texture.waitForPendingInitOrStreaming =
            reinterpret_cast<
            TextureApi::WaitForPendingInitOrStreamingFn>(
                addresses.waitForPendingInitOrStreaming);

        g_nativeApi.texture.getPlatformData =
            reinterpret_cast<TextureApi::GetPlatformDataFn>(
                addresses.getPlatformData);

        g_nativeApi.texture.getBulkDataSize =
            reinterpret_cast<TextureApi::GetBulkDataSizeFn>(
                addresses.getBulkDataSize);

        g_nativeApi.texture.canLoadFromDisk =
            reinterpret_cast<TextureApi::CanLoadFromDiskFn>(
                addresses.canLoadFromDisk);

        g_nativeApi.texture.getBulkDataCopy =
            reinterpret_cast<TextureApi::GetBulkDataCopyFn>(
                addresses.getBulkDataCopy);

        g_nativeApi.texture.memoryFree =
            reinterpret_cast<TextureApi::MemoryFreeFn>(
                addresses.memoryFree);

        // Optional diagnostic addresses: a missing resolution disables Stage A.1
        // without disabling the established Stage A inventory or MiniMap.
        g_nativeApi.gridDiagnostic.getSubsystem = reinterpret_cast<GridDiagnosticApi::GetSubsystemFn>(addresses.getEntityGridSubsystem);
        g_nativeApi.gridDiagnostic.findInBox = reinterpret_cast<GridDiagnosticApi::FindInBoxFn>(addresses.gridFindEntitiesInBox);
        g_nativeApi.gridDiagnostic.destructItems = reinterpret_cast<GridDiagnosticApi::DestructItemsFn>(addresses.gridDestructResultItems);
        g_nativeApi.gridDiagnostic.viewConstruct = reinterpret_cast<GridDiagnosticApi::ViewConstructFn>(addresses.massEntityViewConstruct);
        g_nativeApi.gridDiagnostic.viewHasTag = reinterpret_cast<GridDiagnosticApi::ViewHasTagFn>(addresses.massEntityViewHasTag);

        g_initialized = true;

        LOG_INFO(
            "MiniMap: native API initialized");

        return true;
    }


    void Shutdown()
    {
        g_nativeApi = {};
        g_initialized = false;
    }


    bool IsInitialized()
    {
        return g_initialized;
    }


    const NativeApi* Get()
    {
        if (!g_initialized)
        {
            return nullptr;
        }

        return &g_nativeApi;
    }
}

#endif
