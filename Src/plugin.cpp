#include "CaptureTiming.h"
#include "CallbackTiming.h"
#include "plugin.h"
#include "plugin_helpers.h"
#include "Config/Config.h"

#ifdef MODLOADER_CLIENT_BUILD
#include "Native/Fingerprints.h"
#include "Native/NativeApi.h"
#include "Native/ReadbackNativeAdapter.h"
#include "UI/MiniMapUI.h"
#include "Input/MouseWheel.h"
#include "SDK/CommonUI_classes.hpp"
#include "SDK/ChimeraUI_classes.hpp"
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
static HookHandle g_commonUIActivatedHook = nullptr;
static HookHandle g_commonUIDeactivatedHook = nullptr;
static MiniMapNative::CommonUIApi::NativeOnActivatedFn g_commonUIActivatedOriginal = nullptr;
static MiniMapNative::CommonUIApi::NativeOnDeactivatedFn g_commonUIDeactivatedOriginal = nullptr;

static constexpr uint32_t kBlockingMenuMainMap = 1u << 0;
static constexpr uint32_t kBlockingMenuInventory = 1u << 1;
static constexpr uint32_t kBlockingMenuBuilding = 1u << 2;
static constexpr uint32_t kBlockingMenuCorporateTerminal = 1u << 3;
static constexpr uint32_t kBlockingMenuDevelopmentStation = 1u << 4;
static constexpr uint32_t kBlockingMenuEquipmentUpgradeStation = 1u << 5;
static constexpr uint32_t kBlockingMenuPauseMenu = 1u << 6;

static uint32_t GetBlockingMenuBit(
    SDK::UCommonActivatableWidget* widget,
    const char*& menuName)
{
    if (widget->IsA(SDK::UCrUW_MapMenu::StaticClass()))
    {
        menuName = "main map";
        return kBlockingMenuMainMap;
    }
    if (widget->IsA(SDK::UCrUW_InventoryScreen::StaticClass()))
    {
        menuName = "inventory";
        return kBlockingMenuInventory;
    }
    if (widget->IsA(SDK::UCrUW_BuildingMenu::StaticClass()))
    {
        menuName = "building menu";
        return kBlockingMenuBuilding;
    }
    if (widget->IsA(SDK::UCrUW_ResearchTerminalPanel::StaticClass()))
    {
        menuName = "corporate terminal";
        return kBlockingMenuCorporateTerminal;
    }
    if (widget->IsA(SDK::UCrUW_UpgradeStationPanel::StaticClass()))
    {
        menuName = "development station";
        return kBlockingMenuDevelopmentStation;
    }
    if (widget->IsA(SDK::UCrUW_ArmoryPanel::StaticClass()))
    {
        menuName = "equipment upgrade station";
        return kBlockingMenuEquipmentUpgradeStation;
    }
    if (widget->IsA(SDK::UCrUW_PauseMenu::StaticClass()))
    {
        menuName = "pause menu";
        return kBlockingMenuPauseMenu;
    }

    menuName = nullptr;
    return 0;
}

static void LogCommonUITransition(
    const char* transition,
    const char* menuName,
    uint32_t suppressionMask)
{
    LOG_INFO(
        "MiniMap: %s blocking menu=%s suppressionMask=0x%X",
        transition,
        menuName,
        suppressionMask);
}

static void __fastcall CommonUIActivatedDetour(
    SDK::UCommonActivatableWidget* widget)
{
    g_commonUIActivatedOriginal(widget);

    const char* menuName = nullptr;
    const uint32_t menuBit = GetBlockingMenuBit(widget, menuName);
    if (menuBit != 0)
    {
        const uint32_t suppressionMask = MiniMapUI::SetBlockingMenuActive(menuBit, true);
        LogCommonUITransition("Activated", menuName, suppressionMask);
    }
}

static void __fastcall CommonUIDeactivatedDetour(
    SDK::UCommonActivatableWidget* widget)
{
    g_commonUIDeactivatedOriginal(widget);

    const char* menuName = nullptr;
    const uint32_t menuBit = GetBlockingMenuBit(widget, menuName);
    if (menuBit != 0)
    {
        const uint32_t suppressionMask = MiniMapUI::SetBlockingMenuActive(menuBit, false);
        LogCommonUITransition("Deactivated", menuName, suppressionMask);
    }
}

static void RemoveCommonUIHooks()
{
    if (g_self != nullptr &&
        g_self->hooks != nullptr &&
        g_self->hooks->Hooks != nullptr &&
        g_self->hooks->Hooks->Remove != nullptr)
    {
        if (g_commonUIActivatedHook != nullptr)
        {
            g_self->hooks->Hooks->Remove(g_commonUIActivatedHook);
        }
        if (g_commonUIDeactivatedHook != nullptr)
        {
            g_self->hooks->Hooks->Remove(g_commonUIDeactivatedHook);
        }
    }

    g_commonUIActivatedHook = nullptr;
    g_commonUIDeactivatedHook = nullptr;
    g_commonUIActivatedOriginal = nullptr;
    g_commonUIDeactivatedOriginal = nullptr;
}

static bool InstallCommonUIHooks()
{
    auto* hookUtils = g_self != nullptr && g_self->hooks != nullptr
        ? g_self->hooks->Hooks
        : nullptr;
    const auto* nativeApi = MiniMapNative::Get();
    if (hookUtils == nullptr || hookUtils->Install == nullptr ||
        hookUtils->Remove == nullptr || nativeApi == nullptr ||
        nativeApi->commonUI.nativeOnActivated == nullptr ||
        nativeApi->commonUI.nativeOnDeactivated == nullptr)
    {
        LOG_ERROR("MiniMap: CommonUI lifecycle hook dependencies are unavailable");
        return false;
    }

    void* activatedOriginal = nullptr;
    g_commonUIActivatedHook = hookUtils->Install(
        reinterpret_cast<uintptr_t>(nativeApi->commonUI.nativeOnActivated),
        reinterpret_cast<void*>(&CommonUIActivatedDetour),
        &activatedOriginal);
    if (g_commonUIActivatedHook == nullptr || activatedOriginal == nullptr)
    {
        RemoveCommonUIHooks();
        LOG_ERROR("MiniMap: CommonUI activation detour installation failed");
        return false;
    }
    g_commonUIActivatedOriginal =
        reinterpret_cast<MiniMapNative::CommonUIApi::NativeOnActivatedFn>(
            activatedOriginal);

    void* deactivatedOriginal = nullptr;
    g_commonUIDeactivatedHook = hookUtils->Install(
        reinterpret_cast<uintptr_t>(nativeApi->commonUI.nativeOnDeactivated),
        reinterpret_cast<void*>(&CommonUIDeactivatedDetour),
        &deactivatedOriginal);
    if (g_commonUIDeactivatedHook == nullptr || deactivatedOriginal == nullptr)
    {
        RemoveCommonUIHooks();
        LOG_ERROR("MiniMap: CommonUI deactivation detour installation failed");
        return false;
    }
    g_commonUIDeactivatedOriginal =
        reinterpret_cast<MiniMapNative::CommonUIApi::NativeOnDeactivatedFn>(
            deactivatedOriginal);

    LOG_INFO("MiniMap: installed both CommonUI activation/deactivation detours");
    return true;
}
#endif

#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "0.1.0"
#endif

#ifdef MODLOADER_CLIENT_BUILD
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
    if (!terrainCleared || !buildingsCleared)
        LOG_ERROR("MiniMap: persistent cache clearing failed (terrain=%d, buildings=%d)", terrainCleared, buildingsCleared);
    return terrainCleared && buildingsCleared;
}

void MiniMapCacheIdentity::ClearOnExit()
{
    // Never acquire Config while holding a cache/subsystem lock. No native
    // services or subsystem teardown are needed to drain CPU file operations.
    if (!MiniMapConfig::ClearCache()) return;
    std::unique_lock access(g_fileAccessMutex);
    if (g_exitClearAttempted) return;
    g_fileAccessClosed = true;
    Reset();
    g_exitClearAttempted = true;
    if (ClearPersistentCaches())
        LOG_INFO("MiniMap: exit persistent caches cleared (file access closed and drained)");
    else
        LOG_ERROR("MiniMap: exit persistent cache clear failed; file access remains closed");
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

    // Optional private preflight: failure never changes production readiness.
    MiniMapReadbackNative::ResolvePrerequisites();

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
    if (MiniMapConfig::VisibleAtStartup()) MiniMapUI::Show();
    else MiniMapUI::Hide();
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
    const uint32_t blockingMenuMask = MiniMapUI::ClearBlockingMenuSuppression();
    if (blockingMenuMask != 0)
    {
        LOG_INFO(
            "MiniMap: cleared blocking menu mask at world end (0x%X)",
            blockingMenuMask);
    }
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

        // Unwind the listener on every later initialization failure while self is valid.
        struct ConfigInitGuard
        {
            bool Keep = false;
            ~ConfigInitGuard() { if (!Keep) { MiniMapConfig::Shutdown(); g_self = nullptr; } }
        } configGuard;

#ifdef MODLOADER_CLIENT_BUILD

        if (g_self->hooks->World == nullptr || g_self->hooks->Engine == nullptr ||
            !g_self->hooks->Engine->RegisterOnTick || !g_self->hooks->Engine->UnregisterOnTick ||
            !g_self->hooks->World->RegisterOnBeforeWorldEndPlay || !g_self->hooks->World->UnregisterOnBeforeWorldEndPlay)
        {
            LOG_ERROR(
                "MiniMap: World hooks are unavailable");

            return false;
        }

        if(!MiniMapNative::IsInitialized())
        {
            LOG_ERROR("MiniMap: native API must be initialized before building capture services");
            return false;
        }

        if (!InstallCommonUIHooks())
        {
            return false;
        }

        std::filesystem::path terrainRoot;
        const char* cacheFailure = nullptr;
        if (!MiniMapTerrainCache::GetCacheRoot(terrainRoot) ||
            !MiniMapCacheIdentity::Initialize(terrainRoot.parent_path(),
                &ClearPersistentCaches, cacheFailure, MiniMapConfig::ClearCache()))
        {
            LOG_ERROR("MiniMap: persistent cache initialization failed: %s",
                cacheFailure ? cacheFailure : "cache root unavailable");
            RemoveCommonUIHooks();
            return false;
        }

        if (!MiniMapUI::Initialize(
            g_self))
        {
            LOG_ERROR(
                "MiniMap: UI initialization failed");

            MiniMapCacheIdentity::Reset();
            RemoveCommonUIHooks();

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
            MiniMapUI::Shutdown();
            RemoveCommonUIHooks();
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

        // Registration replays events only during loader startup, never runtime reload.
        // Prepare every owner first; establish world identity before experience completion.
        CapturePluginInitHookBaseline(g_self);
        g_self->hooks->World->RegisterOnAfterWorldEndPlay(&OnAfterWorldEndPlay);
        g_self->hooks->World->RegisterOnWorldBeginPlay(&OnWorldBeginPlay);
        g_self->hooks->World->RegisterOnExperienceLoadComplete(&OnExperienceLoadComplete);
        LOG_INFO("MiniMap: registered world lifecycle callbacks");
        // Public v70 exposes no current-world experience-completion snapshot.
        // A viewport world or CurrentExperience pointer cannot prove readiness.
        // Do not synthesize callbacks or publish a generation on runtime reload.
        if (!MiniMapMap::HasWorld())
            LOG_INFO("MiniMap: no authoritative gameplay world received; public v70 cannot verify active-world experience readiness (runtime hot-load/reload attachment unavailable)");

#endif

        LOG_INFO(
            "MiniMap: initialization complete");

        configGuard.Keep = true;
        return true;
    }

    __declspec(dllexport)
        void PluginShutdown()
    {
        MiniMapCallbackTiming::Scope callbackTiming(MiniMapCallbackTiming::Id::PluginShutdown,false);
        LOG_INFO(
            "MiniMap: shutting down");

#ifdef MODLOADER_CLIENT_BUILD

        MiniMapUI::StopInput();
        MiniMapConfig::Shutdown();
        RemoveCommonUIHooks();
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
        const uint32_t blockingMenuMask = MiniMapUI::ClearBlockingMenuSuppression();
        if (blockingMenuMask != 0)
        {
            LOG_INFO("MiniMap: cleared blocking menu mask during shutdown (0x%X)",
                blockingMenuMask);
        }
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

        MiniMapCacheIdentity::ClearOnExit();

        // NativeApi should be the last subsystem torn down.
        // Other subsystems depend on it for their own shutdown paths.
        MiniMapNative::Shutdown();
#endif

        MiniMapConfig::Shutdown();
        ResetPluginInitHookBaseline();
        g_self = nullptr;
    }
}
