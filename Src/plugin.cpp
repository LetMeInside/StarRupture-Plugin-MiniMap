#include "plugin.h"
#include "plugin_helpers.h"
#include "DiagnosticExports.h"
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
    SDK::UCommonActivatableWidget* widget
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
    , const char*& menuName
#endif
    )
{
    if (widget->IsA(SDK::UCrUW_MapMenu::StaticClass()))
    {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        menuName = "main map";
#endif
        return kBlockingMenuMainMap;
    }
    if (widget->IsA(SDK::UCrUW_InventoryScreen::StaticClass()))
    {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        menuName = "inventory";
#endif
        return kBlockingMenuInventory;
    }
    if (widget->IsA(SDK::UCrUW_BuildingMenu::StaticClass()))
    {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        menuName = "building menu";
#endif
        return kBlockingMenuBuilding;
    }
    if (widget->IsA(SDK::UCrUW_ResearchTerminalPanel::StaticClass()))
    {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        menuName = "corporate terminal";
#endif
        return kBlockingMenuCorporateTerminal;
    }
    if (widget->IsA(SDK::UCrUW_UpgradeStationPanel::StaticClass()))
    {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        menuName = "development station";
#endif
        return kBlockingMenuDevelopmentStation;
    }
    if (widget->IsA(SDK::UCrUW_ArmoryPanel::StaticClass()))
    {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        menuName = "equipment upgrade station";
#endif
        return kBlockingMenuEquipmentUpgradeStation;
    }
    if (widget->IsA(SDK::UCrUW_PauseMenu::StaticClass()))
    {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        menuName = "pause menu";
#endif
        return kBlockingMenuPauseMenu;
    }

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
    menuName = nullptr;
#endif
    return 0;
}

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
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
#endif

static void __fastcall CommonUIActivatedDetour(
    SDK::UCommonActivatableWidget* widget)
{
    g_commonUIActivatedOriginal(widget);

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
    const char* menuName = nullptr;
#endif
    const uint32_t menuBit = GetBlockingMenuBit(widget
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        , menuName
#endif
        );
    if (menuBit != 0)
    {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        const uint32_t suppressionMask =
#endif
        MiniMapUI::SetBlockingMenuActive(menuBit, true);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        LogCommonUITransition("Activated", menuName, suppressionMask);
#endif
    }
}

static void __fastcall CommonUIDeactivatedDetour(
    SDK::UCommonActivatableWidget* widget)
{
    g_commonUIDeactivatedOriginal(widget);

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
    const char* menuName = nullptr;
#endif
    const uint32_t menuBit = GetBlockingMenuBit(widget
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        , menuName
#endif
        );
    if (menuBit != 0)
    {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        const uint32_t suppressionMask =
#endif
        MiniMapUI::SetBlockingMenuActive(menuBit, false);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        LogCommonUITransition("Deactivated", menuName, suppressionMask);
#endif
    }
}

static void RemoveCommonUIHooks()
{
    auto* self = GetSelf();
    if (self != nullptr &&
        self->hooks != nullptr &&
        self->hooks->Hooks != nullptr &&
        self->hooks->Hooks->Remove != nullptr)
    {
        if (g_commonUIActivatedHook != nullptr)
        {
            self->hooks->Hooks->Remove(g_commonUIActivatedHook);
        }
        if (g_commonUIDeactivatedHook != nullptr)
        {
            self->hooks->Hooks->Remove(g_commonUIDeactivatedHook);
        }
    }

    g_commonUIActivatedHook = nullptr;
    g_commonUIDeactivatedHook = nullptr;
    g_commonUIActivatedOriginal = nullptr;
    g_commonUIDeactivatedOriginal = nullptr;
}

static bool InstallCommonUIHooks()
{
    auto* self = GetSelf();
    auto* hookUtils = self != nullptr && self->hooks != nullptr
        ? self->hooks->Hooks
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

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
    LOG_INFO("MiniMap: installed both CommonUI activation/deactivation detours");
#endif
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
#ifdef MODLOADER_CLIENT_BUILD

    g_resolvedAddresses = {};

    const bool fingerprintsResolved=MiniMapFingerprints::Resolve(
        self,
        scanner,
        g_resolvedAddresses);
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

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
    LOG_INFO(
        "MiniMap: native API initialized");
#endif

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
}
static void OnWorldBeginPlay(SDK::UWorld* world)
{
    if(!world||(world==MiniMapMap::GetWorld()&&!g_worldEnding))return;
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
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
    MiniMapTerrain::CancelPendingDiagnostic();
#endif

    LOG_INFO(
        "MiniMap: game world began: %p",
        world);

}

static void OnExperienceLoadComplete()
{
    if(!MiniMapMap::HasWorld()||g_worldEnding||g_preparedGeneration==g_worldGeneration)return;
    // Mark the attempt before preparation so duplicate events cannot repeat it.
    g_preparedGeneration=g_worldGeneration;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
    LOG_INFO("MiniMap: preparing gameplay services for generation %llu",static_cast<unsigned long long>(g_worldGeneration));
#endif

    // Preparation only registers the terrain service and resets CPU state.
    // Player/asset/Mass/capture prerequisites remain local to scheduled services.
    if(!MiniMapTerrain::Initialize(GetSelf()))
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
    if(!world||world!=MiniMapMap::GetWorld())return;
    g_worldEnding=true;
    SetServicesAdmitted(false);
    EndBuildingSession();
    MiniMapUI::Hide();
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
    const uint32_t blockingMenuMask =
#endif
    MiniMapUI::ClearBlockingMenuSuppression();
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
    if (blockingMenuMask != 0)
    {
        LOG_INFO(
            "MiniMap: cleared blocking menu mask at world end (0x%X)",
            blockingMenuMask);
    }
#endif
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
    MiniMapTerrain::CancelPendingDiagnostic();
#endif
}

static void OnAfterWorldEndPlay(
    SDK::UWorld* world,
    const char* worldName)
{
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
        return &s_pluginInfo;
    }

    __declspec(dllexport)
        bool PluginInit(IPluginSelf* self)
    {
        ResetPluginInitHookBaseline();
#ifdef MODLOADER_CLIENT_BUILD
        MiniMapCacheIdentity::Reset();
#endif
        g_self = self;
        auto* pluginSelf = GetSelf();

        if (pluginSelf == nullptr ||
            pluginSelf->hooks == nullptr)
        {
            g_self = nullptr;
            return false;
        }

        if (!MiniMapConfig::Initialize(
            pluginSelf))
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

        if (pluginSelf->hooks->World == nullptr || pluginSelf->hooks->Engine == nullptr ||
            !pluginSelf->hooks->Engine->RegisterOnTick || !pluginSelf->hooks->Engine->UnregisterOnTick ||
            !pluginSelf->hooks->World->RegisterOnBeforeWorldEndPlay || !pluginSelf->hooks->World->UnregisterOnBeforeWorldEndPlay)
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
                &ClearPersistentCaches, &MiniMapBuildingCache::Clear,
                cacheFailure, MiniMapConfig::ClearCache()))
        {
            LOG_ERROR("MiniMap: persistent cache initialization failed: %s",
                cacheFailure ? cacheFailure : "cache root unavailable");
            RemoveCommonUIHooks();
            return false;
        }

        if (!MiniMapUI::Initialize(
            pluginSelf))
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
        if (!MiniMapBuildingAtlas::Initialize(g_buildingSpriteStatuses, pluginSelf->hooks->ImGuiTextures))
        {
            LOG_ERROR("MiniMap: atlas texture interface initialization failed");
            MiniMapUI::Shutdown();
            RemoveCommonUIHooks();
            return false;
        }
        pluginSelf->hooks->Engine->RegisterOnTick(&OnBuildingSpriteRendererTick);

        // Close admission before module-specific native EndPlay cancellation.
        pluginSelf->hooks->World->RegisterOnBeforeWorldEndPlay(&OnBeforeWorldEndPlay);

        if (!MiniMapFogOfWar::Initialize(
            pluginSelf))
        {
            LOG_ERROR(
                "MiniMap: FOW diagnostic initialization failed");
        }

        if (!MiniMapPointsOfInterest::Initialize(
            pluginSelf))
        {
            LOG_ERROR(
                "MiniMap: landmark POI initialization failed");
        }

        if (!MiniMapAlienActors::Initialize(pluginSelf))
        {
            LOG_ERROR("MiniMap: Alien Actors initialization failed");
        }

        if (!MiniMapBuildingCollector::Initialize(pluginSelf))
        {
            LOG_ERROR("MiniMap: Stage B.1 building collector initialization failed");
        }

        if (!MiniMapBuildingVisuals::Initialize(pluginSelf))
        {
            LOG_ERROR("MiniMap: building visuals initialization failed");
        }

        if (!MiniMapFoundables::Initialize(
            pluginSelf))
        {
            LOG_ERROR(
                "MiniMap: Mass foundable initialization failed");
        }

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
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
#endif

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        if (!MiniMapTerrain::RegisterDiagnostics(pluginSelf))
        {
            LOG_ERROR(
                "MiniMap: failed to register terrain diagnostics");
        }
#endif

        // Registration replays events only during loader startup, never runtime reload.
        // Prepare every owner first; establish world identity before experience completion.
        CapturePluginInitHookBaseline(self);
        pluginSelf->hooks->World->RegisterOnAfterWorldEndPlay(&OnAfterWorldEndPlay);
        pluginSelf->hooks->World->RegisterOnWorldBeginPlay(&OnWorldBeginPlay);
        pluginSelf->hooks->World->RegisterOnExperienceLoadComplete(&OnExperienceLoadComplete);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        LOG_INFO("MiniMap: registered world lifecycle callbacks");
#endif
        // Runtime reload does not replay lifecycle events. Attach through the same
        // guarded handlers, accepting the existing loading-screen edge case.
        SDK::UWorld* currentWorld = SDK::UWorld::GetWorld();
        if (currentWorld && currentWorld->GetName() == "ChimeraMain")
        {
            OnWorldBeginPlay(currentWorld);
            OnExperienceLoadComplete();
            if (MiniMapMap::GameplayReady())
                LOG_INFO("MiniMap: active ChimeraMain attachment ready: %p", currentWorld);
        }

#endif

        LOG_INFO(
            "MiniMap: initialization complete");

        configGuard.Keep = true;
        return true;
    }

    __declspec(dllexport)
        void PluginShutdown()
    {
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
        auto* self = GetSelf();
        if(self && self->hooks && self->hooks->Engine)
            self->hooks->Engine->UnregisterOnTick(&OnBuildingSpriteRendererTick);
        MiniMapCacheIdentity::Reset();
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        const uint32_t blockingMenuMask =
#endif
        MiniMapUI::ClearBlockingMenuSuppression();
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        if (blockingMenuMask != 0)
        {
            LOG_INFO("MiniMap: cleared blocking menu mask during shutdown (0x%X)",
                blockingMenuMask);
        }
#endif
        MiniMapUI::Hide();

        if (self != nullptr &&
            self->hooks != nullptr &&
            self->hooks->World != nullptr)
        {
            self->hooks->World->UnregisterOnBeforeWorldEndPlay(&OnBeforeWorldEndPlay);
            self->hooks->World->UnregisterOnExperienceLoadComplete(
                &OnExperienceLoadComplete);

            self->hooks->World->UnregisterOnWorldBeginPlay(
                &OnWorldBeginPlay);

            self->hooks->World->UnregisterOnAfterWorldEndPlay(
                &OnAfterWorldEndPlay);
        }

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        MiniMapTerrain::UnregisterDiagnostics();
#endif
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
