#if defined(MODLOADER_CLIENT_BUILD)

#include "NativeApi.h"
#include "Fingerprints.h"
#include "plugin_helpers.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <type_traits>

namespace
{
    MiniMapNative::NativeApi g_nativeApi = {};
    bool g_initialized = false;
}


namespace MiniMapNative
{
    namespace
    {
        template<class T> bool ReadAt(uintptr_t base,size_t offset,T& out)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            if(base>UINTPTR_MAX-offset)return false;
            return ReadMemory(base+offset,&out,sizeof(T));
        }
    }

    bool IsMemoryRangeReadable(uintptr_t p,size_t bytes,size_t alignment)
    {
        if(!p||!alignment||(alignment&(alignment-1))||(p&(alignment-1))||p>UINTPTR_MAX-bytes)return false;
        const auto end=p+bytes;
        while(p<end)
        {
            MEMORY_BASIC_INFORMATION m{};
            if(!VirtualQuery(reinterpret_cast<const void*>(p),&m,sizeof(m))||m.State!=MEM_COMMIT||
                (m.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
            const auto protection=m.Protect&0xff;
            if(protection!=PAGE_READONLY&&protection!=PAGE_READWRITE&&protection!=PAGE_WRITECOPY&&
                protection!=PAGE_EXECUTE_READ&&protection!=PAGE_EXECUTE_READWRITE&&protection!=PAGE_EXECUTE_WRITECOPY)return false;
            const auto regionBase=reinterpret_cast<uintptr_t>(m.BaseAddress);
            if(regionBase>UINTPTR_MAX-m.RegionSize||regionBase+m.RegionSize<=p)return false;
            p=(std::min)(end,regionBase+m.RegionSize);
        }
        return true;
    }


    bool ReadMemory(uintptr_t address,void* output,size_t bytes)
    {
        if(!output||!IsMemoryRangeReadable(address,bytes))return false;
        std::memcpy(output,reinterpret_cast<const void*>(address),bytes);
        return true;
    }


    bool ValidateObjectIdentity(uintptr_t p,uintptr_t objectArray,size_t extent)
    {
        if(!IsMemoryRangeReadable(p,extent,8)||!IsMemoryRangeReadable(objectArray,0x30,8))return false;
        int32_t index=-1,num=0,max=0,chunks=0,maxChunks=0;
        if(!ReadAt(p,0xc,index)||!ReadAt(objectArray,0x20,max)||!ReadAt(objectArray,0x24,num)||
            !ReadAt(objectArray,0x28,maxChunks)||!ReadAt(objectArray,0x2c,chunks))return false;
        if(index<0||num<0||max<num||max>64*1024*1024||index>=num||chunks<0||
           maxChunks<chunks||maxChunks>4096||int64_t(num)>int64_t(chunks)*65536||
           int64_t(max)>int64_t(maxChunks)*65536)return false;
        const size_t chunkIndex=uint32_t(index)>>16;
        if(chunkIndex>=size_t(chunks))return false;
        uintptr_t table=0,chunk=0;
        if(!ReadAt(objectArray,0x10,table)||
            !IsMemoryRangeReadable(table,(chunkIndex+1)*8,8)||
            !ReadAt(table,chunkIndex*8,chunk))return false;
        const size_t itemOffset=size_t(uint32_t(index)&65535)*0x18;
        if(chunk>UINTPTR_MAX-itemOffset||!IsMemoryRangeReadable(chunk+itemOffset,0x18,8))return false;
        uintptr_t actual=0;uint32_t internalFlags=0,objectFlags=0;
        if(!ReadAt(chunk+itemOffset,0,actual)||!ReadAt(chunk+itemOffset,8,internalFlags)||
            !ReadAt(p,8,objectFlags))return false;
        // PDB: Unreachable | Garbage; pending load/postload and destruction.
        return actual==p&&!(internalFlags&0x10200000)&&!(objectFlags&0x1b400);
    }


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

        // Optional grid/collector addresses: missing resolutions disable the
        // dependent stage without disabling Stage A or the established MiniMap.
        g_nativeApi.gridDiagnostic.getSubsystem = reinterpret_cast<GridDiagnosticApi::GetSubsystemFn>(addresses.getEntityGridSubsystem);
        g_nativeApi.gridDiagnostic.forEachCellInRadius = reinterpret_cast<GridDiagnosticApi::ForEachCellInRadiusFn>(addresses.gridForEachCellInRadius);
        g_nativeApi.mass.addSplineRequirement = reinterpret_cast<MassApi::AddFragmentRequirementFn>(addresses.addSplineRequirement);
        g_nativeApi.mass.isEntityValid = reinterpret_cast<MassApi::EntityCheckFn>(addresses.isMassEntityValid);
        g_nativeApi.mass.isEntityBuilt = reinterpret_cast<MassApi::EntityCheckFn>(addresses.isMassEntityBuilt);
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
