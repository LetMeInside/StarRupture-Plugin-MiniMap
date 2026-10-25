#include "CaptureTiming.h"
#include "CallbackTiming.h"
#include "plugin.h"
#include "plugin_helpers.h"
#include "Config/Config.h"

#ifdef MODLOADER_CLIENT_BUILD
#include "Native/Fingerprints.h"
#include "Native/NativeApi.h"
#include "Native/ReadbackNativeAdapter.h"
#include "Map/Buildings/BuildingCache.h"
#include "Map/Buildings/BuildingScheduler.h"
#include "Map/Buildings/BuildingAtlas.h"
#include "Map/Buildings/BuildingSpriteRenderer.h"
#endif

IPluginSelf* g_self = nullptr;
static MiniMapPluginInitHookBaseline g_pluginInitHookBaseline;
MiniMapPluginInitHookBaseline GetPluginInitHookBaseline() { return g_pluginInitHookBaseline; }
static void ResetPluginInitHookBaseline() { g_pluginInitHookBaseline = {}; }
static void CapturePluginInitHookBaseline(IPluginSelf* self)
{
    auto* hooks=self?self->hooks:nullptr;
    g_pluginInitHookBaseline={true,self,hooks,hooks?hooks->Engine:nullptr,hooks?hooks->ImGuiTextures:nullptr};
}

#ifdef MODLOADER_CLIENT_BUILD
static MiniMapFingerprints::ResolvedAddresses g_resolvedAddresses = {};
static MiniMapSpriteStatus::Dictionary g_buildingSpriteStatuses;
static MiniMapSpriteQueues::SpriteFifo g_buildingSpriteFifo;
#endif

#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "0.1.0"
#endif

#ifdef MODLOADER_CLIENT_BUILD
#include "UI/MiniMapUI.h"
#include "Map/FogOfWar/FogOfWar.h"
#include "Map/Foundables/Foundables.h"
#include "Map/AlienActors/AlienActors.h"
#include "Map/Buildings/BuildingCollector.h"
#include "Map/Buildings/BuildingVisuals.h"
#include "Map/PointsOfInterest/PointsOfInterest.h"
#include "Map/Terrain/Terrain.h"
#include "Map/Terrain/TerrainCache.h"
#include "CacheIdentity.h"
#include "Map/Map.h"
#include <cstdint>
#endif

#ifdef MODLOADER_CLIENT_BUILD
static bool ClearPersistentCaches()
{
    const bool terrainCleared = MiniMapTerrainCache::Clear();
    const bool buildingsCleared = MiniMapBuildingCache::Clear();
    return terrainCleared && buildingsCleared;
}
#endif

#ifdef MODLOADER_SERVER_BUILD
#define MINIMAP_EXPORTS_TARGET PLUGIN_TARGET_SERVER
#else
#define MINIMAP_EXPORTS_TARGET PLUGIN_TARGET_CLIENT
#endif


static PluginInfo s_pluginInfo = {
    "MiniMap",
    MODLOADER_BUILD_TAG,
    "Kian369",
    "A configurable on-screen minimap for StarRupture",
    PLUGIN_INTERFACE_VERSION,
    MINIMAP_EXPORTS_TARGET
};

extern "C" __declspec(dllexport)
void OnPluginLoadHooks(
    IPluginSelf* self,
    IPluginHookScanner* scanner)
{
        MiniMapCallbackTiming::Scope callbackTiming(MiniMapCallbackTiming::Id::PluginLoadHooks,false,self);
#ifdef MODLOADER_CLIENT_BUILD

    g_resolvedAddresses = {};

    const bool fingerprintsResolved=MiniMapFingerprints::Resolve(
        self,
        scanner,
        g_resolvedAddresses);
    MiniMapCallbackTiming::ReportDirectQueries(self,fingerprintsResolved?
        "mandatory-fingerprint-resolution-complete":"mandatory-fingerprint-resolution-refused");
    if (!fingerprintsResolved)
    {
        LOG_ERROR(
            "MiniMap: native fingerprint resolution failed");

        MiniMapNative::Shutdown();
        return;
    }

    if (!MiniMapNative::Initialize(
        g_resolvedAddresses))
    {
        LOG_ERROR(
            "MiniMap: native API initialization failed");

        MiniMapNative::Shutdown();
        return;
    }

    LOG_INFO(
        "MiniMap: native API initialized");

#if defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
    // Optional private preflight: failure never changes production readiness.
    MiniMapReadbackNative::ResolvePrerequisites();
#endif

#else

    (void)self;
    (void)scanner;

#endif
}

#ifdef MODLOADER_CLIENT_BUILD

// Admission belongs to an owned gameplay-world generation, not loading UI state.
static uint64_t g_worldGeneration=0,g_preparedGeneration=0;
static bool g_worldEnding=false;
static void SetServicesAdmitted(bool ready)
{
    const bool changed=MiniMapMap::GameplayReady()!=ready;
    MiniMapMap::SetGameplayReady(ready);
    if(changed)MiniMapUI::RefreshVisibility();
}
static void EndBuildingSession()
{
    MiniMapNative::ScopedQueryPhase queryPhase(MiniMapNative::QueryPhase::WorldCleanup);
    {
        std::lock_guard<std::mutex> lock(MiniMapBuildingSchedulerInternal::LifecycleMutex);
        MiniMapBuildingSchedulerInternal::StopSession();
        MiniMapBuildingSpriteRenderer::EndSession();
    }
    // Withdraw under a short internal lock; FreeTexture runs after that lock ends.
    MiniMapBuildingAtlas::EndSession();
    {
        std::lock_guard<std::mutex> lock(MiniMapBuildingSchedulerInternal::LifecycleMutex);
        MiniMapBuildingScheduler::EndSession();
    }
    MiniMapCaptureTiming::Flush();
}
static bool BuildingServiceGameThread() noexcept
{
    const auto* api = MiniMapNative::Get();
    __try { return api && api->player.isGameThread && api->player.isGameThread(); }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
}
static void OnBuildingSpriteRendererTick(float deltaTime)
{
    MiniMapCallbackTiming::Scope callbackTiming(MiniMapCallbackTiming::Id::BuildingCaptureService,true);
    if(!MiniMapMap::GameplayReady())return;
    // Integration lock is outermost; FIFO/feedback locks are acquired only inside
    // it. Renderer functions never reacquire it or invoke scheduler transitions.
    {
        std::lock_guard<std::mutex> lock(MiniMapBuildingSchedulerInternal::LifecycleMutex);
        if(MiniMapMap::GameplayReady())MiniMapBuildingSpriteRenderer::Tick(deltaTime);
    }
    // Native progress has priority. Process the entire last completed drawing
    // batch on this existing GT callback, never while holding LifecycleMutex.
    if (MiniMapMap::GameplayReady() && MiniMapUI::IsVisible() && BuildingServiceGameThread()) MiniMapBuildingScheduler::Service();
    MiniMapCaptureTiming::Flush(); // All lifecycle/backend locks have been released.
    // Observation only: Complete describes available scoped observation, not
    // worldwide discovery or capture completion. Never a service readiness gate.
    if(MiniMapUI::IsVisible()&&MiniMapCallbackTiming::QueryInitialAcquisition.load(std::memory_order_relaxed))
    {
        const auto snapshot=MiniMapBuildingCollector::GetSnapshot();
        if(snapshot && snapshot->Complete)
        {
            MiniMapCallbackTiming::QueryInitialAcquisition.store(false,std::memory_order_relaxed);
            MiniMapCallbackTiming::ReportDirectQueries(GetSelf(),"first-observed-complete-collector-publication-not-capture-completion");
        }
    }
}
static void OnWorldBeginPlay(SDK::UWorld* world)
{
        MiniMapCallbackTiming::Scope callbackTiming(MiniMapCallbackTiming::Id::WorldBegin,false);
    if(!world||(world==MiniMapMap::GetWorld()&&!g_worldEnding))return;
    MiniMapCallbackTiming::StartWorldTrace();
    ++g_worldGeneration;
    g_preparedGeneration=0;
    g_worldEnding=false;
    SetServicesAdmitted(false);
    // A replacement world can begin before the owned world's EndPlay. Keep the
    // previous gameplay identity available until its native owner has ended.
    EndBuildingSession();
    MiniMapUI::Hide();
    MiniMapMap::SetWorld(world);
    MiniMapFogOfWar::Reset();
    MiniMapFoundables::Reset();
    MiniMapAlienActors::Reset();
    MiniMapBuildingCollector::Reset();
    MiniMapBuildingVisuals::Reset();
    MiniMapPointsOfInterest::Reset();
    MiniMapTerrain::CancelPendingDiagnostic();

    LOG_INFO(
        "MiniMap: game world began: %p",
        world);

}

static void OnExperienceLoadComplete()
{
    MiniMapCallbackTiming::Scope callbackTiming(MiniMapCallbackTiming::Id::ExperienceComplete,false);
    if(!MiniMapMap::HasWorld()||g_worldEnding||g_preparedGeneration==g_worldGeneration)return;
    // Mark the attempt before preparation so duplicate events cannot repeat it.
    g_preparedGeneration=g_worldGeneration;
    LOG_INFO("MiniMap: preparing gameplay services for generation %llu",static_cast<unsigned long long>(g_worldGeneration));

    // Preparation only registers the terrain service and resets CPU state.
    // Player/asset/Mass/capture prerequisites remain local to scheduled services.
    if(!MiniMapTerrain::Initialize(g_self))
    {
        LOG_ERROR("MiniMap: terrain service preparation failed; generation remains unadmitted");
        SetServicesAdmitted(false);
        EndBuildingSession();
        return;
    }

    MiniMapFoundables::OnExperienceLoadComplete();
    MiniMapAlienActors::OnExperienceLoadComplete();
    MiniMapBuildingCollector::OnExperienceLoadComplete();
    MiniMapPointsOfInterest::OnExperienceLoadComplete();

    bool rendererReady=false;
    uint64_t buildingSession=0;
    {
        std::lock_guard<std::mutex> lock(MiniMapBuildingSchedulerInternal::LifecycleMutex);
        MiniMapBuildingScheduler::InitializeSession();
        buildingSession=MiniMapBuildingSchedulerInternal::ActiveSession();
    }
    const bool atlasReady=MiniMapBuildingAtlas::InitializeSession(buildingSession);
    {
        std::lock_guard<std::mutex> lock(MiniMapBuildingSchedulerInternal::LifecycleMutex);
        rendererReady=atlasReady && MiniMapBuildingSchedulerInternal::ActiveSession()==buildingSession &&
            MiniMapBuildingSpriteRenderer::InitializeSession();
    }
    if(!rendererReady)
    {
        EndBuildingSession();
        LOG_ERROR("MiniMap: building renderer/atlas session blocked by retained ownership or missing dependencies; gameplay remains unadmitted");
        return;
    }
    SetServicesAdmitted(true);
    MiniMapUI::Show();
    LOG_INFO("MiniMap: experience complete; gameplay services admitted for generation %llu",static_cast<unsigned long long>(g_worldGeneration));
}

static void OnBeforeWorldEndPlay(SDK::UWorld* world,const char*)
{
    MiniMapCallbackTiming::Scope callbackTiming(MiniMapCallbackTiming::Id::WorldBeforeEnd,false);
    if(!world||world!=MiniMapMap::GetWorld())return;
    g_worldEnding=true;
    SetServicesAdmitted(false);
    EndBuildingSession();
    MiniMapUI::Hide();
    MiniMapTerrain::CancelPendingDiagnostic();
}

static void OnAfterWorldEndPlay(
    SDK::UWorld* world,
    const char* worldName)
{
        MiniMapCallbackTiming::Scope callbackTiming(MiniMapCallbackTiming::Id::WorldEnd,false);
    if(!world||world!=MiniMapMap::GetWorld())return;
    LOG_INFO(
        "MiniMap: world ended: %s",
        worldName != nullptr ? worldName : "<null>");

    g_worldEnding=true;
    SetServicesAdmitted(false);
    EndBuildingSession(); // Idempotent fallback if BeforeEndPlay was not delivered.
    g_preparedGeneration=0;
    MiniMapUI::Hide();

    MiniMapFoundables::Reset();
    MiniMapAlienActors::Reset();
    MiniMapBuildingCollector::Reset();
    MiniMapBuildingVisuals::Reset();
    MiniMapPointsOfInterest::Reset();
    MiniMapFogOfWar::Reset();
    MiniMapMap::SetWorld(nullptr);
    MiniMapTerrain::Shutdown();
}

#endif

extern "C"
{
    __declspec(dllexport)
        PluginInfo* GetPluginInfo()
    {
        MiniMapCallbackTiming::Scope callbackTiming(MiniMapCallbackTiming::Id::PluginInfo,false);
        return &s_pluginInfo;
    }

    __declspec(dllexport)
        bool PluginInit(IPluginSelf* self)
    {
        MiniMapCallbackTiming::Scope callbackTiming(MiniMapCallbackTiming::Id::PluginInit,false,self);
        ResetPluginInitHookBaseline();
#ifdef MODLOADER_CLIENT_BUILD
        MiniMapCacheIdentity::Reset();
#endif
        g_self = self;

        if (g_self == nullptr ||
            g_self->hooks == nullptr)
        {
            g_self = nullptr;
            return false;
        }

        if (!MiniMapConfig::Initialize(
            g_self))
        {
            LOG_ERROR(
                "MiniMap: configuration initialization failed");

            g_self = nullptr;
            return false;
        }

#ifdef MODLOADER_CLIENT_BUILD

        if (g_self->hooks->World == nullptr || g_self->hooks->Engine == nullptr ||
            !g_self->hooks->Engine->RegisterOnTick || !g_self->hooks->Engine->UnregisterOnTick ||
            !g_self->hooks->World->RegisterOnBeforeWorldEndPlay || !g_self->hooks->World->UnregisterOnBeforeWorldEndPlay)
        {
            LOG_ERROR(
                "MiniMap: World hooks are unavailable");

            g_self = nullptr;
            return false;
        }

        if(!MiniMapNative::IsInitialized())
        {
            LOG_ERROR("MiniMap: native API must be initialized before building capture services");
            g_self=nullptr;
            return false;
        }

        std::filesystem::path terrainRoot;
        const char* cacheFailure = nullptr;
        if (!MiniMapTerrainCache::GetCacheRoot(terrainRoot) ||
            !MiniMapCacheIdentity::Initialize(terrainRoot.parent_path(),
                &ClearPersistentCaches, cacheFailure))
        {
            LOG_ERROR("MiniMap: persistent cache initialization failed: %s",
                cacheFailure ? cacheFailure : "cache root unavailable");
            g_self = nullptr;
            return false;
        }

        if (!MiniMapUI::Initialize(
            g_self))
        {
            LOG_ERROR(
                "MiniMap: UI initialization failed");

            MiniMapCacheIdentity::Reset();

            g_self = nullptr;
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(MiniMapBuildingSchedulerInternal::LifecycleMutex);
            MiniMapBuildingScheduler::Initialize(g_buildingSpriteStatuses, g_buildingSpriteFifo);
            MiniMapBuildingSpriteRenderer::Initialize(g_buildingSpriteFifo, MiniMapBuildingScheduler::FeedbackQueue());
        }
        if (!MiniMapBuildingAtlas::Initialize(g_buildingSpriteStatuses, g_self->hooks->ImGuiTextures))
        {
            LOG_ERROR("MiniMap: atlas texture interface initialization failed");
            return false;
        }
        g_self->hooks->Engine->RegisterOnTick(&OnBuildingSpriteRendererTick);

        // Close admission before module-specific native EndPlay cancellation.
        g_self->hooks->World->RegisterOnBeforeWorldEndPlay(&OnBeforeWorldEndPlay);

        if (!MiniMapFogOfWar::Initialize(
            g_self))
        {
            LOG_ERROR(
                "MiniMap: FOW diagnostic initialization failed");
        }

        if (!MiniMapPointsOfInterest::Initialize(
            g_self))
        {
            LOG_ERROR(
                "MiniMap: landmark POI initialization failed");
        }

        if (!MiniMapAlienActors::Initialize(g_self))
        {
            LOG_ERROR("MiniMap: Alien Actors initialization failed");
        }

        if (!MiniMapBuildingCollector::Initialize(g_self))
        {
            LOG_ERROR("MiniMap: Stage B.1 building collector initialization failed");
        }

        if (!MiniMapBuildingVisuals::Initialize(g_self))
        {
            LOG_ERROR("MiniMap: building visuals initialization failed");
        }

        if (!MiniMapFoundables::Initialize(
            g_self))
        {
            LOG_ERROR(
                "MiniMap: Mass foundable initialization failed");
        }

        LOG_INFO(
            "MiniMap: FSoftObjectPtr::LoadSynchronous = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.softObjectLoadSynchronous));

        LOG_INFO(
            "MiniMap: UWidgetBlueprintLibrary::"
            "GetBrushResourceAsTexture2D = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.getBrushResourceAsTexture2D));

        LOG_INFO(
            "MiniMap: UStreamableRenderAsset::"
            "SetForceMipLevelsToBeResident = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.setForceMipLevelsToBeResident));

        LOG_INFO(
            "MiniMap: UStreamableRenderAsset::"
            "WaitForStreaming = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.waitForStreaming));

        LOG_INFO(
            "MiniMap: UTexture2D::"
            "GetNumResidentMips = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.getNumResidentMips));

        LOG_INFO(
            "MiniMap: UTexture2D::"
            "GetNumMipsAllowed = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.getNumMipsAllowed));

        LOG_INFO(
            "MiniMap: UTexture2D::"
            "GetNumMips = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.getNumMips));

        LOG_INFO(
            "MiniMap: UTexture2D::"
            "StreamIn = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.streamIn));

        LOG_INFO(
            "MiniMap: UStreamableRenderAsset::"
            "WaitForPendingInitOrStreaming = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.waitForPendingInitOrStreaming));

        LOG_INFO(
            "MiniMap: UWorld::"
            "GetFirstPlayerController = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.getFirstPlayerController));

        LOG_INFO(
            "MiniMap: AController::"
            "GetPawn<ACrCharacterPlayerBase> = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.getPlayerPawn));

        LOG_INFO(
            "MiniMap: AController::"
            "GetControlRotation = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.getControlRotation));

        LOG_INFO(
            "MiniMap: USceneComponent::"
            "K2_GetComponentLocation = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.getComponentLocation));

        LOG_INFO(
            "MiniMap: UTexture2D::"
            "GetPlatformData = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.getPlatformData));

        LOG_INFO(
            "MiniMap: FBulkData::"
            "GetBulkDataSize = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.getBulkDataSize));

        LOG_INFO(
            "MiniMap: FBulkData::"
            "CanLoadFromDisk = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.canLoadFromDisk));

        LOG_INFO(
            "MiniMap: FBulkData::"
            "GetCopy = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.getBulkDataCopy));

        LOG_INFO(
            "MiniMap: FMemory::"
            "Free = 0x%llX",
            static_cast<unsigned long long>(
                g_resolvedAddresses.memoryFree));

        if (!MiniMapTerrain::RegisterDiagnostics(g_self))
        {
            LOG_ERROR(
                "MiniMap: failed to register terrain diagnostics");
        }

        // Registration can replay startup events: prepare every owner first,
        // establish world identity before accepting experience completion.
        CapturePluginInitHookBaseline(g_self);
        g_self->hooks->World->RegisterOnAfterWorldEndPlay(&OnAfterWorldEndPlay);
        g_self->hooks->World->RegisterOnWorldBeginPlay(&OnWorldBeginPlay);
        g_self->hooks->World->RegisterOnExperienceLoadComplete(&OnExperienceLoadComplete);
        LOG_INFO("MiniMap: registered world lifecycle callbacks");

#endif

        LOG_INFO(
            "MiniMap: initialization complete");

        return true;
    }

    __declspec(dllexport)
        void PluginShutdown()
    {
        MiniMapCallbackTiming::Scope callbackTiming(MiniMapCallbackTiming::Id::PluginShutdown,false);
        LOG_INFO(
            "MiniMap: shutting down");

#ifdef MODLOADER_CLIENT_BUILD

        g_worldEnding=true;
        SetServicesAdmitted(false);
        {
            std::lock_guard<std::mutex> lock(MiniMapBuildingSchedulerInternal::LifecycleMutex);
            MiniMapBuildingSchedulerInternal::StopSession();
            MiniMapBuildingSpriteRenderer::Shutdown();
        }
        MiniMapBuildingAtlas::Shutdown();
        {
            std::lock_guard<std::mutex> lock(MiniMapBuildingSchedulerInternal::LifecycleMutex);
            MiniMapBuildingScheduler::Shutdown();
        }
        if(g_self && g_self->hooks && g_self->hooks->Engine)
            g_self->hooks->Engine->UnregisterOnTick(&OnBuildingSpriteRendererTick);
        MiniMapCacheIdentity::Reset();
        MiniMapUI::Hide();

        if (g_self != nullptr &&
            g_self->hooks != nullptr &&
            g_self->hooks->World != nullptr)
        {
            g_self->hooks->World->UnregisterOnBeforeWorldEndPlay(&OnBeforeWorldEndPlay);
            g_self->hooks->World->UnregisterOnExperienceLoadComplete(
                &OnExperienceLoadComplete);

            g_self->hooks->World->UnregisterOnWorldBeginPlay(
                &OnWorldBeginPlay);

            g_self->hooks->World->UnregisterOnAfterWorldEndPlay(
                &OnAfterWorldEndPlay);
        }

        MiniMapTerrain::UnregisterDiagnostics();
        MiniMapAlienActors::Shutdown();
        MiniMapBuildingVisuals::Shutdown();
        MiniMapBuildingCollector::Shutdown();
        MiniMapFoundables::Shutdown();
        MiniMapPointsOfInterest::Shutdown();
        MiniMapFogOfWar::Shutdown();
        MiniMapTerrain::Shutdown();
        MiniMapMap::Shutdown();
        MiniMapUI::Shutdown();

        // NativeApi should be the last subsystem torn down.
        // Other subsystems depend on it for their own shutdown paths.
        MiniMapNative::Shutdown();
#endif

        ResetPluginInitHookBaseline();
        g_self = nullptr;
    }
}
