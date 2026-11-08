// Composes MiniMap services and lifecycle events, with legacy building work selected at compile time.
#include "plugin.h"
#include "plugin_helpers.h"
#include "DiagnosticExports.h"
#include "Config/Config.h"
#include "Services/Runtime.h"
#include "Services/RuntimeEvents.h"
#include "Services/NativeHooks.h"

#ifdef MODLOADER_CLIENT_BUILD
#include "Native/Fingerprints.h"
#include "Native/NativeApi.h"
#include "Native/TexturePublication.h"
#if MINIMAP_USE_LEGACY_BUILDINGS
#include "Native/ReadbackNativeAdapter.h"
#endif
#include "UI/MiniMapUI.h"
#include "Input/MouseWheel.h"
#include "SDK/CommonUI_classes.hpp"
#include "SDK/ChimeraUI_classes.hpp"
#if MINIMAP_USE_LEGACY_BUILDINGS
#include "Map/Buildings/BuildingVisuals.h"
#endif
#include "Services/Map/Buildings/BuildingMapService.h"
#include "Services/Map/Buildings/Capture/BuildingSpriteRenderer.h"
#include "PluginPaths.h"
#if MINIMAP_USE_LEGACY_BUILDINGS
#include "Native/ResidentMetadata.h"
#endif
#include <array>
#include <filesystem>
#include <memory>
#endif

#ifdef MODLOADER_CLIENT_BUILD
static MiniMapFingerprints::ResolvedAddresses g_resolvedAddresses = {};
namespace S = MiniMap::Services;
static std::unique_ptr<Services::Map::Buildings::BuildingMapService> g_buildingMapService;
static Services::Map::Buildings::Capture::BuildingSpriteRenderer* g_buildingSpriteRenderer = nullptr;
static S::Hook g_commonUIActivatedHook{};
static S::Hook g_commonUIDeactivatedHook{};
static S::Registration g_rendererTick{}, g_beforeEnd{}, g_afterEnd{}, g_gameplayBegan{}, g_experienceReady{};
static bool AcceptEventRegistration(S::Registration& token, const S::RegistrationReceipt<S::Registration>& receipt)
{
    if (receipt.token.id) token = receipt.token;
    if (receipt.acceptance == S::Acceptance::Rejected || receipt.acceptance == S::Acceptance::BackendUnknown)
    {
        LOG_ERROR("MiniMap: required runtime-event registration failed (acceptance=%d)", int(receipt.acceptance));
        return false;
    }
    return true; // AcceptedUnverified preserves normal AlienX initialization.
}
static void RemoveEventRegistration(S::Registration& token)
{
    if (!token.id) return;
    if (auto* events = S::TryRuntimeEvents())
    {
        const auto removal = events->Remove(token);
        if (removal.registry == S::RegistryRemoval::RequestReturned && removal.core == S::CoreDrain::Quiescent)
            token = {};
        else LOG_ERROR("MiniMap: runtime-event removal retains pending/unknown evidence (token=%llu)",
            static_cast<unsigned long long>(token.id));
    }
}
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
    if (auto* hooks = S::TryNativeHooks())
    {
        if (g_commonUIActivatedHook.id)
        {
            const auto removal = hooks->Remove(g_commonUIActivatedHook);
            if (removal.registry != S::RegistryRemoval::RequestReturned)
                LOG_ERROR("MiniMap: CommonUI activation removal remains uncertain");
        }
        if (g_commonUIDeactivatedHook.id)
        {
            const auto removal = hooks->Remove(g_commonUIDeactivatedHook);
            if (removal.registry != S::RegistryRemoval::RequestReturned)
                LOG_ERROR("MiniMap: CommonUI deactivation removal remains uncertain");
        }
    }

    // Existing game-owned slot clearing follows removal requests. The adapter
    // retains backend identity/evidence; clearing is not a drain certificate.
    g_commonUIActivatedHook = {};
    g_commonUIDeactivatedHook = {};
    g_commonUIActivatedOriginal = nullptr;
    g_commonUIDeactivatedOriginal = nullptr;
}

static bool InstallCommonUIHooks()
{
    auto* hookUtils = S::TryNativeHooks();
    const auto* nativeApi = MiniMapNative::Get();
    if (hookUtils == nullptr || nativeApi == nullptr ||
        nativeApi->commonUI.nativeOnActivated == nullptr ||
        nativeApi->commonUI.nativeOnDeactivated == nullptr)
    {
        LOG_ERROR("MiniMap: CommonUI lifecycle hook dependencies are unavailable");
        return false;
    }

    const auto activated = hookUtils->Install({
        reinterpret_cast<uintptr_t>(nativeApi->commonUI.nativeOnActivated),
        reinterpret_cast<void*>(&CommonUIActivatedDetour),
        [](void* owner, void* original) noexcept {
            *static_cast<MiniMapNative::CommonUIApi::NativeOnActivatedFn*>(owner) =
                reinterpret_cast<MiniMapNative::CommonUIApi::NativeOnActivatedFn>(original);
        }, &g_commonUIActivatedOriginal});
    g_commonUIActivatedHook = activated.handle;
    if (activated.state != S::HookInstallState::Active)
    {
        RemoveCommonUIHooks();
        LOG_ERROR("MiniMap: CommonUI activation detour installation failed");
        if (activated.state == S::HookInstallState::BackendUnknown)
            LOG_ERROR("MiniMap: CommonUI activation backend state is unknown; rollback request does not establish restoration");
        return false;
    }

    const auto deactivated = hookUtils->Install({
        reinterpret_cast<uintptr_t>(nativeApi->commonUI.nativeOnDeactivated),
        reinterpret_cast<void*>(&CommonUIDeactivatedDetour),
        [](void* owner, void* original) noexcept {
            *static_cast<MiniMapNative::CommonUIApi::NativeOnDeactivatedFn*>(owner) =
                reinterpret_cast<MiniMapNative::CommonUIApi::NativeOnDeactivatedFn>(original);
        }, &g_commonUIDeactivatedOriginal});
    g_commonUIDeactivatedHook = deactivated.handle;
    if (deactivated.state != S::HookInstallState::Active)
    {
        RemoveCommonUIHooks();
        LOG_ERROR("MiniMap: CommonUI deactivation detour installation failed");
        if (deactivated.state == S::HookInstallState::BackendUnknown)
            LOG_ERROR("MiniMap: CommonUI deactivation backend state is unknown; rollback request does not establish restoration");
        return false;
    }

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
    LOG_INFO("MiniMap: installed both CommonUI activation/deactivation detours");
#endif
    return true;
}
#endif

#ifdef MODLOADER_CLIENT_BUILD
#include "Map/FogOfWar/FogOfWar.h"
#include "Map/Foundables/Foundables.h"
#include "Map/AlienActors/AlienActors.h"
#if MINIMAP_USE_LEGACY_BUILDINGS
#include "Map/Buildings/BuildingCollector.h"
#include "Map/Buildings/BuildingVisuals.h"
#endif
#include "Map/PointsOfInterest/PointsOfInterest.h"
#include "Map/Terrain/Terrain.h"
#include "Map/Terrain/TerrainSource.h"
#include "Map/Terrain/TerrainCache.h"
#include "CacheIdentity.h"
#include "Map/Map.h"
#include <cstdint>
#endif

#ifdef MODLOADER_CLIENT_BUILD
// Clears building files through the application-owned BuildingMapService instance.
static bool ClearBuildingsCache()
{
    return g_buildingMapService != nullptr && g_buildingMapService->ClearCache();
}

static bool ClearPersistentCaches()
{
    const bool terrainCleared = MiniMapTerrainCache::Clear();
    const bool buildingsCleared = ClearBuildingsCache();
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

// Preserve the centralized mandatory resolution chain through a borrowed service.
void MiniMap::Adapters::AlienX::ResolveNativeDependencies(
    MiniMap::Services::IRequiredScanner& scanner)
{
#ifdef MODLOADER_CLIENT_BUILD

    g_resolvedAddresses = {};

    const bool fingerprintsResolved=MiniMapFingerprints::Resolve(
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
        scanner.Fail("MiniMap::NativeDependencies", "native API initialization failed");
        return;
    }

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
    LOG_INFO(
        "MiniMap: native API initialized");
#endif

#if MINIMAP_USE_LEGACY_BUILDINGS
    // Optional private preflight: failure never changes production readiness.
    MiniMapReadbackNative::ResolvePrerequisites();
#endif

#else

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
#if MINIMAP_USE_LEGACY_BUILDINGS
static void EndBuildingSession()
{
    MiniMapBuildingVisuals::EndSession();
}
static bool BuildingServiceGameThread() noexcept
{
    const auto* api = MiniMapNative::Get();
    __try { return api && api->player.isGameThread && api->player.isGameThread(); }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
}

// Legacy world-identity diagnostics run in Client Release as well as Debug.
namespace
{
    enum class DiagnosticWorld { Null, Gameplay, Private, Other };
    enum class DiagnosticGameState { Deferred, Null, Compatible, Incompatible };
    struct WorldIdentitySample
    {
        uint64_t Sequence = 0, Tick = 0;
        const char* Phase = nullptr;
        SDK::UWorld* Getter = nullptr;
        SDK::UWorld* Gameplay = nullptr;
        SDK::AGameStateBase* GameState = nullptr;
        DiagnosticWorld Selection = DiagnosticWorld::Null;
        DiagnosticGameState State = DiagnosticGameState::Deferred;
        bool Changed = false, Determinate = false, Anomalous = false;
        MiniMapBuildingVisuals::DiagnosticSnapshot Capture;
    };
    struct WorldIdentityDiagnostics
    {
        uint64_t Generation = 0, Tick = 0, Sequence = 0, LastSummaryTick = 0;
        SDK::AGameStateBase* BaselineGameState = nullptr; // Identity only; never dereferenced.
        SDK::FName GameStateClassName{};
        bool ClassNameKnown = false, InitialLogged = false, FirstAnomaly = false, Anomalous = false;
        unsigned Transitions = 0, Summaries = 0;
        size_t Next = 0, Count = 0;
        std::array<WorldIdentitySample, 16> History{};
    } g_worldIdentityDiagnostics;

    const char* WorldSelectionName(DiagnosticWorld value)
    {
        switch (value)
        {
        case DiagnosticWorld::Null: return "null";
        case DiagnosticWorld::Gameplay: return "gameplay";
        case DiagnosticWorld::Private: return "private";
        default: return "other";
        }
    }
    const char* GameStateObservationName(DiagnosticGameState value)
    {
        switch (value)
        {
        case DiagnosticGameState::Null: return "null";
        case DiagnosticGameState::Compatible: return "compatible";
        case DiagnosticGameState::Incompatible: return "incompatible";
        default: return "deferred";
        }
    }
    bool DiagnosticLiveInstance(const SDK::UObject* object)
    {
        const auto forbidden = SDK::EObjectFlags::ClassDefaultObject | SDK::EObjectFlags::ArchetypeObject |
            SDK::EObjectFlags::BeginDestroyed | SDK::EObjectFlags::FinishDestroyed;
        return object && !(unsigned(object->Flags) & unsigned(forbidden));
    }
    DiagnosticGameState DiagnosticGameStateCompatibility(const SDK::AGameStateBase* gameState)
    {
        auto& d = g_worldIdentityDiagnostics;
        if (d.ClassNameKnown) return gameState->IsA(d.GameStateClassName) ?
            DiagnosticGameState::Compatible : DiagnosticGameState::Incompatible;
        // StaticClass() can enumerate GObjects on its first use. Instead observe
        // only this admitted object's existing, bounded SDK class hierarchy.
        auto* type = gameState->Class;
        if (!type) return DiagnosticGameState::Deferred;
        for (unsigned depth = 0; type && depth < 32; ++depth, type = static_cast<SDK::UClass*>(type->SuperStruct))
        {
            // No object-table lookup, loading, or native ownership operation.
            if (type->GetName() == "CrGameStateBase")
            {
                d.GameStateClassName = type->Name;
                d.ClassNameKnown = true;
                return DiagnosticGameState::Compatible;
            }
        }
        return type ? DiagnosticGameState::Deferred : DiagnosticGameState::Incompatible;
    }
    void LogWorldIdentity(const char* event, const WorldIdentitySample& s)
    {
        const auto& d = g_worldIdentityDiagnostics;
        const auto& c = s.Capture;
        LOG_INFO("MiniMap world identity: event=%s generation=%llu seq=%llu tick=%llu phase=%s sdk=%p gameplay=%p private=%p selection=%s gs=%p gsState=%s baselineGS=%p changed=%u determinate=%u anomalous=%u capture=%s session=%llu worldPhase=%u requestPhase=%u blueprintTail=%s",
            event, d.Generation, s.Sequence, s.Tick, s.Phase, s.Getter, s.Gameplay,
            reinterpret_cast<void*>(c.PrivateWorld), WorldSelectionName(s.Selection), s.GameState,
            GameStateObservationName(s.State), d.BaselineGameState, unsigned(s.Changed),
            unsigned(s.Determinate), unsigned(s.Anomalous),
            c.Active ? "active" : "none", c.Session, c.WorldPhase, c.RequestPhase, c.BlueprintTail);
    }
    void ObserveWorldIdentity(const char* phase, bool tickEntry = false) noexcept
    {
        // The renderer metadata is GT-owned. Never touch it on rejected threads.
        if (!BuildingServiceGameThread()) return;
        auto* gameplay = MiniMapMap::GetWorld();
        if (!gameplay) return;
        auto& d = g_worldIdentityDiagnostics;
        if (d.Generation != g_worldGeneration)
        {
            d = {};
            d.Generation = g_worldGeneration;
        }
        if (tickEntry) ++d.Tick;
        WorldIdentitySample s;
        s.Sequence = ++d.Sequence; s.Tick = d.Tick; s.Phase = phase;
        s.Gameplay = gameplay;
        s.Getter = SDK::UWorld::GetWorld(); // Scalar identity only; NEVER dereference this result.
        s.Capture = MiniMapBuildingVisuals::GetDiagnosticSnapshot();
        s.Selection = !s.Getter ? DiagnosticWorld::Null : s.Getter == gameplay ? DiagnosticWorld::Gameplay :
            reinterpret_cast<uintptr_t>(s.Getter) == s.Capture.PrivateWorld ? DiagnosticWorld::Private : DiagnosticWorld::Other;
        const bool active = MiniMapMap::GameplayReady() && !g_worldEnding;
        try
        {
            // Available's existing GT/GC predicates do not probe memory or enumerate.
            // Only the lifecycle-retained gameplay world is inspected, never Getter.
            if (active && MiniMapResidentMetadata::Available() && DiagnosticLiveInstance(gameplay))
            {
                s.GameState = gameplay->GameState;
                if (!s.GameState) s.State = DiagnosticGameState::Null;
                else if (DiagnosticLiveInstance(s.GameState))
                    s.State = DiagnosticGameStateCompatibility(s.GameState);
            }
        }
        catch (...) { s.State = DiagnosticGameState::Deferred; }
        if (!d.BaselineGameState && s.State == DiagnosticGameState::Compatible)
            d.BaselineGameState = s.GameState;
        s.Changed = d.BaselineGameState && s.State != DiagnosticGameState::Deferred && s.GameState != d.BaselineGameState;
        s.Determinate = active && (s.Selection != DiagnosticWorld::Gameplay || s.State != DiagnosticGameState::Deferred);
        if (s.Determinate)
            d.Anomalous = s.Selection != DiagnosticWorld::Gameplay || s.State == DiagnosticGameState::Null ||
                s.State == DiagnosticGameState::Incompatible || s.Changed;
        s.Anomalous = d.Anomalous; // A deferred inspection cannot claim recovery.
        const WorldIdentitySample* previous = d.Count ? &d.History[(d.Next + d.History.size() - 1) % d.History.size()] : nullptr;
        const bool transition = previous && active && (s.Getter != previous->Getter ||
            s.Capture.PrivateWorld != previous->Capture.PrivateWorld ||
            (s.State != DiagnosticGameState::Deferred && previous->State != DiagnosticGameState::Deferred &&
                (s.GameState != previous->GameState || s.State != previous->State)) ||
            s.Anomalous != previous->Anomalous);
        d.History[d.Next] = s;
        d.Next = (d.Next + 1) % d.History.size();
        if (d.Count < d.History.size()) ++d.Count;
        if (!d.InitialLogged && active && s.State != DiagnosticGameState::Deferred)
        {
            d.InitialLogged = true;
            LogWorldIdentity("initial-admitted-baseline", s);
        }
        if (s.Determinate && s.Anomalous && !d.FirstAnomaly)
        {
            d.FirstAnomaly = true; d.LastSummaryTick = d.Tick;
            // At most 15 preceding records, oldest first, then the anomaly.
            for (size_t i = 0; i + 1 < d.Count; ++i)
                LogWorldIdentity("history", d.History[(d.Next + d.History.size() - d.Count + i) % d.History.size()]);
            LogWorldIdentity("first-anomaly", s);
        }
        else if (d.InitialLogged && transition && d.Transitions < 12)
        {
            ++d.Transitions;
            LogWorldIdentity("transition", s);
        }
        else if (active && d.FirstAnomaly && d.Anomalous && d.Summaries < 4 && d.Tick - d.LastSummaryTick >= 300)
        {
            ++d.Summaries; d.LastSummaryTick = d.Tick;
            LogWorldIdentity("anomaly-summary", s);
        }
    }
}
#endif // MINIMAP_USE_LEGACY_BUILDINGS

// Services generic native textures on the existing game-thread event; legacy work retains its ordering.
static void OnTextureServiceTick(
    Services::Map::Buildings::Capture::BuildingSpriteRenderer* renderer,
    float deltaTime)
{
    if (renderer != nullptr)
        renderer->ProcessLifecycle();
#if MINIMAP_USE_LEGACY_BUILDINGS
    ObserveWorldIdentity("renderer-entry", true);
    // Service generic texture owners without advancing building acquisition.
    if(!MiniMapMap::GameplayReady())
    {
        MiniMapTexturePublication::Service();
        ObserveWorldIdentity("after-texture-publication");
        return;
    }
    MiniMapBuildingVisuals::TickSpriteCapture(deltaTime,MiniMapMap::GameplayReady());
    ObserveWorldIdentity("after-sprite-renderer");
    // Native progress has priority. Process the entire last completed drawing
    // batch on this existing GT callback, never while holding LifecycleMutex.
    const bool buildingGameplayReady=MiniMapMap::GameplayReady();
    const bool buildingVisible=buildingGameplayReady && MiniMapUI::IsVisible();
    const bool buildingGameThread=buildingGameplayReady && buildingVisible && BuildingServiceGameThread();
    MiniMapBuildingVisuals::ServiceSpriteRequirements(buildingGameplayReady,buildingVisible,buildingGameThread);
    ObserveWorldIdentity("after-sprite-scheduler");
    MiniMapTexturePublication::Service();
    ObserveWorldIdentity("after-texture-publication");
    ObserveWorldIdentity("after-mass-buildingcomplex");
#else
    (void)deltaTime;
    MiniMapTexturePublication::Service();
#endif
}
static void OnWorldBeginPlay(SDK::UWorld* world)
{
    if(!world||(world==MiniMapMap::GetWorld()&&!g_worldEnding))return;
#if !MINIMAP_USE_LEGACY_BUILDINGS
    if (g_buildingMapService != nullptr)
        g_buildingMapService->StopGameSession();
#endif
    ++g_worldGeneration;
    g_preparedGeneration=0;
    g_worldEnding=false;
    SetServicesAdmitted(false);
    // A replacement world can begin before the owned world's EndPlay. Keep the
    // previous gameplay identity available until its native owner has ended.
#if MINIMAP_USE_LEGACY_BUILDINGS
    EndBuildingSession();
#endif
    MiniMapUI::Hide();
    MiniMapMap::SetWorld(world);
    MiniMapFogOfWar::Reset();
    MiniMapFoundables::Reset();
    MiniMapAlienActors::Reset();
#if MINIMAP_USE_LEGACY_BUILDINGS
    MiniMapBuildingCollector::Reset();
    MiniMapBuildingVisuals::Reset();
#endif
    MiniMapPointsOfInterest::Reset();
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
    MiniMapTerrain::CancelPendingDiagnostic();
#endif

    LOG_INFO(
        "MiniMap: game world began: %p",
        world);

}

#if !MINIMAP_USE_LEGACY_BUILDINGS
// Starts V2 gameplay only after authored terrain coverage and the owned service are ready.
static bool TryStartBuildingMapSession()
{
    double mapWidthMapUnits = 0.0;
    double mapHeightMapUnits = 0.0;
    if (!MiniMapTerrainSource::TryGetWorldDimensions(mapWidthMapUnits, mapHeightMapUnits))
    {
        LOG_ERROR("MiniMap: authored terrain coverage dimensions are unavailable; V2 gameplay remains unadmitted");
        SetServicesAdmitted(false);
        return false;
    }
    if (g_buildingMapService == nullptr)
    {
        LOG_ERROR("MiniMap: Building Map service is unavailable; V2 gameplay remains unadmitted");
        SetServicesAdmitted(false);
        return false;
    }
    if (!g_buildingMapService->StartGameSession(mapWidthMapUnits, mapHeightMapUnits))
    {
        LOG_ERROR("MiniMap: Building Map session start was rejected; V2 gameplay remains unadmitted");
        SetServicesAdmitted(false);
        return false;
    }
    return true;
}
#endif

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
#if MINIMAP_USE_LEGACY_BUILDINGS
        EndBuildingSession();
#endif
        return;
    }

    MiniMapFoundables::OnExperienceLoadComplete();
    MiniMapAlienActors::OnExperienceLoadComplete();
#if MINIMAP_USE_LEGACY_BUILDINGS
    MiniMapBuildingCollector::OnExperienceLoadComplete();
#endif
    MiniMapPointsOfInterest::OnExperienceLoadComplete();

#if MINIMAP_USE_LEGACY_BUILDINGS
    if(!MiniMapBuildingVisuals::BeginSession())
    {
        EndBuildingSession();
        LOG_ERROR("MiniMap: building renderer/atlas session blocked by retained ownership or missing dependencies; gameplay remains unadmitted");
        return;
    }
#else
    if (!TryStartBuildingMapSession())
    {
        return;
    }
#endif
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
#if !MINIMAP_USE_LEGACY_BUILDINGS
    if (g_buildingMapService != nullptr)
        g_buildingMapService->StopGameSession();
#endif
#if MINIMAP_USE_LEGACY_BUILDINGS
    EndBuildingSession();
#endif
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
    std::string_view worldName)
{
    if(!world||world!=MiniMapMap::GetWorld())return;
    LOG_INFO(
        "MiniMap: world ended: %.*s",
        int(worldName.data() ? worldName.size() : 6),
        worldName.data() ? worldName.data() : "<null>");

    g_worldEnding=true;
    SetServicesAdmitted(false);
#if !MINIMAP_USE_LEGACY_BUILDINGS
    if (g_buildingMapService != nullptr)
        g_buildingMapService->StopGameSession();
#endif
#if MINIMAP_USE_LEGACY_BUILDINGS
    EndBuildingSession(); // Idempotent fallback if BeforeEndPlay was not delivered.
#endif
    g_preparedGeneration=0;
    MiniMapUI::Hide();

    MiniMapFoundables::Reset();
    MiniMapAlienActors::Reset();
#if MINIMAP_USE_LEGACY_BUILDINGS
    MiniMapBuildingCollector::Reset();
    MiniMapBuildingVisuals::Reset();
#endif
    MiniMapPointsOfInterest::Reset();
    MiniMapFogOfWar::Reset();
    MiniMapMap::SetWorld(nullptr);
    MiniMapTerrain::Shutdown();
}

#endif

namespace MiniMap
{
    bool Initialize()
    {
#ifdef MODLOADER_CLIENT_BUILD
        MiniMapCacheIdentity::Reset();
#endif
        auto* pluginSelf = GetSelf();

        if (pluginSelf == nullptr ||
            pluginSelf->hooks == nullptr)
        {
            return false;
        }

        if (!MiniMapConfig::Initialize())
        {
            LOG_ERROR(
                "MiniMap: configuration initialization failed");

            return false;
        }

        // Unwind the listener on every later initialization failure while self is valid.
        struct ConfigInitGuard
        {
            bool Keep = false;
            ~ConfigInitGuard() { if (!Keep) MiniMapConfig::Shutdown(); }
        } configGuard;

#ifdef MODLOADER_CLIENT_BUILD

        auto* events = S::TryRuntimeEvents();
        if (events == nullptr)
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

        if (g_buildingMapService != nullptr)
        {
            LOG_ERROR("MiniMap: Building Map service is already initialized");
            return false;
        }

        std::filesystem::path pluginDirectory;
        const char* cacheFailure = nullptr;
        if (!MiniMapPluginPaths::GetPluginDirectory(pluginDirectory, &cacheFailure))
        {
            LOG_ERROR("MiniMap: application cache root resolution failed: %s",
                cacheFailure ? cacheFailure : "plugin directory unavailable");
            return false;
        }
        const std::filesystem::path cacheRoot = pluginDirectory / L"MiniMap" / L"Cache";

        // These approved settings use native centimeters and the established direct-index policy table.
        constexpr double sectionSizeMapUnits = 12500.0;
        constexpr std::uint32_t sectionResolutionPixels = 256;
        const std::array<::Services::Map::Buildings::SplinePolicy, 4> splinePolicies{{
            {0xFFFFFF, 1.0}, // Reserved index 0 is never resolved.
            {0x000000, 1.0}, // DroneRail index 1.
            {0x80EBDD, 1.0}, // Zipline index 2.
            {0x1F1E1C, 1.0}}}; // Walkway index 3.
        g_buildingMapService.reset(new ::Services::Map::Buildings::BuildingMapService(
            sectionSizeMapUnits,
            sectionResolutionPixels,
            cacheRoot,
            splinePolicies));
        g_buildingSpriteRenderer = g_buildingMapService->GetCapturerLifecycleTarget();
        if (g_buildingSpriteRenderer == nullptr || !g_buildingSpriteRenderer->RequestInitialization())
        {
            g_buildingMapService.reset();
            g_buildingSpriteRenderer = nullptr;
            LOG_ERROR("MiniMap: capturer lifecycle initialization request failed");
            return false;
        }

        // Owns rollback of the service when any later plugin initialization step fails.
        struct BuildingMapServiceInitGuard
        {
            bool Keep = false;
            // Releases the application-owned service unless initialization completes.
            ~BuildingMapServiceInitGuard()
            {
                if (!Keep)
                {
                    if (g_buildingSpriteRenderer != nullptr)
                    {
                        g_buildingSpriteRenderer->RequestExit();
                        g_buildingSpriteRenderer->ProcessLifecycle();
                    }
                    g_buildingSpriteRenderer = nullptr;
                    g_buildingMapService.reset();
                }
            }
        } buildingMapServiceInitGuard;

        if (!InstallCommonUIHooks())
        {
            return false;
        }

        if (!MiniMapCacheIdentity::Initialize(cacheRoot,
                &ClearPersistentCaches, &ClearBuildingsCache,
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

#if MINIMAP_USE_LEGACY_BUILDINGS
        if(!MiniMapBuildingVisuals::InitializeSpriteProcessing())
        {
            LOG_ERROR("MiniMap: atlas texture interface initialization failed");
            MiniMapUI::Shutdown();
            MiniMapCacheIdentity::Reset();
            RemoveCommonUIHooks();
            return false;
        }
#endif
        if (!AcceptEventRegistration(g_rendererTick, events->OnTick(
            [](void* user, float delta)
            {
                OnTextureServiceTick(
                    static_cast<::Services::Map::Buildings::Capture::BuildingSpriteRenderer*>(user),
                    delta);
            }, g_buildingSpriteRenderer)))
        {
            Shutdown();
            return false;
        }

        // Close admission before module-specific native EndPlay cancellation.
        if (!AcceptEventRegistration(g_beforeEnd, events->OnWorld(S::WorldPhase::BeforeEnd,
            [](void*, const S::WorldEvent& event) { OnBeforeWorldEndPlay(event.world, event.displayName.data()); }, nullptr)))
        {
            Shutdown();
            return false;
        }

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

#if MINIMAP_USE_LEGACY_BUILDINGS
        if (!MiniMapBuildingCollector::Initialize(pluginSelf))
        {
            LOG_ERROR("MiniMap: Stage B.1 building collector initialization failed");
        }

        if (!MiniMapBuildingVisuals::Initialize(pluginSelf))
        {
            LOG_ERROR("MiniMap: building visuals initialization failed");
        }

#endif
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
        MiniMapTexturePublication::Initialize();
        Adapters::AlienX::CapturePluginInitHookBaseline(pluginSelf);
        if (!AcceptEventRegistration(g_afterEnd, events->OnWorld(S::WorldPhase::AfterEnd,
            [](void*, const S::WorldEvent& event) { OnAfterWorldEndPlay(event.world, event.displayName); }, nullptr)) ||
            !AcceptEventRegistration(g_gameplayBegan, events->OnWorld(S::WorldPhase::GameplayBegan,
            [](void*, const S::WorldEvent& event) { OnWorldBeginPlay(event.world); }, nullptr)) ||
            !AcceptEventRegistration(g_experienceReady, events->OnWorld(S::WorldPhase::ExperienceReady,
            [](void*, const S::WorldEvent&) { OnExperienceLoadComplete(); }, nullptr)))
        {
            Shutdown();
            return false;
        }
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

        buildingMapServiceInitGuard.Keep = true;

#endif

        LOG_INFO(
            "MiniMap: initialization complete");

        configGuard.Keep = true;
        return true;
    }

    void Shutdown()
    {
        LOG_INFO(
            "MiniMap: shutting down");

#ifdef MODLOADER_CLIENT_BUILD

        MiniMapUI::StopInput();
        MiniMapTexturePublication::Shutdown();
        MiniMapConfig::Shutdown();
        RemoveCommonUIHooks();
        g_worldEnding=true;
        SetServicesAdmitted(false);
#if MINIMAP_USE_LEGACY_BUILDINGS
        MiniMapBuildingVisuals::ShutdownSpriteProcessing();
#else
        if (g_buildingMapService != nullptr)
            g_buildingMapService->StopGameSession();
#endif
        if (g_buildingSpriteRenderer != nullptr)
        {
            g_buildingSpriteRenderer->RequestExit();
            g_buildingSpriteRenderer->ProcessLifecycle();
        }
        RemoveEventRegistration(g_rendererTick);
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

        RemoveEventRegistration(g_beforeEnd);
        RemoveEventRegistration(g_experienceReady);
        RemoveEventRegistration(g_gameplayBegan);
        RemoveEventRegistration(g_afterEnd);

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        MiniMapTerrain::UnregisterDiagnostics();
#endif
        MiniMapAlienActors::Shutdown();
#if MINIMAP_USE_LEGACY_BUILDINGS
        MiniMapBuildingVisuals::Shutdown();
        MiniMapBuildingCollector::Shutdown();
#endif
        MiniMapFoundables::Shutdown();
        MiniMapPointsOfInterest::Shutdown();
        MiniMapFogOfWar::Shutdown();
        MiniMapTerrain::Shutdown();
        MiniMapMap::Shutdown();
        MiniMapUI::Shutdown();

        MiniMapCacheIdentity::ClearOnExit();
        g_buildingMapService.reset();
        g_buildingSpriteRenderer = nullptr;

        // NativeApi should be the last subsystem torn down.
        // Other subsystems depend on it for their own shutdown paths.
        MiniMapNative::Shutdown();
#endif

        MiniMapConfig::Shutdown();
    }
}
