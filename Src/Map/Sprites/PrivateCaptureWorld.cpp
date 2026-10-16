#if defined(MINIMAP_STAGE_R2_SMELTER_CAPTURE) && defined(MODLOADER_CLIENT_BUILD)
#include "PrivateCaptureWorld.h"
#include "SpriteCaptureCore.h"
#include "SceneCaptureConfiguration.h"
#include "CapturePng.h"
#include <Engine_parameters.hpp>
#include <unordered_set>
#include <string>
#include <algorithm>
#include <cmath>
#include "Native/NativeApi.h"
#include "Native/DefinitionAcquisitionCore.h"
#include "Native/ReadbackNativeAdapter.h"
#include "Native/ReadbackNativeBindings.h"
#include "Native/SmelterCaptureNativeBindings.h"
#include "Map/Sprites/BuildingRepresentation.h"
#include "Map/Sprites/RepresentationMetadataPatterns.h"
#include "Map/Map.h"
#include "plugin_helpers.h"
#include <SDK/Chimera_classes.hpp>
#include <SDK/MassEntity_classes.hpp>
#include <SDK/MassReplication_classes.hpp>
#include <array>
#include <cstring>
#include <cwchar>
#include <type_traits>

namespace MiniMapSpriteCapture
{
namespace DefinitionSlots = MiniMapDefinitionAcquisition;
namespace N = MiniMapNative;
struct PrivateCaptureWorld::Implementation
{
    // Matching CL-127004 PDB type 0x131001. Win64 passes this 16-byte
    // BY-VALUE aggregate indirectly using caller-owned temporary storage.
    struct alignas(8) InitializationValues
    {
        uint32_t Flags = 1; // Only bInitializeScenes; all other bits disabled.
        uint32_t Padding = 0;
        uintptr_t DefaultGameMode = 0;
    };
    static_assert(sizeof(InitializationValues) == 16);
    static_assert(offsetof(InitializationValues, DefaultGameMode) == 8);
    static_assert(sizeof(SDK::FName) == 8);
    static_assert(std::is_trivially_destructible_v<SDK::FName>);
    static_assert(std::is_trivially_copyable_v<SDK::FName>);
    static_assert(sizeof(SDK::UWorld) == 0xab0);
    static_assert(offsetof(SDK::UWorld, PersistentLevel) == 0x30);
    static_assert(offsetof(SDK::UWorld, OwningGameInstance) == 0x230);
    static_assert(offsetof(SDK::ULevel, OwningWorld) == 0xc0);
    static_assert(offsetof(SDK::ULevel, WorldSettings) == 0x2c8);
    static_assert(offsetof(SDK::UStruct, SuperStruct) == 0x40);
    static_assert(sizeof(SDK::UCrElectricityGraphSubsystem) == 0x358);
    static_assert(sizeof(SDK::UCrEntityGridSubsystem) == 0x150);
    static_assert(sizeof(SDK::UCrBuildingCustomNameSubsystem) == 0x1a8);
    static_assert(sizeof(SDK::UTickableWorldSubsystem) == 0x40);
    static_assert(sizeof(SDK::USubsystem) == 0x30);
    static_assert(0x9f0 + 0xc0 == sizeof(SDK::UWorld));

    using Create = uintptr_t(*)(int32_t, bool, SDK::FName, uintptr_t, bool,
                                int32_t, const InitializationValues*, bool);
    using Init = void(*)(uintptr_t, InitializationValues);
    using Update = void(*)(uintptr_t, bool, bool, void*);
    using Cleanup = void(*)(uintptr_t, bool, bool, uintptr_t);
    using Root = void(*)(uintptr_t);
    using Predicate = bool(*)();
    using Context = uintptr_t(*)(uintptr_t, uintptr_t);
    using VisibilityPending = bool(*)(uintptr_t);
    // The verified native collection method uses plain UClass*, avoiding the
    // nontrivial TSubclassOf by-value ABI of the reflected World wrapper.
    using Subsystem = uintptr_t(*)(uintptr_t, uintptr_t);
    using TickType = uint8_t(*)(uintptr_t); // FTickableGameObject subobject this.

    enum class Phase { Inactive, Pending, Creating, Ready, Retiring, Cleaned, Unavailable, Failed, Quarantined };
    enum class Operation { CreateShell, Retain, Initialize, UpdateComponents, CleanupWorld, Release };
    struct Identity
    {
        uintptr_t Bits{};
        int32_t Index = -1, Serial{};
        bool operator==(const Identity&) const = default;
    };
    struct Reference { Identity Object; bool Owned = false, Attempted = false, Uncertain = false; };
    enum ClassId { World, Engine, Level, Settings, CrSettings, GameInstance, Electricity, Grid,
                   Names, Mass, Replication, Wave, ClassCount };
    struct Metadata { const wchar_t* Path; size_t Size; Reference Ref; };
    Metadata metadata[ClassCount] = {
        {L"/Script/Engine.World", sizeof(SDK::UWorld)},
        {L"/Script/Engine.Engine", sizeof(SDK::UEngine)},
        {L"/Script/Engine.Level", sizeof(SDK::ULevel)},
        {L"/Script/Engine.WorldSettings", sizeof(SDK::AWorldSettings)},
        {L"/Script/Chimera.CrWorldSettings", sizeof(SDK::ACrWorldSettings)},
        {L"/Script/Engine.GameInstance", sizeof(SDK::UGameInstance)},
        {L"/Script/Chimera.CrElectricityGraphSubsystem", sizeof(SDK::UCrElectricityGraphSubsystem)},
        {L"/Script/Chimera.CrEntityGridSubsystem", sizeof(SDK::UCrEntityGridSubsystem)},
        {L"/Script/Chimera.CrBuildingCustomNameSubsystem", sizeof(SDK::UCrBuildingCustomNameSubsystem)},
        {L"/Script/MassEntity.MassEntitySubsystem", sizeof(SDK::UMassEntitySubsystem)},
        {L"/Script/MassReplication.MassReplicationSubsystem", sizeof(SDK::UMassReplicationSubsystem)},
        {L"/Script/Chimera.CrEnviroWaveVisualsWorldSubsystem", sizeof(SDK::UCrEnviroWaveVisualsWorldSubsystem)}
    };
    static_assert(sizeof(metadata) / sizeof(Metadata) == ClassCount);
    struct Baseline
    {
        Identity Gameplay, EngineObject, GameInstance;
        uintptr_t ContextBits{};
        uint64_t Generation{}, ContextHash{};
        int32_t ContextCount{};
        bool operator==(const Baseline&) const = default;
    };
    // Intentionally no RAII native owner/destructor. Quarantine is never
    // released by shutdown/static destruction or by a later world transition.
    struct State
    {
        Phase Current = Phase::Inactive;
        void* SessionWorld{};
        uint64_t Generation{};
        uintptr_t Base{}, Array{}, CreateFn{}, InitFn{}, UpdateFn{}, CleanupFn{};
        uintptr_t ContextFn{}, SubsystemFn{}, LockingFn{}, PurgeFn{}, ReachabilityFn{}, TickFn{}, VisibilityPendingFn{};
        Root Retain{}, Release{};
        Predicate GameThread{};
        Reference Private, Subsystems[3], Package, PersistentLevel, WorldSettings;
        uintptr_t Scene{};
        Baseline Before;
        bool CleanupCompleted = false, ReferencesReleased = false;
        const char* Failure = "none";
    } state;

    template<class T> bool Read(uintptr_t p, T& value)
    { return N::ReadMemory(p, &value, sizeof(value)); }
    // No SDK GetByIndex or unchecked pointer walk.
    struct SlotReader { template<class T> bool Read(uintptr_t p, T& v) { return N::ReadMemory(p, &v, sizeof(v)); } };

    bool Identify(uintptr_t object, Identity& out, size_t extent = 0x28)
    {
        if (!N::ValidateObjectIdentity(object, state.Array, extent)) return false;
        int32_t index{}; uint32_t flags{};
        if (!Read(object + 0xc, index) || !Read(object + 8, flags) || (flags & 0x1b400)) return false;
        DefinitionSlots::Slot slot; SlotReader reader;
        if (DefinitionSlots::ReadSlot(reader, state.Array, index, slot) != DefinitionSlots::Status::Success ||
            slot.Object != object || (slot.Flags & 0x10200000)) return false;
        out = {object, index, slot.Serial}; // Zero is observed, never allocated.
        return true;
    }
    bool CheckOwned(Reference& ref)
    {
        if (!ref.Owned || ref.Uncertain || !state.GameThread || !state.GameThread()) return false;
        Identity now;
        if (!Identify(ref.Object.Bits, now) || now.Bits != ref.Object.Bits || now.Index != ref.Object.Index) return false;
        if (now.Serial == ref.Object.Serial) return true;
        // Shipping AllocateSerialNumber assigns a positive serial to an item
        // that had zero, without replacing its UObject. Our uninterrupted native
        // ownership prevents slot recycling; pointer/index/flags are rechecked.
        // Never adopt after release, from an uncertain retain, or from a
        // previously nonzero serial. This does not allocate a serial.
        DefinitionSlots::Slot slot; SlotReader reader;
        if (ref.Object.Serial != 0 || now.Serial <= 0 ||
            DefinitionSlots::ReadSlot(reader, state.Array, now.Index, slot) != DefinitionSlots::Status::Success ||
            slot.Object != now.Bits || slot.Serial != now.Serial || !(slot.Flags & 0x20000000)) return false;
        ref.Object.Serial = now.Serial;
        LOG_INFO("MiniMap: private_capture_world retained_serial_adopted=1 index=%d serial=%d", now.Index, now.Serial);
        return true;
    }
    bool Is(uintptr_t object, ClassId expected)
    {
        Identity id; uintptr_t type{};
        if (!CheckOwned(metadata[expected].Ref) || !Identify(object, id) || !Read(object + 0x10, type)) return false;
        for (unsigned depth = 0; type && depth < 64; ++depth)
        {
            if (!Identify(type, id, sizeof(SDK::UClass))) return false;
            if (type == metadata[expected].Ref.Object.Bits) return true;
            uintptr_t parent{}; if (!Read(type + 0x40, parent) || parent == type) return false;
            type = parent;
        }
        return false;
    }
    bool NoForeignRoot(Reference& ref)
    {
        if (!CheckOwned(ref)) return false;
        const auto& id = ref.Object;
        uint32_t flags{}; DefinitionSlots::Slot slot; SlotReader reader;
        return Read(id.Bits + 8, flags) && !(flags & 2) && // RF_Standalone
            DefinitionSlots::ReadSlot(reader, state.Array, id.Index, slot) == DefinitionSlots::Status::Success &&
            !(slot.Flags & 0x40000000); // RootSet, distinct from our RefCounted bit.
    }
    // Leaf SEH boundary, no C++ objects with destructors. A native interruption
    // does not authorize a retry, cleanup of partial initialization, or refund.
    static const char* OperationName(Operation op)
    {
        switch(op) {
        case Operation::CreateShell: return "create_world";
        case Operation::Retain: return "retain_reference";
        case Operation::Initialize: return "init_world";
        case Operation::UpdateComponents: return "update_world_components";
        case Operation::CleanupWorld: return "cleanup_world";
        case Operation::Release: return "release_reference";
        }
        return "invalid_operation";
    }
    bool Invoke(Operation op, uintptr_t object, uintptr_t* created = nullptr) noexcept
    {
        const char* name = OperationName(op);
        LOG_INFO("MiniMap: private_capture_world step=%s outcome=entered", name);
        __try
        {
            switch (op)
            {
            case Operation::CreateShell:
                {
                    InitializationValues values;
                    *created = reinterpret_cast<Create>(state.CreateFn)(7, false, SDK::FName{}, 0, false, 5, &values, true);
                } break;
            case Operation::Retain: state.Retain(object); break;
            case Operation::Initialize: reinterpret_cast<Init>(state.InitFn)(object, InitializationValues{}); break;
            case Operation::UpdateComponents: reinterpret_cast<Update>(state.UpdateFn)(object, false, false, nullptr); break;
            case Operation::CleanupWorld: reinterpret_cast<Cleanup>(state.CleanupFn)(object, true, true, 0); break;
            case Operation::Release: state.Release(object); break;
            }
            LOG_INFO("MiniMap: private_capture_world step=%s outcome=success evidence=normal_return_only", name);
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
                    ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            LOG_ERROR("MiniMap: private_capture_world step=%s outcome=failure reason=native_memory_interruption code=%lu", name, GetExceptionCode());
            return false;
        }
    }
    bool Hold(uintptr_t object, Reference& ref)
    {
        if (!QuietGC() || !Identify(object, ref.Object)) return Step("retain_reference", false, "GC_or_object_identity_invalid");
        ref.Attempted = true;
        if (!Invoke(Operation::Retain, object)) { ref.Uncertain = true; return false; }
        ref.Owned = true;
        if (!CheckOwned(ref)) { ref.Uncertain = true; return Step("reference_ownership", false, "retained_identity_unverified"); }
        return Step("reference_ownership", true, "retained_identity_verified");
    }
    bool Drop(Reference& ref)
    {
        if (!ref.Owned) return !ref.Attempted;
        if (!QuietGC() || !CheckOwned(ref) || !Invoke(Operation::Release, ref.Object.Bits))
        { ref.Uncertain = true; return false; }
        ref.Owned = false; ref.Attempted = false;
        return true;
    }
    void Quarantine(const char* reason)
    {
        state.Current = Phase::Quarantined;
        LOG_ERROR("MiniMap: private_capture_world state=quarantined reason=%s world_reference_confirmed=%u world_reference_uncertain=%u cleanup=%u released=%u; no retry or shutdown release",
                  reason, unsigned(state.Private.Owned && !state.Private.Uncertain), unsigned(state.Private.Uncertain),
                  unsigned(state.CleanupCompleted), unsigned(state.ReferencesReleased));
    }
    struct QuietGCDetail
    {
        int32_t ExitPurgeReadable = -1, ExitPurgeActive = -1;
        int32_t FlagReadable = -1, FlagActive = -1;
        int32_t Locking = -1, Purge = -1, Reachability = -1;
        const char* Step = "no GC or purge condition detected";
    };
    bool QuietGC(QuietGCDetail* detail = nullptr)
    {
        uint8_t exitPurge{};
        if (!Read(state.Base + 0xe268c10, exitPurge))
        {
            if (detail) { detail->ExitPurgeReadable = 0; detail->Step = "exit purge state is unreadable"; }
            return false;
        }
        if (detail) { detail->ExitPurgeReadable = 1; detail->ExitPurgeActive = exitPurge != 0; }
        if (exitPurge)
        { if (detail) detail->Step = "exit purge is active"; return false; }
        bool gc{};
        if (detail) detail->Step = "garbage collection state is unreadable";
        if (!Read(state.Base + 0xe344998, gc))
        {
            if (detail) detail->FlagReadable = 0;
            return false;
        }
        if (detail) { detail->FlagReadable = 1; detail->FlagActive = gc; detail->Step = "garbage collection is active"; }
        if (gc) return false;
        if (detail) detail->Step = "garbage collection is locking UObject hash tables";
        const bool locking = reinterpret_cast<Predicate>(state.LockingFn)();
        if (detail) detail->Locking = locking;
        if (locking) return false;
        if (detail) detail->Step = "incremental purge is pending";
        const bool purge = reinterpret_cast<Predicate>(state.PurgeFn)();
        if (detail) detail->Purge = purge;
        if (purge) return false;
        if (detail) detail->Step = "incremental reachability analysis is pending";
        const bool reachability = reinterpret_cast<Predicate>(state.ReachabilityFn)();
        if (detail) detail->Reachability = reachability;
        if (reachability) return false;
        if (detail) detail->Step = "no GC or purge condition detected";
        return true;
    }
    bool Collection(uintptr_t world, bool initialized)
    {
        const auto collection = world + 0x9f0;
        uintptr_t outer{}; bool populating{}, iterating{};
        int32_t entries{}, free{}, arrays{}, arraysFree{};
        // Matching PDB: both 0x50-byte maps use TSparseArray at +0;
        // Data.ArrayNum +8 and NumFreeIndices +0x34. No map iteration.
        return Read(collection + 0xb0, outer) &&
            Read(world + 0x9f0 + 0xb8, populating) && Read(world + 0x9f0 + 0xb9, iterating) &&
            Read(collection + 8 + 8, entries) && Read(collection + 8 + 0x34, free) &&
            Read(collection + 0x58 + 8, arrays) && Read(collection + 0x58 + 0x34, arraysFree) &&
            entries >= 0 && free >= 0 && free <= entries && arrays >= 0 && arraysFree >= 0 && arraysFree <= arrays &&
            (initialized || (entries == free && arrays == arraysFree)) &&
            outer == (initialized ? world : 0) && !populating && !iterating;
    }
    enum class LookupStatus { Invalid, Absent, Present };
    struct LookupResult { LookupStatus Status = LookupStatus::Invalid; uintptr_t Object{}; };
    LookupResult FindSubsystem(uintptr_t world, ClassId cls)
    {
        if (!CheckOwned(metadata[cls].Ref) || !Is(world, World) || !Collection(world, true) || !QuietGC()) return {};
        const auto object = reinterpret_cast<Subsystem>(state.SubsystemFn)(world + 0x9f0, metadata[cls].Ref.Object.Bits);
        // Revalidate even for an absent result; indeterminate inspection must
        // never masquerade as successful proof of dependency absence.
        if (!CheckOwned(metadata[cls].Ref) || !Is(world, World) || !Collection(world, true) || !QuietGC()) return {};
        if (!object) return {LookupStatus::Absent, 0};
        uintptr_t outer{}, collection{};
        if (!Is(object, cls) || !Read(object + 0x20, outer) || outer != world ||
            !Read(object + 0x28, collection) || collection != world + 0x9f0) return {};
        return {LookupStatus::Present, object};
    }
    bool GetBaseline(Baseline& b)
    {
        // Gameplay identity is an isolation fence, never a discovery prerequisite.
        const auto game = reinterpret_cast<uintptr_t>(MiniMapMap::GetWorld());
        uintptr_t engine{}, gi{}, storage{}, current{};
        uint8_t tearing{}, initialized{}; int32_t maximum{};
        if (game != reinterpret_cast<uintptr_t>(state.SessionWorld) || !state.Generation ||
            !Read(state.Base + 0xe4f88e8, engine) || !Is(engine, Engine) || !Is(game, World) ||
            !Read(game + 0x18d, tearing) || (tearing & 0x20) ||
            !Read(game + 0x18b, initialized) || !(initialized & 0x40) ||
            !Identify(game, b.Gameplay) || !Identify(engine, b.EngineObject) ||
            !Read(game + 0x230, gi) || !Is(gi, GameInstance) || !Identify(gi, b.GameInstance))
            return Step("gameplay_baseline", false, "session_world_engine_or_instance_invalid");
        b.Generation = state.Generation;
        b.ContextBits = reinterpret_cast<Context>(state.ContextFn)(engine, game);
        if (!b.ContextBits || !Read(b.ContextBits + 0x2c0, current) || current != game ||
            !Read(engine + 0x11c8, storage) || !Read(engine + 0x11d0, b.ContextCount) ||
            !Read(engine + 0x11d4, maximum) || b.ContextCount < 1 ||
            b.ContextCount > maximum || maximum > 4096 ||
            !N::IsMemoryRangeReadable(storage, size_t(b.ContextCount) * 8, 8))
            return Step("gameplay_baseline", false, "context_header_or_current_world_invalid");
        b.ContextHash = 1469598103934665603ULL;
        for (int32_t i = 0; i < b.ContextCount; ++i)
        {
            uintptr_t context{};
            if (!Read(storage + size_t(i) * 8, context) || !context ||
                !N::IsMemoryRangeReadable(context, 0x2c8, 8) || !Read(context + 0x2c0, current))
                return Step("gameplay_baseline", false, "context_entry_unreadable");
            b.ContextHash = ((b.ContextHash ^ context) * 1099511628211ULL ^ current) * 1099511628211ULL;
        }
        return true;
    }
    bool Unchanged()
    { Baseline after; return GetBaseline(after) && after == state.Before; }
    bool PrivateState(bool initialized)
    {
        const auto w = state.Private.Object.Bits;
        uint8_t type{}, a{}, b{}, c{}, ever{}; uintptr_t scene{}, gameplayScene{}, gameplayPackage{}, privatePackage{}, level{}, gi{}, net{}, owner{}, settings{}, collection{};
        if (w == state.Before.Gameplay.Bits || !CheckOwned(state.Private) || !Is(w, World) || !NoForeignRoot(state.Private) ||
            !CheckOwned(state.Package) || !NoForeignRoot(state.Package) ||
            !Read(w + 0x20, privatePackage) || privatePackage != state.Package.Object.Bits ||
            !Read(state.Before.Gameplay.Bits + 0x20, gameplayPackage) || privatePackage == gameplayPackage ||
            !Read(w + 0x18a, type) || type != 7 || !Read(w + 0x18b, a) ||
            !Read(w + 0x18c, b) || !Read(w + 0x18d, c) || !Read(w + 0x18f, ever) ||
            bool(a & 0x40) != initialized || (b & 0x80) || (c & 0x21) ||
            bool(ever & 2) != initialized || !Read(w + 0x258, scene) || bool(scene) != initialized ||
            !Read(w + 0x230, gi) || gi || !Read(w + 0x38, net) || net ||
            reinterpret_cast<Context>(state.ContextFn)(state.Before.EngineObject.Bits, w) ||
            !CheckOwned(state.PersistentLevel) || !CheckOwned(state.WorldSettings) ||
            !Read(w + 0x30, level) || level != state.PersistentLevel.Object.Bits ||
            !Read(state.PersistentLevel.Object.Bits + 0xc0, owner) || owner != w ||
            !Read(state.PersistentLevel.Object.Bits + 0x2c8, settings) || settings != state.WorldSettings.Object.Bits ||
            !Read(state.WorldSettings.Object.Bits + 0x10, collection) || collection != metadata[CrSettings].Ref.Object.Bits ||
            !Collection(w, initialized)) return false;
        if (initialized && ((state.Scene && state.Scene != scene) || !N::IsMemoryRangeReadable(scene, 8, 8) ||
            !Read(state.Before.Gameplay.Bits + 0x258, gameplayScene) || !gameplayScene || scene == gameplayScene)) return false;
        if (initialized) state.Scene = scene;
        return true;
    }
    bool CleanupPreconditions()
    {
        const auto world = state.Private.Object.Bits;
        int32_t streamingLevels{}, consideredLevels{}, levelsToProcess{}, beingConsidered{};
        if (!QuietGC() || !Unchanged() || !PrivateState(true) ||
            !Read(world + 0x98, streamingLevels) || streamingLevels != 0 ||
            !Read(world + 0xa8, consideredLevels) || consideredLevels != 0 ||
            !Read(world + 0xb8, levelsToProcess) || levelsToProcess != 0 ||
            !Read(world + 0xc0, beingConsidered) || beingConsidered != 0 ||
            !state.VisibilityPendingFn || reinterpret_cast<VisibilityPending>(state.VisibilityPendingFn)(world))
        {
            state.Failure = "cleanup_preconditions_streaming_visibility_context_or_ownership";
            return false;
        }
        // The private scene is observed through its owning world. No scene
        // pointer is exported by this owner; external renderer users are
        // not exhaustively enumerable here. Its non-sharing is supported by
        // the unchanged engine context baseline and this owner's ownership
        // and lifecycle invariants, not by a global reference enumeration.
        return true;
    }
    bool InspectSubsystems()
    {
        const auto w = state.Private.Object.Bits;
        const ClassId ids[] = {Electricity, Grid, Names};
        for (size_t i = 0; i < 3; ++i)
        {
            const auto subsystem = FindSubsystem(w, ids[i]);
            if (subsystem.Status != LookupStatus::Present || !Hold(subsystem.Object, state.Subsystems[i])) return false;
            LOG_INFO("MiniMap: private_capture_world subsystem=%u index=%d serial=%d outer_private=1 retained=1",
                     unsigned(ids[i]), state.Subsystems[i].Object.Index, state.Subsystems[i].Object.Serial);
        }
        uintptr_t graphManager{}, graphControl{}, gridManager{}, gridControl{}; bool tickInitialized{};
        const auto graph = state.Subsystems[0].Object.Bits, grid = state.Subsystems[1].Object.Bits;
        if (!Read(graph + 0x290, graphManager) || !Read(graph + 0x298, graphControl) || graphManager || graphControl ||
            !Read(grid + 0xf0, gridManager) || !Read(grid + 0xf8, gridControl) || gridManager || gridControl ||
            !Read(graph + 0x38, tickInitialized) || !tickInitialized) return false;
        for (const auto dependency : {Mass, Replication, Wave})
            if (FindSubsystem(w, dependency).Status != LookupStatus::Absent) return false;
        LOG_INFO("MiniMap: private_capture_world isolated=1 Mass=absent replication=absent wave=absent graph_manager=absent grid_manager=absent GameInstance=absent; custom_name_save_binding=structural_null_GameInstance_path_not_delegate_enumeration");
        return true;
    }

    static bool Step(const char* name, bool success, const char* reason = "verified")
    {
        LOG_INFO("MiniMap: private_capture_world step=%s outcome=%s reason=%s", name,
                 success ? "success" : "failure", reason);
        return success;
    }
    // CL-127004 code bytes generated from the matching shipping PE, included
    // separately for review. RSDS identity AND every entry fingerprint required.
    struct Entry { uintptr_t Rva; std::array<uint8_t, 16> Bytes; };
#include "PrivateCaptureWorldBindings.inl"
    bool RendererAvailable()
    {
        const auto& renderer = MiniMapReadbackNative::GetBindings();
        uint32_t gpuCount{}; uintptr_t dynamicRHI{};
        uint8_t initialized{}, threaded{}, multithreaded{};
        if (!renderer.GpuCount || !Read(renderer.GpuCount, gpuCount) || gpuCount == 0 || gpuCount > 8)
            return Step("renderer_availability", false, "GPU_count_missing_unreadable_or_out_of_range");
        if (!renderer.DynamicRHI || !Read(renderer.DynamicRHI, dynamicRHI) || !dynamicRHI)
            return Step("renderer_availability", false, "DynamicRHI_missing_unreadable_or_null");
        if (renderer.Multithreaded < 0x10 || !Read(renderer.Multithreaded - 0x10, initialized) || !initialized)
            return Step("renderer_availability", false, "RHI_initialization_missing_unreadable_or_inactive");
        if (!renderer.Threaded || !Read(renderer.Threaded, threaded) || !threaded ||
            !Read(renderer.Multithreaded, multithreaded) || !multithreaded)
            return Step("renderer_availability", false, "render_or_RHI_threading_missing_unreadable_or_inactive");
        return Step("renderer_availability", true, "checked_RHI_globals_scene_verified_after_init");
    }
    bool Preflight()
    {
        state.Failure = "matching_build_or_native_facilities";
        state.Base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (!MiniMapReadbackNative::MatchesCaptureRetirementBuild() || !N::IsInitialized()) return false;
        state.Failure = "native_entry_fingerprint_or_execution_page";
        for (const auto& entry : entries)
        {
            const auto p = state.Base + entry.Rva; std::array<uint8_t, 16> bytes{};
            MEMORY_BASIC_INFORMATION page{};
            if (!N::ReadMemory(p, bytes.data(), bytes.size()) || bytes != entry.Bytes ||
                !VirtualQuery(reinterpret_cast<void*>(p), &page, sizeof(page)) || page.State != MEM_COMMIT ||
                !(page.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ||
                (page.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            {
                LOG_ERROR("MiniMap: private_capture_world step=native_bindings outcome=failure reason=fingerprint_or_execution_page_mismatch rva=%llx", static_cast<unsigned long long>(entry.Rva));
                return false;
            }
        }
        Step("native_bindings", true, "matching_build_and_15_entry_fingerprints");
        state.CreateFn = state.Base + entries[0].Rva; state.InitFn = state.Base + entries[1].Rva;
        state.UpdateFn = state.Base + entries[2].Rva; state.CleanupFn = state.Base + entries[3].Rva;
        state.ContextFn = state.Base + entries[4].Rva; state.SubsystemFn = state.Base + entries[5].Rva;
        state.LockingFn = state.Base + entries[6].Rva; state.PurgeFn = state.Base + entries[7].Rva;
        state.ReachabilityFn = state.Base + entries[8].Rva; state.TickFn = state.Base + entries[9].Rva;
        state.VisibilityPendingFn = state.Base + entries[10].Rva;
        const auto& native = MiniMapSmelterCapture::GetBindings();
        // The existing resolver deliberately binds the GExitPurge-guarded
        // Release wrapper, not the direct ReleaseRef entry (entries[12]).
        // Both the wrapper and its branch target are fingerprinted above.
        const bool retainVerified = native.Retain == state.Base + entries[11].Rva;
        const bool releaseVerified = native.Release == state.Base + entries[13].Rva;
        const bool threadVerified = native.GameThread == state.Base + entries[14].Rva;
        LOG_INFO("MiniMap: private_capture_world prerequisites AddRef=%s Release=%s GameThread=%s",
                 retainVerified ? "verified" : (native.Retain ? "mismatch" : "missing"),
                 releaseVerified ? "verified_guarded_wrapper" : (native.Release ? "mismatch" : "missing"),
                 threadVerified ? "verified" : (native.GameThread ? "mismatch" : "missing"));
        state.Failure = "existing_AddRef_binding_missing_or_mismatch";
        if (!retainVerified) return false;
        state.Failure = "existing_Release_wrapper_binding_missing_or_mismatch";
        if (!releaseVerified) return false;
        state.Failure = "existing_GameThread_binding_missing_or_mismatch";
        if (!threadVerified) return false;
        state.GameThread = reinterpret_cast<Predicate>(native.GameThread);
        state.Retain = reinterpret_cast<Root>(native.Retain); state.Release = reinterpret_cast<Root>(native.Release);
        state.Failure = "wrong_game_thread";
        const bool gameThread = state.GameThread();
        if (!Step("game_thread", gameThread, gameThread ? "verified" : "wrong_thread")) return false;
        // The verified wrapper reads GExitPurge at RVA 0xe268c10 and skips
        // ReleaseRef when set. Reject teardown before acquiring any references.
        state.Failure = "exit_purge_active_or_unreadable";
        uint8_t exitPurge{};
        if (!Read(state.Base + 0xe268c10, exitPurge) || exitPurge) return false;
        state.Failure = "GC_not_quiet";
        if (!QuietGC()) return false;
        // Empty world initialization needs renderer facilities, not readback
        // adapter Preflight or its mutable GPU-count snapshot. These globals
        // are resolved by the existing build-verified readback binding resolver.
        state.Failure = "renderer_globals_missing_unreadable_or_inactive";
        if (!RendererAvailable()) return false;
        state.Failure = "checked_object_table_anchor_unavailable";
        const auto anchor = MiniMapReadbackNative::ResolveOptionalFunction(MiniMapBuildingRepresentation::Patterns::ObjectArrayAnchor, false);
        uint8_t op[3]{}; int32_t displacement{};
        if (!anchor || !N::ReadMemory(anchor + 0x15, op, 3) || op[0] != 0x48 || op[1] != 0x8d || op[2] != 0x0d ||
            !Read(anchor + 0x18, displacement)) return false;
        state.Array = uintptr_t(int64_t(anchor) + 0x1c + displacement);
        if (!N::IsMemoryRangeReadable(state.Array, 0x30, 8)) return false;
        for (auto& cls : metadata)
        {
            state.Failure = "GC_changed_during_class_acquisition";
            if (!QuietGC()) return Step("gc_safety", false, state.Failure);
            state.Failure = "resident_native_class_missing_or_layout_mismatch";
            const auto object = MiniMapBuildingRepresentation::AcquireResidentMetadata(cls.Path, false, cls.Size);
            if (!object) { LOG_ERROR("MiniMap: private_capture_world step=native_class outcome=failure reason=resident_class_or_layout_unavailable class=%ls", cls.Path); return false; }
            // The helper already transferred exactly one reference. Record it
            // before further checks so failures cannot lose known ownership.
            cls.Ref.Object.Bits = object; cls.Ref.Owned = true; cls.Ref.Attempted = true;
            if (!Identify(object, cls.Ref.Object, sizeof(SDK::UClass)))
            { Step("native_class", false, "retained_class_identity_invalid"); return false; }
            Step("native_class", true, "resident_class_retained_and_layout_checked");
        }
        uintptr_t configured{};
        state.Failure = "gameplay_baseline_failed";
        if (!GetBaseline(state.Before)) return false;
        Step("gameplay_baseline", true, "session_identity_and_contexts_copied");
        state.Failure = "WorldSettings_class_configuration_invalid";
        const bool configuredReadable = Read(state.Before.EngineObject.Bits + 0x190, configured);
        const int32_t configuredNull = configuredReadable ? int32_t(configured == 0) : -1;
        const int32_t configuredMatches = configuredReadable && configuredNull == 0
            ? int32_t(configured == metadata[CrSettings].Ref.Object.Bits) : -1;
        if (!configuredReadable || configuredNull == 1 || configuredMatches != 1)
        {
            LOG_ERROR("MiniMap: private_capture_world preflight_detail category=world_settings step=%s configured_class_readable=%d configured_class_null=%d exact_CrWorldSettings_match=%d",
                      !configuredReadable ? "configured_class_unreadable" : configuredNull ? "configured_class_null" : "configured_class_mismatch",
                      int(configuredReadable), int(configuredNull), int(configuredMatches));
            return false;
        }
        Step("world_configuration", true, "Inactive_scenes_only_CrWorldSettings_skip_init_no_context_no_root");
        state.Failure = "GC_not_quiet";
        QuietGCDetail gcDetail;
        const bool gcQuiet = QuietGC(&gcDetail);
        if (!gcQuiet)
            LOG_ERROR("MiniMap: private_capture_world preflight_detail category=gc step=%s gc_flag_readable=%d gc_flag_active=%d locking_hash_tables=%d incremental_purge_pending=%d incremental_reachability_pending=%d",
                      gcDetail.Step, gcDetail.FlagReadable, gcDetail.FlagActive, gcDetail.Locking,
                      gcDetail.Purge, gcDetail.Reachability);
        if (!Step("gc_safety", gcQuiet, gcQuiet ? "quiet" : gcDetail.Step)) return false;
        LOG_INFO("MiniMap: private_capture_world step=preflight outcome=success bindings=15 game_thread=1 GC=quiet gameplay_index=%d serial=%d engine_index=%d generation=%llu contexts=%d context_hash=%llu GameInstance_index=%d baseline_subsystem_queries=none save_association=GameInstance_identity_only WorldSettings=CrWorldSettings",
                 state.Before.Gameplay.Index, state.Before.Gameplay.Serial, state.Before.EngineObject.Index,
                 static_cast<unsigned long long>(state.Before.Generation), state.Before.ContextCount,
                 static_cast<unsigned long long>(state.Before.ContextHash), state.Before.GameInstance.Index);
        state.Failure = "none";
        return true;
    }
    bool ReleaseMetadata()
    { for (auto& cls : metadata) if (!Drop(cls.Ref)) return false; return true; }
    #include "PrivateCaptureWorldActors.inl"

    void Run()
    {
        state.Current = Phase::Creating;
        Step("owner_readiness", true, "capture_worker_session_notification");
        if (!Preflight())
        {
            if (!ReleaseMetadata()) { Quarantine("preflight_reference_release_uncertain"); return; }
            state.Current = Phase::Unavailable;
            LOG_INFO("MiniMap: private_capture_world state=unavailable reason=%s", state.Failure);
            LOG_ERROR("MiniMap: private_capture_world step=preflight outcome=failure reason=%s world_creation_attempts=0; reconsider_only_at_next_session_transition", state.Failure);
            return;
        }
        uintptr_t world{};
        LOG_INFO("MiniMap: private_capture_world step=private_package outcome=entered reason=CreateWorld_prepares_dedicated_package_from_null_argument");
        if (!Invoke(Operation::CreateShell, 0, &world) || !world)
        {
            Step("create_world_result", false, "null_or_interrupted_no_validated_ownership");
            Step("private_package", false, "no_validated_shell_package");
            // CreateWorld did not transfer a validated object. Do not scan for
            // an unreturned shell or guess ownership/rollback of native effects.
            if (!ReleaseMetadata()) { Quarantine("creation_interrupted_metadata_retained"); return; }
            state.Current = Phase::Failed;
            LOG_ERROR("MiniMap: private_capture_world state=failed reason=create_null_or_interrupted no_retry=1");
            return;
        }
        Step("create_world_result", true, "non_null_result_ownership_not_yet_established");
        if (!Is(world, World) || !Hold(world, state.Private)) { Quarantine("shell_identity_or_retention_uncertain"); return; }
        uintptr_t package{}, level{}, settings{};
        if (!Read(world + 0x20, package) || !Hold(package, state.Package) ||
            !Read(world + 0x30, level) || !Is(level, Level) || !Hold(level, state.PersistentLevel) ||
            !Read(level + 0x2c8, settings) || !Is(settings, CrSettings) || !Hold(settings, state.WorldSettings) ||
            !QuietGC() || !Unchanged() || !PrivateState(false)) { Quarantine("unexpected_retained_shell"); return; }
        Step("private_package", true, "owned_dedicated_non_rooted_package_verified");
        LOG_INFO("MiniMap: private_capture_world shell=retained world_index=%d serial=%d package_index=%d level_index=%d settings_index=%d scene=absent context=absent root=absent",
                 state.Private.Object.Index, state.Private.Object.Serial, state.Package.Object.Index, state.PersistentLevel.Object.Index, state.WorldSettings.Object.Index);
        if (!Invoke(Operation::Initialize, world)) { Quarantine("init_world_interrupted"); return; }
        if (!Step("init_world_postconditions", QuietGC() && Unchanged() && PrivateState(true),
                  "requires_retained_initialized_private_world_and_scene_before_component_update"))
        { Quarantine("init_world_postconditions_failed"); return; }
        if (!Invoke(Operation::UpdateComponents, world)) { Quarantine("update_world_components_interrupted"); return; }
        if (!Step("renderer_scene", QuietGC() && Unchanged() && PrivateState(true),
                  "requires_owned_initialized_world_private_scene_and_unchanged_gameplay"))
        { Quarantine("initialized_world_or_scene_postconditions_failed"); return; }
        if (!Step("isolation", InspectSubsystems() && Unchanged() && QuietGC(),
                  "requires_inert_private_subsystems_absent_gameplay_dependencies_unchanged_contexts"))
        { Quarantine("subsystem_or_gameplay_isolation_failed"); return; }
        state.Current = Phase::Ready;
        Step("persistent_ownership", true, "retained_world_package_level_settings_subsystems_and_classes");
        LOG_INFO("MiniMap: private_capture_world state=ready generation=%llu persistent_reuse=1", static_cast<unsigned long long>(state.Generation));
    }
    void Retire()
    {
        // A never-submitted candidate can retire synchronously at this boundary.
        // In-flight capture/readback must retain its entire dependent private world.
        if (blueprint.Active && !blueprint.CaptureUsed) BeginBlueprintRetirement();
        if (blueprint.Active) { BlueprintQuarantine("session_end_with_private_work"); return; }
        if (state.Current == Phase::Quarantined) return;
        if (!Step("cleanup_admission", state.GameThread && state.GameThread() && CleanupPreconditions(),
                  "requires_owned_isolated_world_unchanged_session_quiet_GC_no_streaming_or_visibility_no_private_work"))
        { Quarantine("cleanup_admission_failed"); return; }

        state.Current = Phase::Retiring;
        const auto world = state.Private.Object.Bits;
        if (!Invoke(Operation::CleanupWorld, world)) { Quarantine("cleanup_interrupted"); return; }
        uint8_t flags{}; bool ticking{}; uintptr_t gi{}, net{}, scene{};
        if (!QuietGC() || !Unchanged() || !CheckOwned(state.Private) || !Read(world + 0x18b, flags) || (flags & 0x40) ||
            !Collection(world, false) || !CheckOwned(state.Subsystems[0]) ||
            !Read(state.Subsystems[0].Object.Bits + 0x38, ticking) || ticking ||
            reinterpret_cast<TickType>(state.TickFn)(state.Subsystems[0].Object.Bits + 0x30) != 2 ||
            !Read(world + 0x230, gi) || gi || !Read(world + 0x38, net) || net ||
            !Read(world + 0x258, scene) || scene != state.Scene || !NoForeignRoot(state.Private) ||
            !NoForeignRoot(state.Package) || !Unchanged() ||
            reinterpret_cast<Context>(state.ContextFn)(state.Before.EngineObject.Bits, world) || !QuietGC())
        { Quarantine("cleanup_postconditions_disagree"); return; }
        for (auto& ref : state.Subsystems)
        {
            uintptr_t collection{};
            if (!CheckOwned(ref) || !Read(ref.Object.Bits + 0x28, collection) || collection)
            { Quarantine("subsystem_collection_link_not_cleared"); return; }
        }
        state.CleanupCompleted = true;
        Step("cleanup_postconditions", true, "collection_deinitialized_tick_disabled_context_absent_gameplay_unchanged");
        LOG_INFO("MiniMap: private_capture_world cleanup_completed=1 collection_outer=cleared electricity_initialized=0 tick_type=Never scene_gc_owned=1 gameplay_unchanged=1; no manual destruction");
        for (auto& ref : state.Subsystems) if (!Drop(ref)) { Quarantine("subsystem_release_uncertain"); return; }
        if (!Drop(state.WorldSettings) || !Drop(state.PersistentLevel) || !Drop(state.Package))
        { Quarantine("shell_reference_release_uncertain"); return; }
        if (!Drop(state.Private) || !ReleaseMetadata()) { Quarantine("reference_release_uncertain"); return; }
        state.ReferencesReleased = true; state.Current = Phase::Cleaned;
        LOG_INFO("MiniMap: private_capture_world state=cleaned cleanup_invoked=1 references_released=1 UObject_retirement=not_observed renderer_retirement=not_independently_verifiable");
        ObserveRetirement();
    }
    void ObserveRetirement()
    {
        // Boundary-only copied slot observation; never dereference released objects.
        if (!state.ReferencesReleased) return;
        DefinitionSlots::Slot slot; SlotReader reader;
        const bool observed = state.GameThread && state.GameThread() && QuietGC() &&
            DefinitionSlots::ReadSlot(reader, state.Array, state.Private.Object.Index, slot) == DefinitionSlots::Status::Success &&
            (!slot.Object || slot.Object != state.Private.Object.Bits ||
             (state.Private.Object.Serial > 0 && slot.Serial != state.Private.Object.Serial));
        LOG_INFO("MiniMap: private_capture_world step=uobject_retirement outcome=%s reason=copied_slot_at_session_boundary", observed ? "success" : "unobserved");
        LOG_INFO("MiniMap: private_capture_world step=renderer_retirement outcome=unobserved reason=no_independent_scene_retirement_contract");
    }
    void Arm(void* world, uint64_t generation)
    {
        if (state.Current == Phase::Quarantined || state.Current == Phase::Failed) return;
        if (state.SessionWorld == world && state.Generation == generation) return;
        if (state.Current == Phase::Ready || state.Current == Phase::Creating || state.Current == Phase::Retiring)
        { Quarantine("session_replaced_without_safe_retirement"); return; }
        ObserveRetirement();
        ResetBlueprintSession();
        state = {};
        for (auto& cls : metadata) cls.Ref = {};
        state.SessionWorld = world; state.Generation = generation;
        state.Current = Phase::Pending;
        LOG_INFO("MiniMap: private_capture_world state=pending generation=%llu", static_cast<unsigned long long>(generation));
    }
};

void PrivateCaptureWorld::OnSessionReady(void* world, uint64_t generation) noexcept
{
    try { if (!impl) impl = new Implementation; impl->Arm(world, generation); }
    catch (...) { if (impl) impl->Quarantine("session_notification_exception");
        else LOG_ERROR("MiniMap: private_capture_world state=failed step=owner_readiness outcome=failure reason=owner_allocation_failed"); }
}
void PrivateCaptureWorld::Service() noexcept
{
    if (!impl) return;
    if (impl->state.Current == Implementation::Phase::Ready)
    {
        try { impl->ServiceBlueprint(); } catch (...) { impl->BlueprintQuarantine("service_exception"); }
        return;
    }
    if (impl->state.Current != Implementation::Phase::Pending) return;
    try {
        if (!MiniMapMap::GameplayReady() || MiniMapMap::GetWorld() != impl->state.SessionWorld)
        {
            impl->state.Current = Implementation::Phase::Unavailable;
            Implementation::Step("owner_readiness", false, "session_not_admitted_reconsider_at_session_transition");
            LOG_INFO("MiniMap: private_capture_world state=unavailable");
            return;
        }
        impl->Run();
    } catch (...) { impl->Quarantine("lifecycle_exception"); }
}
void PrivateCaptureWorld::OfferCandidate(const Candidate& candidate, uintptr_t type, uint32_t building, void* gameplay) noexcept
{
    if (!impl) return;
    try { impl->OfferBlueprint(candidate, type, building, gameplay); }
    catch (...) { impl->BlueprintCandidateException(); }
}
void PrivateCaptureWorld::ObserveProduction(const Candidate& candidate, const unsigned char* rgba, size_t bytes) noexcept
{
    if (!impl) return;
    try { impl->CompareProduction(candidate, rgba, bytes); }
    catch (...) { LOG_WARN("MiniMap: private_blueprint_actor step=comparison outcome=failure reason=CPU_comparison_exception"); }
}
bool PrivateCaptureWorld::ValidateReady(void* world, uint64_t generation) noexcept
{
    if (!impl || impl->state.Current != Implementation::Phase::Ready) return false;
    try {
        if (world == impl->state.SessionWorld && generation == impl->state.Generation &&
            impl->state.GameThread && impl->state.GameThread() && impl->QuietGC() &&
            impl->Unchanged() && impl->PrivateState(true)) return true;
        impl->Quarantine("request_session_or_persistent_ownership_invalid");
    } catch (...) { impl->Quarantine("ownership_validation_exception"); }
    return false;
}
void PrivateCaptureWorld::EndSession(void* world) noexcept
{
    if (!impl || world != impl->state.SessionWorld) return;
    try {
        if (impl->state.Current == Implementation::Phase::Ready) impl->Retire();
        else if (impl->state.Current == Implementation::Phase::Pending) {
            impl->state.Current = Implementation::Phase::Unavailable;
            Implementation::Step("owner_readiness", false, "session_ended_before_service");
            LOG_INFO("MiniMap: private_capture_world state=unavailable");
        }
        else impl->ObserveRetirement();
    } catch (...) { impl->Quarantine("retirement_exception"); }
}
}
#endif
