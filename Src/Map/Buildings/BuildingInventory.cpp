#include "CallbackTiming.h"
#if defined(MODLOADER_CLIENT_BUILD)

#include "BuildingInventory.h"
#include "BuildingCollector.h"
#include "Native/ResidentMetadata.h"
#include "Map/Map.h"
#include "Native/NativeApi.h"
#include "plugin_helpers.h"
#include "SDK/Chimera_classes.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace
{
    using Clock = std::chrono::steady_clock;
    using Native = MiniMapNative::NativeApi;
    constexpr auto kOracleInterval = std::chrono::seconds(10);
    constexpr std::ptrdiff_t kManagerOffset = 0x38;
    constexpr int32_t kMaximumHandles = 1000000;
    IPluginSelf* g_inventorySelf = nullptr;
    bool g_initialized = false, g_ready = false, g_constructed = false;
    uint64_t g_generation = 0;
    SDK::UWorld* g_world = nullptr;
    SDK::UMassEntitySubsystem* g_subsystem = nullptr;
    const void* g_manager = nullptr; // Game-thread query owner; never published.
    SDK::UScriptStruct *g_building = nullptr, *g_transform = nullptr, *g_spline = nullptr;
    alignas(16) std::array<std::byte, 0x350> g_query = {};
    Clock::time_point g_nextPoll = {}, g_nextWarning = {};
#if defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
    std::array<uintptr_t,3> g_metadataRoots{};size_t g_metadataCount=0;
    Clock::time_point g_metadataRetry{};
#endif

    double Ms(Clock::time_point start)
    { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }

    void Warn(const char* reason)
    {
        if (Clock::now() < g_nextWarning) return;
        LOG_WARN("MiniMap: BuildingInventory: oracle unavailable: %s", reason);
        g_nextWarning = Clock::now() + kOracleInterval;
    }

    void Clear(const char* reason)
    {
        if (g_constructed)
            if (const auto* native = MiniMapNative::Get()) native->mass.queryDestruct(g_query.data());
        g_constructed = false; g_query = {};
#if defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
        size_t retained=0;
        for(size_t i=0;i<g_metadataCount;++i)
            if(!MiniMapResidentMetadata::Release(g_metadataRoots[i]))g_metadataRoots[retained++]=g_metadataRoots[i];
        g_metadataCount=retained;g_metadataRetry={};
#endif
        g_world = nullptr; g_subsystem = nullptr; g_manager = nullptr;
        g_building = g_transform = g_spline = nullptr;
        g_nextPoll = g_nextWarning = {};
        ++g_generation;
        LOG_INFO("MiniMap: BuildingInventory: oracle reset reason=%s generation=%llu",
            reason, static_cast<unsigned long long>(g_generation));
    }

#if defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
    SDK::UScriptStruct* FindStruct(const wchar_t* path,size_t size)
    {
        const auto p=MiniMapResidentMetadata::Acquire(path,true,size);
        if(p)g_metadataRoots[g_metadataCount++]=p;
        else g_metadataRetry=Clock::now()+std::chrono::seconds(1);
        return reinterpret_cast<SDK::UScriptStruct*>(p);
    }
#else
    SDK::UScriptStruct* FindStruct(const char* name, size_t size)
    {
        PluginObjectInfo info = {};
        if (g_inventorySelf->hooks->ObjectWalker->FindObjectsByNameInto(name, PluginObjectLookup_Both, &info, 1) != 1 ||
            std::strcmp(info.className, "ScriptStruct") != 0) return nullptr;
        auto* type = static_cast<SDK::UScriptStruct*>(info.object);
        return type->Size == int32_t(size) ? type : nullptr;
    }

#endif
    bool EnsureQuery(const Native& native)
    {
        MiniMapCallbackTiming::Phase phase(MiniMapCallbackTiming::Id::InventoryEnsure);
        if(!native.mass.getMassEntitySubsystem||!native.mass.queryConstruct||!native.mass.queryDestruct||
            !native.mass.addTagRequirement||!native.mass.addTransformRequirement)return false;
        auto* world = MiniMapMap::GetWorld();
        auto* subsystem = world ? native.mass.getMassEntitySubsystem(world) : nullptr;
        const void* manager = nullptr;
        if (subsystem)
            std::memcpy(&manager, reinterpret_cast<const uint8_t*>(subsystem) + kManagerOffset, sizeof(manager));
        if (g_constructed && world == g_world && subsystem == g_subsystem && manager == g_manager) return true;
        if (g_constructed) Clear("world/manager changed");
        if (!manager || !g_inventorySelf->hooks->ObjectWalker->IsReady()) return false;
#if defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
        if(!MiniMapResidentMetadata::Available())return false;
        if(Clock::now()<g_metadataRetry)return false;
        if(!g_building){g_building=FindStruct(L"/Script/Chimera.CrMassBuildingTag",sizeof(SDK::FCrMassBuildingTag));return false;}
        if(!g_transform){g_transform=FindStruct(L"/Script/MassCommon.TransformFragment",sizeof(SDK::FTransform));return false;}
        if(!g_spline){g_spline=FindStruct(L"/Script/AuActorPlacement.AuSplineConnectionFragment",sizeof(SDK::FAuSplineConnectionFragment));return false;}
#else
        if (!g_building) g_building = FindStruct("CrMassBuildingTag", sizeof(SDK::FCrMassBuildingTag));
        if (!g_transform) g_transform = FindStruct("TransformFragment", sizeof(SDK::FTransform));
        if (!g_spline) g_spline = FindStruct("AuSplineConnectionFragment", sizeof(SDK::FAuSplineConnectionFragment));
#endif
        // Missing spline metadata must not silently turn spline entities into ordinary buildings.
        if (!g_building || !g_transform || !g_spline) return false;
        native.mass.queryConstruct(g_query.data(), reinterpret_cast<const uint8_t*>(subsystem) + kManagerOffset);
        g_constructed = true;
        native.mass.addTagRequirement(g_query.data(), g_building, SDK::EMassFragmentPresence::All);
        native.mass.addTransformRequirement(g_query.data(),
            static_cast<uint8_t>(SDK::EMassFragmentAccess::ReadOnly),
            static_cast<uint8_t>(SDK::EMassFragmentPresence::Optional));
        g_world = world; g_subsystem = subsystem; g_manager = manager;
        LOG_INFO("MiniMap: BuildingInventory: independent oracle ready cadence=10s");
        return true;
    }

    MiniMapBuildingInventory::Record ReadReference(const Native& native, MiniMapNative::MassEntityHandle id)
    {
        MiniMapBuildingInventory::Record record;
        record.Index = id.Index; record.SerialNumber = id.SerialNumber;
        // Both checks precede InternalGetFragmentDataPtr, which asserts on invalid/unbuilt entities.
        if (id.Index < 0 || id.SerialNumber == 0 || !native.mass.isEntityValid(g_manager, id) ||
            !native.mass.isEntityBuilt(g_manager, id)) return record;
        const auto* transform = static_cast<const SDK::FTransform*>(native.mass.getFragmentDataPtr(g_manager, id, g_transform));
        if (transform)
        {
            record.Position = {transform->Translation.X, transform->Translation.Y, transform->Translation.Z};
            record.Rotation = {transform->Rotation.X, transform->Rotation.Y, transform->Rotation.Z, transform->Rotation.W};
            record.Scale = {transform->Scale3D.X, transform->Scale3D.Y, transform->Scale3D.Z};
            auto finite = [](double v) { return std::isfinite(v); };
            double norm = 0; for (double v : record.Rotation) norm += v * v;
            record.HasTransform = std::all_of(record.Position.begin(), record.Position.end(), finite) &&
                std::all_of(record.Rotation.begin(), record.Rotation.end(), finite) &&
                std::all_of(record.Scale.begin(), record.Scale.end(), finite) && std::isfinite(norm) && norm > 0;
        }
        record.HasSpline = native.mass.getFragmentDataPtr(g_manager, id, g_spline) != nullptr;
        return record;
    }

    void Poll()
    {
        MiniMapCallbackTiming::Phase phase(MiniMapCallbackTiming::Id::InventoryPoll);
        const auto* native = MiniMapNative::Get();
        if (!native || !native->mass.getMassEntitySubsystem || !native->mass.queryConstruct ||
            !native->mass.queryDestruct || !native->mass.addTagRequirement || !native->mass.addTransformRequirement ||
            !native->mass.getMatchingEntityHandles || !native->mass.getFragmentDataPtr ||
            !native->mass.isEntityValid || !native->mass.isEntityBuilt || !native->texture.memoryFree || !EnsureQuery(*native))
        { Warn("native query/metadata prerequisites"); return; }
        MiniMapNative::MassEntityHandleArray handles = {};
        const auto queryStart = Clock::now();
        native->mass.getMatchingEntityHandles(g_query.data(), &handles);
        const double queryMs = Ms(queryStart);
        struct HandleOwner
        {
            void* Data;
            MiniMapNative::TextureApi::MemoryFreeFn Free;
            ~HandleOwner() { if (Data) Free(Data); }
        } owner{handles.Data, native->texture.memoryFree};
        if (handles.Num < 0 || handles.Max < handles.Num || handles.Num > kMaximumHandles || (handles.Num && !handles.Data))
        { Warn("invalid native handle array; comparison skipped"); return; }
        if (!MiniMapBuildingCollector::BeginComparison(g_world, g_manager)) return;
        const auto referenceStart = Clock::now();
        // Stream one copied reference at a time: no worldwide record map, snapshot or delta history.
        for (int32_t i = 0; i < handles.Num; ++i)
            MiniMapBuildingCollector::ObserveBaseline(ReadReference(*native, handles.Data[i]));
        if (owner.Data) { owner.Free(owner.Data); owner.Data = nullptr; }
        MiniMapBuildingCollector::EndComparison(size_t(handles.Num), queryMs, Ms(referenceStart));
    }

    void OnTick(float)
    {
        MiniMapCallbackTiming::Scope callbackTiming(MiniMapCallbackTiming::Id::InventoryTick,true);
        if(!MiniMapMap::GameplayReady())return;
        if (!g_ready || !MiniMapMap::HasWorld() || Clock::now() < g_nextPoll) return;
#if defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
        // Metadata setup is serviced once per tick; the 10-second oracle cadence
        // begins only after the native query is ready.
        if(!g_constructed){const auto* native=MiniMapNative::Get();if(!native||!EnsureQuery(*native))return;}
#endif
        Poll();
        g_nextPoll = Clock::now() + kOracleInterval; // No catch-up; does not affect production cadence.
    }
}

namespace MiniMapBuildingInventory
{
    bool Initialize(IPluginSelf* self)
    {
#if !defined(MINIMAP_BUILDING_INVENTORY_ORACLE) || !MINIMAP_BUILDING_INVENTORY_ORACLE
        (void)self;
        LOG_INFO("MiniMap: BuildingInventory oracle disabled; opt in with MiniMapBuildingInventoryOracle=true");
        return true; // No callback, query setup, roots, enumeration or comparison.
#else
        if (g_initialized) return true;
        if (!self || !self->hooks || !self->hooks->Engine || !self->hooks->ObjectWalker ||
            !self->hooks->ObjectWalker->IsReady || !self->hooks->ObjectWalker->FindObjectsByNameInto) return false;
        g_inventorySelf = self; Reset(); self->hooks->Engine->RegisterOnTick(&OnTick); g_initialized = true;
        return true;
#endif
    }
    void Reset() { g_ready = false; Clear("lifecycle"); }
    void OnExperienceLoadComplete() { if (g_initialized) { g_ready = true; g_nextPoll = {}; } }
    void Shutdown()
    {
        if (!g_initialized) return;
        g_inventorySelf->hooks->Engine->UnregisterOnTick(&OnTick);
        Reset(); // Query destruction precedes NativeApi shutdown.
        g_inventorySelf = nullptr; g_initialized = false;
    }
}
#endif
