#if defined(MODLOADER_CLIENT_BUILD)

#include "NativeApi.h"
#include "DefinitionAcquisitionCore.h"
#include "Fingerprints.h"
#include "plugin_helpers.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <type_traits>
#include <chrono>
#include <atomic>

namespace
{
    MiniMapNative::NativeApi g_nativeApi = {};
    bool g_initialized = false;
}


namespace MiniMapNative
{
    // BEGIN direct-query accounting (constant initialized; independent of NativeApi lifetime).
    namespace
    {
        constinit std::array<std::atomic<uint64_t>,QueryCellCount> directQueries{};
        constinit std::array<std::atomic<uint32_t>,size_t(QueryThread::Count)> queryThreadIds{};
        thread_local QueryPhase queryPhase=QueryPhase::Unclassified;
        static_assert(std::atomic<uint64_t>::is_always_lock_free);
        static_assert(std::atomic<uint32_t>::is_always_lock_free);
    }
    void SetQueryThreadIdentity(QueryThread kind,uint32_t thread) noexcept
    {
        queryThreadIds[size_t(kind)].store(thread,std::memory_order_relaxed);
    }
    ScopedQueryPhase::ScopedQueryPhase(QueryPhase phase) noexcept:Previous(queryPhase)
    {
        // Lifecycle cleanup remains the root even when teardown invokes capture helpers.
        if(queryPhase!=QueryPhase::Shutdown && queryPhase!=QueryPhase::WorldCleanup &&
            phase!=QueryPhase::Unclassified) queryPhase=phase;
    }
    ScopedQueryPhase::~ScopedQueryPhase() noexcept {queryPhase=Previous;}
    void CountDirectQuery(QuerySite site) noexcept
    {
        const auto id=GetCurrentThreadId();
        QueryThread thread=QueryThread::Other;
        // Verified game/UI identities take precedence when initialization shares a thread.
        for(auto kind:{QueryThread::Initialization,QueryThread::UI,QueryThread::Game})
            if(queryThreadIds[size_t(kind)].load(std::memory_order_relaxed)==id)thread=kind;
        const size_t cell=(size_t(site)*size_t(QueryPhase::Count)+size_t(queryPhase))*size_t(QueryThread::Count)+size_t(thread);
        directQueries[cell].fetch_add(1,std::memory_order_relaxed);
    }
    DirectQuerySnapshot SnapshotDirectQueries() noexcept
    {
        DirectQuerySnapshot result;
        auto collect=[](auto& values) noexcept {
            for(size_t i=0;i<QueryCellCount;++i)values[i]=directQueries[i].load(std::memory_order_relaxed);
        };
        collect(result.Cells);
        for(unsigned attempt=0;attempt<4;++attempt)
        {
            std::array<uint64_t,QueryCellCount> next{};collect(next);
            if(next==result.Cells){result.Stable=true;break;}
            result.Cells=next;
        }
        for(size_t site=0;site<size_t(QuerySite::Count);++site)
            for(size_t phase=0;phase<size_t(QueryPhase::Count);++phase)
                for(size_t thread=0;thread<size_t(QueryThread::Count);++thread)
                {
                    const auto value=result.Cells[(site*size_t(QueryPhase::Count)+phase)*size_t(QueryThread::Count)+thread];
                    result.Total+=value;result.Sites[site]+=value;result.Phases[phase]+=value;result.Threads[thread]+=value;
                }
        return result;
    }
    // END direct-query accounting.

    namespace
    {
        thread_local ValidationProfile* validationProfile=nullptr;
        struct ValidationMeasurement
        {
            ValidationProfile* Profile=validationProfile;
            bool Identity;
            std::chrono::steady_clock::time_point Start{};
            explicit ValidationMeasurement(bool identity):Identity(identity)
            {
                if(Profile){if(Profile->MeasureTime)Start=std::chrono::steady_clock::now();
                    if(Identity)++Profile->IdentityCalls;else ++Profile->ReadableCalls;}
            }
            ~ValidationMeasurement()
            {
                if(!Profile||!Profile->MeasureTime)return;
                const auto ns=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-Start).count());
                if(Identity)Profile->IdentityNanoseconds+=ns;else Profile->ReadableNanoseconds+=ns;
            }
        };
    }
    ScopedValidationProfile::ScopedValidationProfile(ValidationProfile& profile):Previous(validationProfile)
    {validationProfile=&profile;}
    ScopedValidationProfile::~ScopedValidationProfile()
    {
        // Include the independent inventory observer in its enclosing audit's
        // work totals while retaining the observer's own detailed counters.
        if(Previous)
        {
            Previous->IdentityCalls+=validationProfile->IdentityCalls;
            Previous->IdentityNanoseconds+=validationProfile->IdentityNanoseconds;
            Previous->ReadableCalls+=validationProfile->ReadableCalls;
            Previous->ReadableNanoseconds+=validationProfile->ReadableNanoseconds;
            Previous->VirtualQueries+=validationProfile->VirtualQueries;
        }
        validationProfile=Previous;
    }

    void RecordVirtualQuery() noexcept {if(validationProfile)++validationProfile->VirtualQueries;}

    bool IsMemoryRangeReadable(uintptr_t p,size_t bytes,size_t alignment)
    {
        ValidationMeasurement measurement(false);
        if(!p||!alignment||(alignment&(alignment-1))||(p&(alignment-1))||p>UINTPTR_MAX-bytes)return false;
        const auto end=p+bytes;
        while(p<end)
        {
            MEMORY_BASIC_INFORMATION m{};
            if(validationProfile)++validationProfile->VirtualQueries;
            CountDirectQuery(QuerySite::NativeApi);
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
        ValidationMeasurement measurement(true);
        if(!IsMemoryRangeReadable(p,(std::max)(extent,size_t(0x10)),8)||!IsMemoryRangeReadable(objectArray,0x30,8))return false;
        // Copy only from the ranges just validated above. Re-querying each field
        // in those same ranges adds no identity evidence. No range/identity cache
        // survives this call, and all original object-array/flag checks remain.
        auto copy=[]<class T>(uintptr_t base,size_t offset,T& value)
        {std::memcpy(&value,reinterpret_cast<const void*>(base+offset),sizeof(T));};
        int32_t index=-1,num=0,max=0,chunks=0,maxChunks=0;
        copy(p,0xc,index);copy(objectArray,0x20,max);copy(objectArray,0x24,num);
        copy(objectArray,0x28,maxChunks);copy(objectArray,0x2c,chunks);
        if(index<0||num<0||max<num||max>64*1024*1024||index>=num||chunks<0||
           maxChunks<chunks||maxChunks>4096||int64_t(num)>int64_t(chunks)*65536||
           int64_t(max)>int64_t(maxChunks)*65536)return false;
        const size_t chunkIndex=uint32_t(index)>>16;
        if(chunkIndex>=size_t(chunks))return false;
        uintptr_t table=0,chunk=0;
        copy(objectArray,0x10,table);
        if(!IsMemoryRangeReadable(table,(chunkIndex+1)*8,8))return false;
        copy(table,chunkIndex*8,chunk);
        const size_t itemOffset=size_t(uint32_t(index)&65535)*0x18;
        if(chunk>UINTPTR_MAX-itemOffset||!IsMemoryRangeReadable(chunk+itemOffset,0x18,8))return false;
        uintptr_t actual=0;uint32_t internalFlags=0,objectFlags=0;
        copy(chunk+itemOffset,0,actual);copy(chunk+itemOffset,8,internalFlags);copy(p,8,objectFlags);
        // PDB: Unreachable | Garbage; pending load/postload and destruction.
        return actual==p&&!(internalFlags&MiniMapDefinitionAcquisition::UnusableObjectItemFlags)&&!(objectFlags&0x1b400);
    }


    bool Initialize(
        const MiniMapFingerprints::ResolvedAddresses& addresses)
    {
        Shutdown();

        if (addresses.softObjectLoadSynchronous == 0 ||
            addresses.commonUIWidgetNativeOnActivated == 0 ||
            addresses.commonUIWidgetNativeOnDeactivated == 0 ||
            addresses.getBrushResourceAsTexture2D == 0 ||
            addresses.setForceMipLevelsToBeResident == 0 ||
            addresses.waitForStreaming == 0 ||
            addresses.getNumResidentMips == 0 ||
            addresses.getNumMipsAllowed == 0 ||
            addresses.getNumMips == 0 ||
            addresses.streamIn == 0 ||
            addresses.waitForPendingInitOrStreaming == 0 ||
            addresses.getFirstPlayerController == 0 ||
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
            addresses.getSplineActorData == 0 ||
            addresses.destroySplineActorPayloadData == 0 ||
            addresses.splineActorPayloadStaticStruct == 0 ||
            addresses.isGameThread == 0 ||
            addresses.getPlatformData == 0 ||
            addresses.getBulkDataSize == 0 ||
            addresses.canLoadFromDisk == 0 ||
            addresses.getBulkDataCopy == 0 ||
            addresses.memoryFree == 0)
        {
            return false;
        }

        if (!addresses.retainObject || !addresses.releaseObject || !addresses.nameEqualsWide) return false;

#if defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
        if (!addresses.objectArray || !addresses.getSkeletalMeshAsset) return false;
        if (!addresses.activeGarbageCollection) return false;
        if (!addresses.engineStorage) return false;
        if (!addresses.exitPurge) return false;
        if (!addresses.createWorld) return false;
        if (!addresses.initWorld) return false;
        if (!addresses.updateWorldComponents) return false;
        if (!addresses.cleanupWorld) return false;
        if (!addresses.getWorldContextFromWorld) return false;
        if (!addresses.getSubsystemInternal) return false;
        if (!addresses.isGarbageCollectingAndLocking) return false;
        if (!addresses.isIncrementalPurgePending) return false;
        if (!addresses.isIncrementalReachabilityPending) return false;
        if (!addresses.getTickableTickType) return false;
        if (!addresses.isVisibilityRequestPending) return false;
        if (!addresses.destroyActor) return false;
        if (!addresses.readbackFromValidEName) return false;
        if (!addresses.readbackSubmit) return false;
        if (!addresses.readbackImmediate) return false;
        if (!addresses.readbackCreate) return false;
        if (!addresses.readbackConstruct) return false;
        if (!addresses.readbackDestroy) return false;
        if (!addresses.readbackCopy) return false;
        if (!addresses.readbackLock) return false;
        if (!addresses.readbackUnlock) return false;
        if (!addresses.readbackPoll) return false;
        if (!addresses.readbackMarkForDelete) return false;
        if (!addresses.readbackMalloc) return false;
        if (!addresses.readbackFree) return false;
        if (!addresses.readbackDynamicRHI) return false;
        if (!addresses.readbackPipe) return false;
        if (!addresses.readbackThreaded) return false;
        if (!addresses.readbackMultithreaded) return false;
        if (!addresses.readbackGpuCount) return false;
        if (!addresses.spawnObject) return false;
        if (!addresses.setObjectFlags) return false;
        if (!addresses.registerComponent) return false;
        if (!addresses.unregisterComponent) return false;
        if (!addresses.destroyComponent) return false;
        if (!addresses.initRenderTarget) return false;
        if (!addresses.getRenderTargetResource) return false;
        if (!addresses.transitionResources) return false;
        if (!addresses.renderFenceConstruct) return false;
        if (!addresses.renderFenceDestroy) return false;
        if (!addresses.renderFenceBegin) return false;
        if (!addresses.renderFencePoll) return false;
#endif

        g_nativeApi.asset.loadSynchronous =
            reinterpret_cast<AssetApi::LoadSynchronousFn>(
                addresses.softObjectLoadSynchronous);

        g_nativeApi.commonUI.nativeOnActivated =
            reinterpret_cast<CommonUIApi::NativeOnActivatedFn>(
                addresses.commonUIWidgetNativeOnActivated);
        g_nativeApi.commonUI.nativeOnDeactivated =
            reinterpret_cast<CommonUIApi::NativeOnDeactivatedFn>(
                addresses.commonUIWidgetNativeOnDeactivated);

        g_nativeApi.player.getFirstPlayerController =
            reinterpret_cast<PlayerApi::GetFirstPlayerControllerFn>(
                addresses.getFirstPlayerController);

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
#if defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
                addresses.readbackFree);
#else
                addresses.memoryFree);
#endif

        // Optional grid/collector addresses: missing resolutions disable the
        // dependent stage without disabling Stage A or the established MiniMap.
        g_nativeApi.gridDiagnostic.getSubsystem = reinterpret_cast<GridDiagnosticApi::GetSubsystemFn>(addresses.getEntityGridSubsystem);
        g_nativeApi.gridDiagnostic.forEachCellInRadius = reinterpret_cast<GridDiagnosticApi::ForEachCellInRadiusFn>(addresses.gridForEachCellInRadius);
        g_nativeApi.mass.isEntityValid = reinterpret_cast<MassApi::EntityCheckFn>(addresses.isMassEntityValid);
        g_nativeApi.mass.isEntityBuilt = reinterpret_cast<MassApi::EntityCheckFn>(addresses.isMassEntityBuilt);

#if defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
        g_nativeApi.asset.objectArray = reinterpret_cast<const void*>(addresses.objectArray);
        g_nativeApi.player.getSkeletalMeshAsset = reinterpret_cast<decltype(g_nativeApi.player.getSkeletalMeshAsset)>(addresses.getSkeletalMeshAsset);
        g_nativeApi.asset.fromValidEName = reinterpret_cast<decltype(g_nativeApi.asset.fromValidEName)>(addresses.readbackFromValidEName);
        g_nativeApi.texture.submitRenderCommand = reinterpret_cast<decltype(g_nativeApi.texture.submitRenderCommand)>(addresses.readbackSubmit);
        g_nativeApi.texture.immediateCommandList = reinterpret_cast<decltype(g_nativeApi.texture.immediateCommandList)>(addresses.readbackImmediate);
        g_nativeApi.texture.createRHITexture = reinterpret_cast<decltype(g_nativeApi.texture.createRHITexture)>(addresses.readbackCreate);
        g_nativeApi.texture.readbackConstruct = reinterpret_cast<decltype(g_nativeApi.texture.readbackConstruct)>(addresses.readbackConstruct);
        g_nativeApi.texture.readbackDestroy = reinterpret_cast<decltype(g_nativeApi.texture.readbackDestroy)>(addresses.readbackDestroy);
        g_nativeApi.texture.readbackCopy = reinterpret_cast<decltype(g_nativeApi.texture.readbackCopy)>(addresses.readbackCopy);
        g_nativeApi.texture.readbackLock = reinterpret_cast<decltype(g_nativeApi.texture.readbackLock)>(addresses.readbackLock);
        g_nativeApi.texture.readbackUnlock = reinterpret_cast<decltype(g_nativeApi.texture.readbackUnlock)>(addresses.readbackUnlock);
        g_nativeApi.texture.pollGPUFence = reinterpret_cast<decltype(g_nativeApi.texture.pollGPUFence)>(addresses.readbackPoll);
        g_nativeApi.texture.markRHIResourceForDelete = reinterpret_cast<decltype(g_nativeApi.texture.markRHIResourceForDelete)>(addresses.readbackMarkForDelete);
        g_nativeApi.asset.memoryMalloc = reinterpret_cast<decltype(g_nativeApi.asset.memoryMalloc)>(addresses.readbackMalloc);
        g_nativeApi.texture.dynamicRHI = reinterpret_cast<decltype(g_nativeApi.texture.dynamicRHI)>(addresses.readbackDynamicRHI);
        g_nativeApi.texture.renderCommandPipe = reinterpret_cast<decltype(g_nativeApi.texture.renderCommandPipe)>(addresses.readbackPipe);
        g_nativeApi.texture.threadedRendering = reinterpret_cast<decltype(g_nativeApi.texture.threadedRendering)>(addresses.readbackThreaded);
        g_nativeApi.texture.supportsRHIMultithreading = reinterpret_cast<decltype(g_nativeApi.texture.supportsRHIMultithreading)>(addresses.readbackMultithreaded);
        g_nativeApi.texture.explicitGpuCount = reinterpret_cast<decltype(g_nativeApi.texture.explicitGpuCount)>(addresses.readbackGpuCount);
        g_nativeApi.asset.activeGarbageCollection = reinterpret_cast<decltype(g_nativeApi.asset.activeGarbageCollection)>(addresses.activeGarbageCollection);
        g_nativeApi.player.engineStorage = reinterpret_cast<decltype(g_nativeApi.player.engineStorage)>(addresses.engineStorage);
        g_nativeApi.asset.exitPurge = reinterpret_cast<decltype(g_nativeApi.asset.exitPurge)>(addresses.exitPurge);
        g_nativeApi.world.createWorld = reinterpret_cast<decltype(g_nativeApi.world.createWorld)>(addresses.createWorld);
        g_nativeApi.world.initWorld = reinterpret_cast<decltype(g_nativeApi.world.initWorld)>(addresses.initWorld);
        g_nativeApi.world.updateWorldComponents = reinterpret_cast<decltype(g_nativeApi.world.updateWorldComponents)>(addresses.updateWorldComponents);
        g_nativeApi.world.cleanupWorld = reinterpret_cast<decltype(g_nativeApi.world.cleanupWorld)>(addresses.cleanupWorld);
        g_nativeApi.world.getWorldContextFromWorld = reinterpret_cast<decltype(g_nativeApi.world.getWorldContextFromWorld)>(addresses.getWorldContextFromWorld);
        g_nativeApi.world.getSubsystemInternal = reinterpret_cast<decltype(g_nativeApi.world.getSubsystemInternal)>(addresses.getSubsystemInternal);
        g_nativeApi.world.isGarbageCollectingAndLocking = reinterpret_cast<decltype(g_nativeApi.world.isGarbageCollectingAndLocking)>(addresses.isGarbageCollectingAndLocking);
        g_nativeApi.world.isIncrementalPurgePending = reinterpret_cast<decltype(g_nativeApi.world.isIncrementalPurgePending)>(addresses.isIncrementalPurgePending);
        g_nativeApi.world.isIncrementalReachabilityPending = reinterpret_cast<decltype(g_nativeApi.world.isIncrementalReachabilityPending)>(addresses.isIncrementalReachabilityPending);
        g_nativeApi.world.getTickableTickType = reinterpret_cast<decltype(g_nativeApi.world.getTickableTickType)>(addresses.getTickableTickType);
        g_nativeApi.world.isVisibilityRequestPending = reinterpret_cast<decltype(g_nativeApi.world.isVisibilityRequestPending)>(addresses.isVisibilityRequestPending);
        g_nativeApi.world.destroyActor = reinterpret_cast<decltype(g_nativeApi.world.destroyActor)>(addresses.destroyActor);
        g_nativeApi.asset.spawnObject = reinterpret_cast<decltype(g_nativeApi.asset.spawnObject)>(addresses.spawnObject);
        g_nativeApi.asset.setObjectFlags = reinterpret_cast<decltype(g_nativeApi.asset.setObjectFlags)>(addresses.setObjectFlags);
        g_nativeApi.player.registerComponent = reinterpret_cast<decltype(g_nativeApi.player.registerComponent)>(addresses.registerComponent);
        g_nativeApi.player.unregisterComponent = reinterpret_cast<decltype(g_nativeApi.player.unregisterComponent)>(addresses.unregisterComponent);
        g_nativeApi.player.destroyComponent = reinterpret_cast<decltype(g_nativeApi.player.destroyComponent)>(addresses.destroyComponent);
        g_nativeApi.texture.initRenderTarget = reinterpret_cast<decltype(g_nativeApi.texture.initRenderTarget)>(addresses.initRenderTarget);
        g_nativeApi.texture.getRenderTargetResource = reinterpret_cast<decltype(g_nativeApi.texture.getRenderTargetResource)>(addresses.getRenderTargetResource);
        g_nativeApi.texture.transitionResources = reinterpret_cast<decltype(g_nativeApi.texture.transitionResources)>(addresses.transitionResources);
        g_nativeApi.texture.renderFenceConstruct = reinterpret_cast<decltype(g_nativeApi.texture.renderFenceConstruct)>(addresses.renderFenceConstruct);
        g_nativeApi.texture.renderFenceDestroy = reinterpret_cast<decltype(g_nativeApi.texture.renderFenceDestroy)>(addresses.renderFenceDestroy);
        g_nativeApi.texture.renderFenceBegin = reinterpret_cast<decltype(g_nativeApi.texture.renderFenceBegin)>(addresses.renderFenceBegin);
        g_nativeApi.texture.renderFencePoll = reinterpret_cast<decltype(g_nativeApi.texture.renderFencePoll)>(addresses.renderFencePoll);
#endif

        g_nativeApi.asset.retainObject = reinterpret_cast<decltype(g_nativeApi.asset.retainObject)>(addresses.retainObject);
        g_nativeApi.asset.releaseObject = reinterpret_cast<decltype(g_nativeApi.asset.releaseObject)>(addresses.releaseObject);
        g_nativeApi.asset.nameEqualsWide = reinterpret_cast<decltype(g_nativeApi.asset.nameEqualsWide)>(addresses.nameEqualsWide);
        g_nativeApi.player.isGameThread = reinterpret_cast<PlayerApi::IsGameThreadFn>(addresses.isGameThread);
        g_nativeApi.splinePayload.get = reinterpret_cast<SplinePayloadApi::GetFn>(addresses.getSplineActorData);
        g_nativeApi.splinePayload.destroy = reinterpret_cast<SplinePayloadApi::DestroyFn>(addresses.destroySplineActorPayloadData);
        // This populates the same static type cache used by the native typed getter.
        // No script-struct discovery belongs in the gameplay numerical adapter.
        g_nativeApi.splinePayload.payloadType =
            reinterpret_cast<MassApi::StaticStructFn>(addresses.splineActorPayloadStaticStruct)();
        if (!g_nativeApi.splinePayload.payloadType) return false;

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
