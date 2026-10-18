#if defined(MODLOADER_CLIENT_BUILD)

#include "Map/Buildings/BuildingSpriteRenderer.h"
#include "Map/Buildings/BuildingCache.h"
#include "Map/Map.h"
#include "Native/NativeApi.h"
#include "Native/DefinitionAcquisitionCore.h"
#include "Native/ReadbackNativeAdapter.h"
#include "Native/CaptureRetirementCore.h"
#include "Map/Sprites/CapturePng.h"
#include "Map/Sprites/SceneCaptureConfiguration.h"
#include "plugin_helpers.h"

#include <Engine_classes.hpp>
#include <SDK/Chimera_classes.hpp>
#include <SDK/MassEntity_classes.hpp>
#include <SDK/MassReplication_classes.hpp>
#include <Windows.h>

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
    namespace N = MiniMapNative;
    namespace Slots = MiniMapDefinitionAcquisition;

    // These offsets and native arguments are the existing CL-127004 world contract.
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
    static_assert(sizeof(SDK::FName) == 8 && std::is_trivially_destructible_v<SDK::FName>);

    struct ObjectIdentity
    {
        uintptr_t Bits{};
        int32_t Index = -1, Serial{};
        bool operator==(const ObjectIdentity&) const = default;
    };
    enum class Ownership { Empty, Held, Uncertain };
    struct NativeReference
    {
        ObjectIdentity Object;
        Ownership State = Ownership::Empty;
    };
    enum class WorldPhase { Inactive, Preparing, Ready, Retiring, Released, Unavailable, Quarantined };
    enum class Operation { Create, Retain, Initialize, Update, Cleanup, Release, Context, Subsystem, TickType, Visibility };

    // Optional stack-only evidence from the original observation, never a reread.
    struct ReadinessDiagnostic
    {
        const char* Predicate = "native world phase";
        const char* Detail = "validation/observation failed";
        uint64_t Expected{}, Current{};
        bool ValuesAvailable = false;
        bool TransientGC = false; // Set only by a successful observation of active GC; never by observation failure.
        const char* Outcome = "observation_or_validation_failed";
        uint64_t Address{};
        void Stage(const char* predicate)
        {
            Predicate = predicate; Detail = "observation/validation failed";
            Outcome = "observation_or_validation_failed"; Address = 0; ValuesAvailable = false; Expected = Current = 0;
        }
        bool Observe(bool succeeded, const char* detail, const char* outcome = "native_observation_failed", uint64_t address = 0)
        {
            if (!succeeded) { Detail = detail; Outcome = outcome; Address = address; ValuesAvailable = false; }
            return succeeded;
        }
        bool Check(bool passed, const char* detail, uint64_t expected = 1, uint64_t current = 0)
        {
            if (!passed) { Detail = detail; Expected = expected; Current = current; ValuesAvailable = true; Outcome = "unsafe_value"; Address = 0; }
            return passed;
        }
    };

    template<class T> bool Read(uintptr_t address, T& value)
    { return N::ReadMemory(address, &value, sizeof(value)); }
    template<class T> bool Read(uintptr_t address, T& value, ReadinessDiagnostic* diagnostic)
    {
        const bool succeeded = Read(address, value);
        return !diagnostic ? succeeded : diagnostic->Observe(succeeded, "memory read", "read_failed", address);
    }
    struct SlotReader
    {
        ReadinessDiagnostic* Diagnostic = nullptr;
        template<class T> bool Read(uintptr_t address, T& value)
        { return ::Read(address, value, Diagnostic); }
    };
    bool SlotObserved(Slots::Status status, ReadinessDiagnostic* diagnostic)
    {
        if (status == Slots::Status::Success) return true;
        // A failed underlying read has already recorded its exact address.
        if (diagnostic && std::strcmp(diagnostic->Outcome, "read_failed") != 0)
        {
            diagnostic->Check(false, Slots::Label(status), uint64_t(Slots::Status::Success), uint64_t(status));
            diagnostic->Outcome = "slot_validation_failed";
        }
        return false;
    }

    bool GameThread(ReadinessDiagnostic* diagnostic = nullptr) noexcept
    {
        const auto* api = N::Get();
        __try
        {
            if (!api || !api->player.isGameThread)
            {
                if (diagnostic) diagnostic->Observe(false, "game-thread facility unavailable", "facility_unavailable");
                return false;
            }
            const bool gameThread = api->player.isGameThread();
            return !diagnostic ? gameThread : diagnostic->Check(gameThread, "game-thread admission", 1, gameThread);
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            if (diagnostic) diagnostic->Observe(false, "game-thread observation interrupted");
            return false;
        }
    }

    bool Facilities(ReadinessDiagnostic* diagnostic = nullptr)
    {
        const auto* api = N::Get();
        const auto facility = [&](bool available, const char* name)
        { return !diagnostic ? available : diagnostic->Check(available, name, 1, available ? 1 : 0); };
        return facility(N::IsInitialized(), "facility unavailable: native API initialization") &&
            facility(api != nullptr, "facility unavailable: native API table") &&
            GameThread(diagnostic) &&
            facility(api->asset.objectArray != nullptr, "facility unavailable: object array") &&
            facility(api->asset.retainObject != nullptr, "facility unavailable: retain object") &&
            facility(api->asset.releaseObject != nullptr, "facility unavailable: release object") &&
            facility(api->asset.exitPurge != nullptr, "facility unavailable: exit-purge flag") &&
            facility(api->asset.activeGarbageCollection != nullptr, "facility unavailable: active-GC flag") &&
            facility(api->player.engineStorage != nullptr, "facility unavailable: engine storage") &&
            facility(api->world.createWorld != nullptr, "facility unavailable: create world") &&
            facility(api->world.initWorld != nullptr, "facility unavailable: initialize world") &&
            facility(api->world.updateWorldComponents != nullptr, "facility unavailable: update world components") &&
            facility(api->world.cleanupWorld != nullptr, "facility unavailable: cleanup world") &&
            facility(api->world.getWorldContextFromWorld != nullptr, "facility unavailable: world-context lookup") &&
            facility(api->world.getSubsystemInternal != nullptr, "facility unavailable: subsystem lookup") &&
            facility(api->world.isGarbageCollectingAndLocking != nullptr, "facility unavailable: GC/hash-table predicate") &&
            facility(api->world.isIncrementalPurgePending != nullptr, "facility unavailable: incremental-purge predicate") &&
            facility(api->world.isIncrementalReachabilityPending != nullptr, "facility unavailable: incremental-reachability predicate") &&
            facility(api->world.getTickableTickType != nullptr, "facility unavailable: tickable type predicate") &&
            facility(api->world.isVisibilityRequestPending != nullptr, "facility unavailable: visibility predicate");
    }

    bool QuietGC(ReadinessDiagnostic* diagnostic = nullptr) noexcept
    {
        if (diagnostic) diagnostic->TransientGC = false;
        if (!Facilities(diagnostic)) return false;
        const auto* api = N::Get();
        uint8_t exitPurge{}; bool collecting{};
        const char* nativeStep = "memory observation interrupted: exit-purge flag";
        __try
        {
            if (!Read(reinterpret_cast<uintptr_t>(api->asset.exitPurge), exitPurge))
            {
                if (diagnostic) diagnostic->Observe(false, "memory read failed: exit-purge flag", "read_failed", reinterpret_cast<uintptr_t>(api->asset.exitPurge));
                return false;
            }
            if (exitPurge)
            {
                if (diagnostic) diagnostic->Check(false, "active GC flag: exit purge", 0, exitPurge);
                return false;
            }
            nativeStep = "memory observation interrupted: active-GC flag";
            if (!Read(reinterpret_cast<uintptr_t>(api->asset.activeGarbageCollection), collecting))
            {
                if (diagnostic) diagnostic->Observe(false, "memory read failed: active-GC flag", "read_failed", reinterpret_cast<uintptr_t>(api->asset.activeGarbageCollection));
                return false;
            }
            if (collecting)
            {
                if (diagnostic) diagnostic->TransientGC = true;
                if (diagnostic) diagnostic->Check(false, "active GC flag: garbage collection", 0, collecting);
                return false;
            }

            nativeStep = "native GC state observation interrupted: garbage-collecting/hash-table predicate";
            const bool locking = api->world.isGarbageCollectingAndLocking();
            if (locking)
            {
                if (diagnostic) diagnostic->TransientGC = true;
                if (diagnostic) diagnostic->Check(false, "native GC state: garbage collection/hash-table locking active", 0, locking);
                return false;
            }
            nativeStep = "native GC state observation interrupted: incremental-purge predicate";
            const bool purgePending = api->world.isIncrementalPurgePending();
            if (purgePending)
            {
                if (diagnostic) diagnostic->TransientGC = true;
                if (diagnostic) diagnostic->Check(false, "native GC state: incremental purge pending", 0, purgePending);
                return false;
            }
            nativeStep = "native GC state observation interrupted: incremental-reachability predicate";
            const bool reachabilityPending = api->world.isIncrementalReachabilityPending();
            if (reachabilityPending)
            {
                if (diagnostic) diagnostic->TransientGC = true;
                if (diagnostic) diagnostic->Check(false, "native GC state: incremental reachability pending", 0, reachabilityPending);
                return false;
            }
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            if (diagnostic) diagnostic->Observe(false, nativeStep);
            return false;
        }
    }

    // Leaf SEH boundaries: only trivial locals; never unwind native ownership here.
    bool Invoke(Operation operation, uintptr_t object = 0, uintptr_t argument = 0, uintptr_t* result = nullptr) noexcept
    {
        const auto* api = N::Get();
        if (!api) return false;
        __try
        {
            switch (operation)
            {
            case Operation::Create:
                {
                    N::WorldInitializationValues values;
                    *result = api->world.createWorld(5, false, SDK::FName{}, 0, false, 5, &values, true);
                } break;
            case Operation::Retain: api->asset.retainObject(reinterpret_cast<SDK::UObject*>(object)); break;
            case Operation::Initialize: api->world.initWorld(object, N::WorldInitializationValues{}); break;
            case Operation::Update: api->world.updateWorldComponents(object, false, false, nullptr); break;
            case Operation::Cleanup: api->world.cleanupWorld(object, true, true, 0); break;
            case Operation::Release: api->asset.releaseObject(reinterpret_cast<SDK::UObject*>(object)); break;
            case Operation::Context: *result = api->world.getWorldContextFromWorld(object, argument); break;
            case Operation::Subsystem: *result = api->world.getSubsystemInternal(object, argument); break;
            case Operation::TickType: *result = api->world.getTickableTickType(object); break;
            case Operation::Visibility: *result = api->world.isVisibilityRequestPending(object); break;
            }
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
    }

    bool FindResident(uintptr_t address, uintptr_t type, const wchar_t* path, bool exact, uintptr_t& object) noexcept
    {
        using Find = SDK::UObject*(*)(SDK::UClass*, SDK::UObject*, const wchar_t*, bool);
        __try
        {
            object = reinterpret_cast<uintptr_t>(reinterpret_cast<Find>(address)(
                reinterpret_cast<SDK::UClass*>(type), nullptr, path, exact));
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
    }

    struct NativeWorldOwner
    {
        enum ClassId { Class, World, Engine, Level, Settings, GameInstance, Electricity, Grid, Names, Mass, Replication, Wave, ClassCount };
        struct Metadata { const wchar_t* Path; size_t Size; NativeReference Ref; };
        std::array<Metadata, ClassCount> MetadataClasses{{
            {L"/Script/CoreUObject.Class", sizeof(SDK::UClass)},
            {L"/Script/Engine.World", sizeof(SDK::UWorld)},
            {L"/Script/Engine.Engine", sizeof(SDK::UEngine)},
            {L"/Script/Engine.Level", sizeof(SDK::ULevel)},
            {L"/Script/Chimera.CrWorldSettings", sizeof(SDK::ACrWorldSettings)},
            {L"/Script/Engine.GameInstance", sizeof(SDK::UGameInstance)},
            {L"/Script/Chimera.CrElectricityGraphSubsystem", sizeof(SDK::UCrElectricityGraphSubsystem)},
            {L"/Script/Chimera.CrEntityGridSubsystem", sizeof(SDK::UCrEntityGridSubsystem)},
            {L"/Script/Chimera.CrBuildingCustomNameSubsystem", sizeof(SDK::UCrBuildingCustomNameSubsystem)},
            {L"/Script/MassEntity.MassEntitySubsystem", sizeof(SDK::UMassEntitySubsystem)},
            {L"/Script/MassReplication.MassReplicationSubsystem", sizeof(SDK::UMassReplicationSubsystem)},
            {L"/Script/Chimera.CrEnviroWaveVisualsWorldSubsystem", sizeof(SDK::UCrEnviroWaveVisualsWorldSubsystem)}
        }};
        struct Baseline
        {
            ObjectIdentity Gameplay, EngineObject, GameInstanceObject;
            uintptr_t Context{};
            uint64_t ContextHash{};
            int32_t ContextCount{};
            bool operator==(const Baseline&) const = default;
        } Before;
        WorldPhase Phase = WorldPhase::Inactive;
        NativeReference Private, Package, PersistentLevel, WorldSettings;
        std::array<NativeReference, 3> Subsystems{};
        uintptr_t Scene{}; // Observed through the owned world; never manually freed.
        // Numeric separation evidence from successful active PrivateState checks.
        // These are not owners and must never be dereferenced during teardown.
        uintptr_t IsolatedGameplayPackage{}, IsolatedGameplayScene{};

        void Quarantine(const char* reason)
        {
            if (Phase != WorldPhase::Quarantined)
                LOG_ERROR("MiniMap: standalone capture world quarantined: %s; native ownership retained", reason);
            Phase = WorldPhase::Quarantined;
        }
        bool CanStartSession() const
        { return Phase == WorldPhase::Inactive || Phase == WorldPhase::Released || Phase == WorldPhase::Unavailable; }

        bool Identify(uintptr_t object, ObjectIdentity& identity, size_t extent = 0x28, ReadinessDiagnostic* diagnostic = nullptr)
        {
            const auto* api = N::Get();
            if (!api) { if (diagnostic) diagnostic->Observe(false, "identity API unavailable", "facility_unavailable"); return false; }
            const bool valid = N::ValidateObjectIdentity(object, reinterpret_cast<uintptr_t>(api->asset.objectArray), extent);
            if (!valid)
            {
                // This existing boolean helper does not expose read failure versus invalid identity.
                if (diagnostic) diagnostic->Observe(false, "native identity validation", "identity_invalid_or_unobservable", object);
                return false;
            }
            int32_t index{}; Slots::Slot slot; SlotReader reader{diagnostic};
            if (!Read(object + 0xc, index, diagnostic) ||
                !SlotObserved(Slots::ReadSlot(reader, reinterpret_cast<uintptr_t>(api->asset.objectArray), index, slot), diagnostic) ||
                (!diagnostic ? slot.Object != object : !diagnostic->Check(slot.Object == object, "identity slot object", object, slot.Object)) ||
                (!diagnostic ? bool(slot.Flags & 0x10200000) : !diagnostic->Check(!(slot.Flags & 0x10200000), "identity slot flags", 0, slot.Flags & 0x10200000))) return false;
            identity = {object, index, slot.Serial};
            return true;
        }
        bool CheckOwned(NativeReference& ref, ReadinessDiagnostic* diagnostic = nullptr)
        {
            if (ref.State != Ownership::Held)
            {
                if (diagnostic) diagnostic->Check(false, "reference ownership", uint64_t(Ownership::Held), uint64_t(ref.State));
                return false;
            }
            if (!GameThread(diagnostic)) return false;
            ObjectIdentity now;
            if (!Identify(ref.Object.Bits, now, 0x28, diagnostic)) return false;
            if (now.Index != ref.Object.Index)
            {
                if (diagnostic) diagnostic->Check(false, "reference identity index", uint64_t(ref.Object.Index), uint64_t(now.Index));
                return false;
            }
            if (now.Serial == ref.Object.Serial) return true;
            // UE may assign a serial to a continuously retained zero-serial item.
            // Never allocate one or adopt a changed nonzero serial.
            Slots::Slot slot; SlotReader reader{diagnostic};
            const auto check = [&](bool passed, const char* detail, uint64_t expected, uint64_t current)
            { return !diagnostic ? passed : diagnostic->Check(passed, detail, expected, current); };
            if (!check(ref.Object.Serial == 0, "retained reference serial changed", uint64_t(ref.Object.Serial), uint64_t(now.Serial)) ||
                !check(now.Serial > 0, "zero-serial adoption requires positive serial", 1, uint64_t(now.Serial)) ||
                !SlotObserved(Slots::ReadSlot(reader, reinterpret_cast<uintptr_t>(N::Get()->asset.objectArray), now.Index, slot), diagnostic) ||
                !check(slot.Object == now.Bits, "adoption slot object", now.Bits, slot.Object) ||
                !check(slot.Serial == now.Serial, "adoption slot serial", uint64_t(now.Serial), uint64_t(slot.Serial)) ||
                !check(bool(slot.Flags & 0x20000000), "adoption retained slot flag", 0x20000000, slot.Flags & 0x20000000)) return false;
            ref.Object = now;
            return true;
        }
        bool Hold(uintptr_t object, NativeReference& ref)
        {
            if (ref.State != Ownership::Empty) return false;
            ref.Object.Bits = object; // Keep a returned pointer even if its identity cannot be established.
            if (!QuietGC() || !Identify(object, ref.Object)) return false;
            ref.State = Ownership::Uncertain; // Before the irreversible void AddRef call.
            if (!Invoke(Operation::Retain, object)) return false;
            ref.State = Ownership::Held;
            if (CheckOwned(ref)) return true;
            ref.State = Ownership::Uncertain;
            return false;
        }
        bool Drop(NativeReference& ref)
        {
            if (ref.State == Ownership::Empty) return true;
            if (ref.State != Ownership::Held || !QuietGC() || !CheckOwned(ref)) return false;
            ref.State = Ownership::Uncertain; // Never repeat a possibly executed ReleaseRef.
            if (!Invoke(Operation::Release, ref.Object.Bits)) return false;
            ref = {};
            return true;
        }
        bool Is(uintptr_t object, ClassId expected, ReadinessDiagnostic* diagnostic = nullptr)
        {
            uintptr_t type{}; ObjectIdentity identity;
            auto& anchor = MetadataClasses[expected].Ref;
            if (!CheckOwned(anchor, diagnostic) || !Identify(object, identity, 0x28, diagnostic) || !Read(object + 0x10, type, diagnostic)) return false;
            for (unsigned depth = 0; type && depth < 64; ++depth)
            {
                if (!Identify(type, identity, sizeof(SDK::UClass), diagnostic)) return false;
                if (type == anchor.Object.Bits) return true;
                uintptr_t parent{};
                if (!Read(type + 0x40, parent, diagnostic) ||
                    (!diagnostic ? parent == type : !diagnostic->Check(parent != type, "class parent must differ", type, parent))) return false;
                type = parent;
            }
            if (diagnostic) diagnostic->Check(false, "class ancestry absent/exhausted", anchor.Object.Bits, type);
            return false;
        }
        bool NoForeignRoot(NativeReference& ref, ReadinessDiagnostic* diagnostic = nullptr)
        {
            uint32_t flags{}; Slots::Slot slot; SlotReader reader{diagnostic};
            return CheckOwned(ref, diagnostic) && Read(ref.Object.Bits + 8, flags, diagnostic) &&
                (!diagnostic ? !(flags & 2) : diagnostic->Check(!(flags & 2), "foreign object root flag", 0, flags & 2)) &&
                SlotObserved(Slots::ReadSlot(reader, reinterpret_cast<uintptr_t>(N::Get()->asset.objectArray), ref.Object.Index, slot), diagnostic) &&
                (!diagnostic ? !(slot.Flags & 0x40000000) : diagnostic->Check(!(slot.Flags & 0x40000000), "foreign slot root flag", 0, slot.Flags & 0x40000000));
        }
        bool ReleaseMetadata()
        {
            for (size_t i = MetadataClasses.size(); i > 0; --i)
                if (!Drop(MetadataClasses[i - 1].Ref)) return false;
            return true;
        }
        bool AcquireMetadata()
        {
            auto* hooks = GetHooks();
            auto* engine = hooks ? hooks->Engine : nullptr;
            const auto find = engine && engine->GetStaticFindObjectByNameAddress ? engine->GetStaticFindObjectByNameAddress() : 0;
            if (!find) return false;
            for (size_t i = 0; i < MetadataClasses.size(); ++i)
            {
                auto& metadata = MetadataClasses[i];
                const auto filter = i ? MetadataClasses[Class].Ref.Object.Bits : 0;
                uintptr_t object{}, type{}; int32_t size{}; ObjectIdentity identity;
                if (!QuietGC() || (i && !CheckOwned(MetadataClasses[Class].Ref))) return false;
                if (!FindResident(find, filter, metadata.Path, i != 0, object))
                { Quarantine("resident metadata lookup interrupted"); return false; }
                if (!object || !Identify(object, identity, sizeof(SDK::UClass)) ||
                    !Read(object + 0x10, type) || type != (i ? filter : object) ||
                    !Read(object + offsetof(SDK::UStruct, Size), size) || size != int32_t(metadata.Size)) return false;
                if (!Hold(object, metadata.Ref)) return false;
            }
            return true;
        }
        bool RendererAvailable()
        {
            const auto& renderer = N::Get()->texture;
            uint32_t gpuCount{}; uintptr_t rhi{}; bool initialized{}, threaded{}, multithreaded{};
            const auto threading = reinterpret_cast<uintptr_t>(renderer.supportsRHIMultithreading);
            return Read(reinterpret_cast<uintptr_t>(renderer.explicitGpuCount), gpuCount) && gpuCount && gpuCount <= 8 &&
                Read(reinterpret_cast<uintptr_t>(renderer.dynamicRHI), rhi) && rhi && threading >= 0x10 &&
                Read(threading - 0x10, initialized) && initialized &&
                Read(reinterpret_cast<uintptr_t>(renderer.threadedRendering), threaded) && threaded &&
                Read(threading, multithreaded) && multithreaded;
        }
        bool ObserveGameplay(Baseline& baseline, ReadinessDiagnostic* diagnostic = nullptr)
        {
            const auto game = reinterpret_cast<uintptr_t>(MiniMapMap::GetWorld());
            uintptr_t engine{}, instance{}, contexts{}, current{}; int32_t capacity{}; uint8_t flags{};
            const auto check = [&](bool passed, const char* detail, uint64_t expected = 1, uint64_t current = 0)
            { return !diagnostic ? passed : diagnostic->Check(passed, detail, expected, current); };
            const auto observe = [&](bool succeeded, const char* detail)
            { return !diagnostic ? succeeded : diagnostic->Observe(succeeded, detail); };
            if (!Read(reinterpret_cast<uintptr_t>(N::Get()->player.engineStorage), engine, diagnostic) ||
                !Is(engine, Engine, diagnostic) || !Is(game, World, diagnostic) || !Read(game + 0x18d, flags, diagnostic) || !check(!(flags & 0x20), "gameplay flags 0x18d mask", 0, flags & 0x20) ||
                !Read(game + 0x18b, flags, diagnostic) || !check(bool(flags & 0x40), "gameplay initialized flag", 0x40, flags & 0x40) ||
                !Identify(game, baseline.Gameplay, 0x28, diagnostic) || !Identify(engine, baseline.EngineObject, 0x28, diagnostic) ||
                !Read(game + 0x230, instance, diagnostic) || !Is(instance, GameInstance, diagnostic) || !Identify(instance, baseline.GameInstanceObject, 0x28, diagnostic) ||
                !observe(Invoke(Operation::Context, engine, game, &baseline.Context), "gameplay context lookup interrupted/unavailable") || !check(bool(baseline.Context), "gameplay context presence") ||
                !Read(baseline.Context + 0x2c0, current, diagnostic) || !check(current == game, "context gameplay world", game, current) ||
                !Read(engine + 0x11c8, contexts, diagnostic) || !Read(engine + 0x11d0, baseline.ContextCount, diagnostic) ||
                !Read(engine + 0x11d4, capacity, diagnostic) || !check(baseline.ContextCount >= 1, "context count >= 1", 1, uint64_t(baseline.ContextCount)) ||
                !check(capacity >= baseline.ContextCount, "context capacity >= count", uint64_t(baseline.ContextCount), uint64_t(capacity)) ||
                !check(capacity <= 4096, "context capacity <= 4096", 4096, uint64_t(capacity)) ||
                !observe(N::IsMemoryRangeReadable(contexts, size_t(baseline.ContextCount) * 8, 8), "context array unreadable")) return false;
            baseline.ContextHash = 1469598103934665603ULL;
            for (int32_t i = 0; i < baseline.ContextCount; ++i)
            {
                uintptr_t context{};
                if (!Read(contexts + size_t(i) * 8, context, diagnostic) || !observe(N::IsMemoryRangeReadable(context, 0x2c8, 8), "context unreadable") ||
                    !Read(context + 0x2c0, current, diagnostic)) return false;
                baseline.ContextHash = ((baseline.ContextHash ^ context) * 1099511628211ULL ^ current) * 1099511628211ULL;
            }
            return true;
        }
        bool GameplayUnchanged(ReadinessDiagnostic* diagnostic = nullptr)
        {
            Baseline after;
            if (diagnostic) diagnostic->Detail = "gameplay baseline observation";
            if (!ObserveGameplay(after, diagnostic))
            {
                return false;
            }
            const bool unchanged = after == Before;
            if (!unchanged && diagnostic)
            {
                // Compare already observed CPU values only, in Baseline member order.
                const auto identity = [&](const char* bits, const char* index, const char* serial,
                    const ObjectIdentity& expected, const ObjectIdentity& current)
                {
                    return diagnostic->Check(expected.Bits == current.Bits, bits, expected.Bits, current.Bits) &&
                        diagnostic->Check(expected.Index == current.Index, index, uint64_t(expected.Index), uint64_t(current.Index)) &&
                        diagnostic->Check(expected.Serial == current.Serial, serial, uint64_t(expected.Serial), uint64_t(current.Serial));
                };
                identity("gameplay pointer", "gameplay index", "gameplay serial", Before.Gameplay, after.Gameplay) &&
                    identity("engine pointer", "engine index", "engine serial", Before.EngineObject, after.EngineObject) &&
                    identity("game instance pointer", "game instance index", "game instance serial", Before.GameInstanceObject, after.GameInstanceObject) &&
                    diagnostic->Check(Before.Context == after.Context, "gameplay context", Before.Context, after.Context) &&
                    diagnostic->Check(Before.ContextHash == after.ContextHash, "context list hash", Before.ContextHash, after.ContextHash) &&
                    diagnostic->Check(Before.ContextCount == after.ContextCount, "context count", uint64_t(Before.ContextCount), uint64_t(after.ContextCount));
            }
            return unchanged;
        }
        bool Collection(uintptr_t world, bool initialized, ReadinessDiagnostic* diagnostic = nullptr)
        {
            const auto collection = world + 0x9f0;
            uintptr_t outer{}; bool populating{}, iterating{}; int32_t entries{}, free{}, arrays{}, arraysFree{};
            const auto check = [&](bool passed, const char* detail, uint64_t expected = 1, uint64_t current = 0)
            { return !diagnostic ? passed : diagnostic->Check(passed, detail, expected, current); };
            return Read(collection + 0xb0, outer, diagnostic) && Read(collection + 0xb8, populating, diagnostic) && Read(collection + 0xb9, iterating, diagnostic) &&
                Read(collection + 0x10, entries, diagnostic) && Read(collection + 0x3c, free, diagnostic) &&
                Read(collection + 0x60, arrays, diagnostic) && Read(collection + 0x8c, arraysFree, diagnostic) &&
                check(entries >= 0, "collection entries nonnegative", 0, uint64_t(entries)) &&
                check(free >= 0, "collection free nonnegative", 0, uint64_t(free)) &&
                check(free <= entries, "collection free <= entries", uint64_t(entries), uint64_t(free)) &&
                check(arrays >= 0, "collection arrays nonnegative", 0, uint64_t(arrays)) &&
                check(arraysFree >= 0, "collection arrays free nonnegative", 0, uint64_t(arraysFree)) &&
                check(arraysFree <= arrays, "collection arrays free <= arrays", uint64_t(arrays), uint64_t(arraysFree)) &&
                (initialized || (check(entries == free, "uninitialized collection entries/free", uint64_t(entries), uint64_t(free)) &&
                    check(arrays == arraysFree, "uninitialized collection arrays/free", uint64_t(arrays), uint64_t(arraysFree)))) &&
                check(outer == (initialized ? world : 0), "collection outer", initialized ? world : 0, outer) &&
                check(!populating, "collection populating", 0, populating) && check(!iterating, "collection iterating", 0, iterating);
        }
        bool PrivateState(bool initialized, ReadinessDiagnostic* diagnostic = nullptr)
        {
            const auto world = Private.Object.Bits;
            uintptr_t package{}, gameplayPackage{}, level{}, owner{}, settings{}, type{}, scene{}, gameplayScene{}, instance{}, net{}, context{};
            uint8_t worldType{}, a{}, b{}, c{}, ever{};
            const auto check = [&](bool passed, const char* detail, uint64_t expected = 1, uint64_t current = 0)
            { return !diagnostic ? passed : diagnostic->Check(passed, detail, expected, current); };
            const auto observe = [&](bool succeeded, const char* detail)
            { return !diagnostic ? succeeded : diagnostic->Observe(succeeded, detail); };
            if (!CheckOwned(Private, diagnostic) || !check(world != Before.Gameplay.Bits, "private/gameplay worlds must differ", Before.Gameplay.Bits, world) || !Is(world, World, diagnostic) ||
                !NoForeignRoot(Private, diagnostic) || !CheckOwned(Package, diagnostic) || !NoForeignRoot(Package, diagnostic) ||
                !Read(world + 0x20, package, diagnostic) || !check(package == Package.Object.Bits, "private package", Package.Object.Bits, package) ||
                !Read(Before.Gameplay.Bits + 0x20, gameplayPackage, diagnostic) || !check(package != gameplayPackage, "private/gameplay packages must differ", gameplayPackage, package) ||
                !Read(world + 0x18a, worldType, diagnostic) || !check(worldType == 5, "private world type", 5, worldType) || !Read(world + 0x18b, a, diagnostic) ||
                !Read(world + 0x18c, b, diagnostic) || !Read(world + 0x18d, c, diagnostic) || !Read(world + 0x18f, ever, diagnostic) ||
                !check(bool(a & 0x40) == initialized, "private initialized flag", initialized, bool(a & 0x40)) ||
                !check(!(b & 0x80), "private flags 0x18c mask", 0, b & 0x80) ||
                !check(!(c & 0x21), "private flags 0x18d mask", 0, c & 0x21) ||
                !check(bool(ever & 2) == initialized, "private ever-initialized flag", initialized, bool(ever & 2)) ||
                !Read(world + 0x258, scene, diagnostic) || !check(bool(scene) == initialized, "private scene presence", initialized, bool(scene)) ||
                !Read(world + 0x230, instance, diagnostic) || !check(!instance, "private game instance", 0, instance) ||
                !Read(world + 0x38, net, diagnostic) || !check(!net, "private net driver", 0, net) ||
                !observe(Invoke(Operation::Context, Before.EngineObject.Bits, world, &context), "private context lookup interrupted/unavailable") || !check(!context, "private world context", 0, context) ||
                !CheckOwned(PersistentLevel, diagnostic) || !CheckOwned(WorldSettings, diagnostic) ||
                !Read(world + 0x30, level, diagnostic) || !check(level == PersistentLevel.Object.Bits, "private persistent level", PersistentLevel.Object.Bits, level) ||
                !Read(level + 0xc0, owner, diagnostic) || !check(owner == world, "level owning world", world, owner) ||
                !Read(level + 0x2c8, settings, diagnostic) || !check(settings == WorldSettings.Object.Bits, "level world settings", WorldSettings.Object.Bits, settings) ||
                !Read(settings + 0x10, type, diagnostic) || !check(type == MetadataClasses[Settings].Ref.Object.Bits, "world settings class", MetadataClasses[Settings].Ref.Object.Bits, type) || !Collection(world, initialized, diagnostic)) return false;
            if (initialized)
            {
                if (!check(!(Scene && Scene != scene), "private scene identity", Scene, scene) || !observe(N::IsMemoryRangeReadable(scene, 8, 8), "private scene unreadable") ||
                    !Read(Before.Gameplay.Bits + 0x258, gameplayScene, diagnostic) || !check(bool(gameplayScene), "gameplay scene presence") ||
                    !check(scene != gameplayScene, "private/gameplay scenes must differ", gameplayScene, scene)) return false;
                Scene = scene;
                IsolatedGameplayPackage = gameplayPackage;
                IsolatedGameplayScene = gameplayScene;
            }
            return true;
        }
        bool TeardownPrivateState(bool initialized, ReadinessDiagnostic* diagnostic)
        {
            const auto world = Private.Object.Bits;
            const auto currentGameplay = reinterpret_cast<uintptr_t>(MiniMapMap::GetWorld());
            const auto check = [&](bool passed, const char* detail, uint64_t expected = 1, uint64_t current = 0)
            { return !diagnostic ? passed : diagnostic->Check(passed, detail, expected, current); };
            const auto observe = [&](bool succeeded, const char* detail)
            { return !diagnostic ? succeeded : diagnostic->Observe(succeeded, detail); };
            // The gameplay world/context may already be ending or gone. Validate
            // the current engine against its baseline before using its native accessor;
            // never dereference Before.Gameplay, its package, scene, or context.
            uintptr_t engine{}; ObjectIdentity engineIdentity;
            if (!Read(reinterpret_cast<uintptr_t>(N::Get()->player.engineStorage), engine, diagnostic) ||
                !Identify(engine, engineIdentity, sizeof(SDK::UEngine), diagnostic) ||
                !check(engineIdentity.Bits == Before.EngineObject.Bits, "teardown engine pointer", Before.EngineObject.Bits, engineIdentity.Bits) ||
                !check(engineIdentity.Index == Before.EngineObject.Index, "teardown engine index", uint64_t(Before.EngineObject.Index), uint64_t(engineIdentity.Index)) ||
                !check(engineIdentity.Serial == Before.EngineObject.Serial, "teardown engine serial", uint64_t(Before.EngineObject.Serial), uint64_t(engineIdentity.Serial)) ||
                !Is(engine, Engine, diagnostic)) return false;
            uintptr_t package{}, level{}, owner{}, settings{}, type{}, scene{}, instance{}, net{}, context{};
            uint8_t worldType{}, flags{};
            if (!CheckOwned(Private, diagnostic) || !Is(world, World, diagnostic) ||
                !check(world != Before.Gameplay.Bits, "private/baseline gameplay worlds must differ", Before.Gameplay.Bits, world) ||
                !check(world != currentGameplay, "private/current gameplay worlds must differ", currentGameplay, world) ||
                !NoForeignRoot(Private, diagnostic) || !CheckOwned(Package, diagnostic) || !NoForeignRoot(Package, diagnostic) ||
                !check(bool(IsolatedGameplayPackage) && bool(IsolatedGameplayScene) && bool(Scene), "verified active isolation evidence present") ||
                !Read(world + 0x20, package, diagnostic) || !check(package == Package.Object.Bits, "private package", Package.Object.Bits, package) ||
                !check(package != IsolatedGameplayPackage, "private/baseline gameplay packages must differ", IsolatedGameplayPackage, package) ||
                !Read(world + 0x18a, worldType, diagnostic) || !check(worldType == 5, "private world type", 5, worldType) ||
                !Read(world + 0x18b, flags, diagnostic) || !check(bool(flags & 0x40) == initialized, "private initialized flag", initialized, bool(flags & 0x40)) ||
                !Read(world + 0x258, scene, diagnostic) || !check(scene == Scene, "private scene identity", Scene, scene) ||
                !check(scene != IsolatedGameplayScene, "private/baseline gameplay scenes must differ", IsolatedGameplayScene, scene) ||
                !Read(world + 0x230, instance, diagnostic) || !check(!instance, "private game instance", 0, instance) ||
                !Read(world + 0x38, net, diagnostic) || !check(!net, "private net driver", 0, net) ||
                !observe(Invoke(Operation::Context, engine, world, &context), "private context lookup interrupted/unavailable") || !check(!context, "private world context", 0, context) ||
                !CheckOwned(PersistentLevel, diagnostic) || !CheckOwned(WorldSettings, diagnostic) || !CheckOwned(MetadataClasses[Settings].Ref, diagnostic) ||
                !Read(world + 0x30, level, diagnostic) || !check(level == PersistentLevel.Object.Bits, "private persistent level", PersistentLevel.Object.Bits, level) ||
                !Read(level + 0xc0, owner, diagnostic) || !check(owner == world, "level owning world", world, owner) ||
                !Read(level + 0x2c8, settings, diagnostic) || !check(settings == WorldSettings.Object.Bits, "level world settings", WorldSettings.Object.Bits, settings) ||
                !Read(settings + 0x10, type, diagnostic) || !check(type == MetadataClasses[Settings].Ref.Object.Bits, "world settings class", MetadataClasses[Settings].Ref.Object.Bits, type) ||
                !Collection(world, initialized, diagnostic)) return false;
            if (initialized)
            {
                if (!Read(world + 0x18c, flags, diagnostic) || !check(!(flags & 0x80), "private flags 0x18c mask", 0, flags & 0x80) ||
                    !Read(world + 0x18d, flags, diagnostic) || !check(!(flags & 0x21), "private flags 0x18d mask", 0, flags & 0x21) ||
                    !Read(world + 0x18f, flags, diagnostic) || !check(bool(flags & 2), "private ever-initialized flag", 2, flags & 2) ||
                    !observe(N::IsMemoryRangeReadable(scene, 8, 8), "private scene unreadable")) return false;
            }
            constexpr std::array<ClassId, 3> required{Electricity, Grid, Names};
            for (size_t i = 0; i < Subsystems.size(); ++i)
            {
                auto& ref = Subsystems[i]; uintptr_t outer{}, collection{};
                if (!CheckOwned(ref, diagnostic) || !Is(ref.Object.Bits, required[i], diagnostic) ||
                    !Read(ref.Object.Bits + 0x20, outer, diagnostic) || !check(outer == world, "subsystem owning world", world, outer) ||
                    !Read(ref.Object.Bits + 0x28, collection, diagnostic) ||
                    !check(collection == (initialized ? world + 0x9f0 : 0), "subsystem collection", initialized ? world + 0x9f0 : 0, collection)) return false;
            }
            return true;
        }
        bool FindSubsystem(ClassId type, uintptr_t& object)
        {
            const auto world = Private.Object.Bits;
            auto& metadata = MetadataClasses[type].Ref;
            if (!QuietGC() || !CheckOwned(metadata) || !Is(world, World) || !Collection(world, true) ||
                !Invoke(Operation::Subsystem, world + 0x9f0, metadata.Object.Bits, &object) ||
                !QuietGC() || !CheckOwned(metadata) || !Is(world, World) || !Collection(world, true)) return false;
            uintptr_t outer{}, collection{};
            return !object || (Is(object, type) && Read(object + 0x20, outer) && outer == world &&
                Read(object + 0x28, collection) && collection == world + 0x9f0);
        }
        bool RetainSubsystems()
        {
            constexpr std::array<ClassId, 3> required{Electricity, Grid, Names};
            for (size_t i = 0; i < required.size(); ++i)
            {
                uintptr_t object{};
                if (!FindSubsystem(required[i], object) || !object || !Hold(object, Subsystems[i])) return false;
            }
            uintptr_t manager{}, controller{}; bool ticking{};
            if (!Read(Subsystems[0].Object.Bits + 0x290, manager) || manager ||
                !Read(Subsystems[0].Object.Bits + 0x298, controller) || controller ||
                !Read(Subsystems[1].Object.Bits + 0xf0, manager) || manager ||
                !Read(Subsystems[1].Object.Bits + 0xf8, controller) || controller ||
                !Read(Subsystems[0].Object.Bits + 0x38, ticking) || !ticking) return false;
            for (const auto excluded : {Mass, Replication, Wave})
            {
                uintptr_t object{};
                if (!FindSubsystem(excluded, object) || object) return false;
            }
            return true;
        }
        bool Preflight()
        {
            uintptr_t configured{};
            return Facilities() && MiniMapMap::GameplayReady() && QuietGC() && RendererAvailable() &&
                AcquireMetadata() && ObserveGameplay(Before) &&
                Read(Before.EngineObject.Bits + 0x190, configured) && configured == MetadataClasses[Settings].Ref.Object.Bits && QuietGC();
        }

        // Called only by the serialized game-thread production service.
        void Create()
        {
            if (!CanStartSession()) return;
            Before = {};
            Scene = 0;
            IsolatedGameplayPackage = IsolatedGameplayScene = 0;
            Phase = WorldPhase::Preparing;
            if (!Preflight())
            {
                if (Phase == WorldPhase::Quarantined) return;
                if (!ReleaseMetadata()) { Quarantine("preflight reference release failed"); return; }
                Phase = WorldPhase::Unavailable;
                LOG_WARN("MiniMap: standalone capture world prerequisites unavailable");
                return;
            }
            // Preserve even a partially returned shell pointer if the native call interrupts.
            if (!Invoke(Operation::Create, 0, 0, &Private.Object.Bits) || !Private.Object.Bits)
            { Quarantine("world creation returned null or was interrupted"); return; }
            if (!Is(Private.Object.Bits, World) || !Hold(Private.Object.Bits, Private))
            { Quarantine("created world identity or retention uncertain"); return; }
            uintptr_t object{};
            if (!Read(Private.Object.Bits + 0x20, object) || !Hold(object, Package) ||
                !Read(Private.Object.Bits + 0x30, object) || !Is(object, Level) || !Hold(object, PersistentLevel) ||
                !Read(PersistentLevel.Object.Bits + 0x2c8, object) || !Is(object, Settings) || !Hold(object, WorldSettings) ||
                !QuietGC() || !GameplayUnchanged() || !PrivateState(false))
            { Quarantine("created world dependencies or isolation invalid"); return; }
            if (!Invoke(Operation::Initialize, Private.Object.Bits) || !QuietGC() || !GameplayUnchanged() || !PrivateState(true))
            { Quarantine("world initialization interrupted or postconditions invalid"); return; }
            if (!Invoke(Operation::Update, Private.Object.Bits) || !QuietGC() || !GameplayUnchanged() || !PrivateState(true) ||
                !RetainSubsystems() || !GameplayUnchanged() || !QuietGC())
            { Quarantine("component update or subsystem isolation invalid"); return; }
            Phase = WorldPhase::Ready;
        }

        bool CleanupPreconditions(ReadinessDiagnostic* diagnostic = nullptr)
        {
            const auto world = Private.Object.Bits;
            int32_t count{}; uintptr_t visibility{};
            const auto stage = [&](const char* name) { if (diagnostic) diagnostic->Stage(name); return true; };
            const auto counter = [&](uintptr_t offset, const char* name)
            {
                if (!Read(world + offset, count, diagnostic)) return false;
                return !diagnostic ? !count : diagnostic->Check(!count, name, 0, uint64_t(count));
            };
            return stage("GC admission") && QuietGC(diagnostic) &&
                stage("teardown private ownership/isolation") && TeardownPrivateState(true, diagnostic) &&
                stage("streaming counter +0x98") && counter(0x98, "world counter +0x98") &&
                stage("streaming counter +0xA8") && counter(0xa8, "world counter +0xA8") &&
                stage("streaming counter +0xB8") && counter(0xb8, "world counter +0xB8") &&
                stage("streaming counter +0xC0") && counter(0xc0, "world counter +0xC0") &&
                stage("pending visibility") &&
                (!diagnostic ? Invoke(Operation::Visibility, world, 0, &visibility) :
                    diagnostic->Observe(Invoke(Operation::Visibility, world, 0, &visibility), "visibility lookup interrupted/unavailable")) &&
                (!diagnostic ? !visibility : diagnostic->Check(!visibility, "visibility pending", 0, visibility));
        }
        bool CleanupPostconditions(ReadinessDiagnostic* diagnostic = nullptr)
        {
            bool ticking{}; uintptr_t tickType{};
            const auto stage = [&](const char* name) { if (diagnostic) diagnostic->Stage(name); return true; };
            return stage("post-cleanup GC admission") && QuietGC(diagnostic) &&
                stage("post-cleanup private ownership/isolation") && TeardownPrivateState(false, diagnostic) &&
                stage("post-cleanup electricity initialized flag") &&
                Read(Subsystems[0].Object.Bits + 0x38, ticking, diagnostic) &&
                (!diagnostic ? !ticking : diagnostic->Check(!ticking, "electricity initialized flag", 0, ticking)) &&
                stage("post-cleanup electricity tick type") &&
                (!diagnostic ? Invoke(Operation::TickType, Subsystems[0].Object.Bits + 0x30, 0, &tickType) :
                    diagnostic->Observe(Invoke(Operation::TickType, Subsystems[0].Object.Bits + 0x30, 0, &tickType), "tick type lookup interrupted/unavailable")) &&
                (!diagnostic ? tickType == 2 : diagnostic->Check(tickType == 2, "electricity tick type Never", 2, tickType)) &&
                stage("post-cleanup final GC admission") && QuietGC(diagnostic);
        }
        bool Retire(bool requestActive)
        {
            if (CanStartSession()) return true; // No native ownership remains in these states.
            if (Phase == WorldPhase::Quarantined) return false;
            // Cleanup has not begun: an observed GC pause leaves every owner intact.
            ReadinessDiagnostic diagnostic;
            if (!requestActive && Phase == WorldPhase::Ready && !QuietGC(&diagnostic))
            {
                if (diagnostic.TransientGC) return false;
                LOG_ERROR("MiniMap: world cleanup GC admission failed: detail=%s outcome=%s valuesAvailable=%u expected=0x%llX current=0x%llX address=0x%llX",
                    diagnostic.Detail, diagnostic.Outcome, unsigned(diagnostic.ValuesAvailable),
                    static_cast<unsigned long long>(diagnostic.Expected), static_cast<unsigned long long>(diagnostic.Current),
                    static_cast<unsigned long long>(diagnostic.Address));
                Quarantine("world cleanup GC observation or prerequisites invalid"); return false;
            }
            // Request resources must be retired before world-only teardown.
            diagnostic.Stage("active-resource exclusion");
            if (!diagnostic.Check(!requestActive, "request active", 0, requestActive) ||
                (diagnostic.Stage("native world phase"), !diagnostic.Check(Phase == WorldPhase::Ready, "native world phase", uint64_t(WorldPhase::Ready), uint64_t(Phase))) ||
                (diagnostic.Stage("facilities"), !Facilities(&diagnostic)) || !CleanupPreconditions(&diagnostic))
            {
                if (diagnostic.TransientGC) return false; // No cleanup operation has begun.
                LOG_ERROR("MiniMap: world cleanup admission rejected: worldPhase=%u requestActive=%u predicate=%s detail=%s outcome=%s valuesAvailable=%u expected=0x%llX current=0x%llX address=0x%llX",
                    unsigned(Phase), unsigned(requestActive), diagnostic.Predicate, diagnostic.Detail, diagnostic.Outcome,
                    unsigned(diagnostic.ValuesAvailable), static_cast<unsigned long long>(diagnostic.Expected),
                    static_cast<unsigned long long>(diagnostic.Current), static_cast<unsigned long long>(diagnostic.Address));
                Quarantine("world cleanup admission or active-resource exclusion failed"); return false;
            }
            Phase = WorldPhase::Retiring;
            diagnostic.Stage("native cleanup invocation");
            if (!diagnostic.Observe(Invoke(Operation::Cleanup, Private.Object.Bits), "cleanup invocation interrupted/unavailable") || !CleanupPostconditions(&diagnostic))
            {
                LOG_ERROR("MiniMap: world cleanup execution/postconditions rejected: worldPhase=%u requestActive=%u predicate=%s detail=%s outcome=%s valuesAvailable=%u expected=0x%llX current=0x%llX address=0x%llX",
                    unsigned(Phase), unsigned(requestActive), diagnostic.Predicate, diagnostic.Detail, diagnostic.Outcome,
                    unsigned(diagnostic.ValuesAvailable), static_cast<unsigned long long>(diagnostic.Expected),
                    static_cast<unsigned long long>(diagnostic.Current), static_cast<unsigned long long>(diagnostic.Address));
                Quarantine("world cleanup interrupted or subsystem detachment invalid"); return false;
            }
            for (auto& ref : Subsystems)
                if (!Drop(ref)) { Quarantine("subsystem release uncertain"); return false; }
            if (!Drop(WorldSettings) || !Drop(PersistentLevel) || !Drop(Package) || !Drop(Private) || !ReleaseMetadata())
            { Quarantine("world dependency release uncertain"); return false; }
            // Cleanup and released references do not prove synchronous UObject
            // destruction or independent renderer-scene retirement. Never free Scene.
            Phase = WorldPhase::Released;
            return true;
        }
    };

    // Only plain ownership records: no native releases during static destruction.
    static_assert(std::is_trivially_destructible_v<NativeWorldOwner>);
    NativeWorldOwner g_nativeWorld;

    enum class RequestPhase { Waiting, Preparing, Prepared, C0, Readback, Retiring, Failed, Retired, Quarantined };
    enum class ResourceOperation { New, Flags, Register, Unregister, DestroyActor, DestroyComponent, InitializeTarget, Resource, ConstructFence, BeginFence, PollFence, DestroyFence, CaptureScene };
    uint64_t g_captureServiceTick = 0; // Private service iterations, never reset at session boundaries.

    bool ReadbackAvailable(bool initialize, bool& available) noexcept
    {
#if defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
        __try
        {
            available = initialize ? MiniMapReadbackNative::Preflight() : MiniMapReadbackNative::CaptureRetirementRendererAvailable();
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
#else
        (void)initialize;
        available = false; // The independent adapter is compiled out.
        return true;
#endif
    }

    // Leaf boundaries mirror the supported native calls. An interrupted
    // irreversible operation is never retried.
    bool ResourceCall(ResourceOperation operation, uintptr_t object, uintptr_t argument = 0, uintptr_t* result = nullptr) noexcept
    {
        const auto* api = N::Get();
        if (!api) return false;
        __try
        {
            switch (operation)
            {
            case ResourceOperation::New:
                {
                    SDK::TSubclassOf<SDK::UObject> type(reinterpret_cast<SDK::UClass*>(argument));
                    *result = reinterpret_cast<uintptr_t>(api->asset.spawnObject(&type, reinterpret_cast<SDK::UObject*>(object)));
                } break;
            case ResourceOperation::Flags: api->asset.setObjectFlags(reinterpret_cast<SDK::UObject*>(object), 0x40); break;
            case ResourceOperation::Register: api->player.registerComponent(reinterpret_cast<SDK::UActorComponent*>(object), reinterpret_cast<SDK::UWorld*>(argument), nullptr); break;
            case ResourceOperation::Unregister: api->player.unregisterComponent(reinterpret_cast<SDK::UActorComponent*>(object)); break;
            case ResourceOperation::DestroyActor: *result = api->world.destroyActor(object, false, false); break;
            case ResourceOperation::DestroyComponent: api->player.destroyComponent(reinterpret_cast<SDK::UActorComponent*>(object), false); break;
            case ResourceOperation::InitializeTarget: api->texture.initRenderTarget(reinterpret_cast<SDK::UTextureRenderTarget2D*>(object), 256, 256, 10, true); break;
            case ResourceOperation::Resource: *result = reinterpret_cast<uintptr_t>(api->texture.getRenderTargetResource(reinterpret_cast<SDK::UTextureRenderTarget2D*>(object))); break;
            case ResourceOperation::ConstructFence: api->texture.renderFenceConstruct(reinterpret_cast<void*>(object)); break;
            case ResourceOperation::BeginFence: api->texture.renderFenceBegin(reinterpret_cast<void*>(object), 1); break;
            case ResourceOperation::PollFence: *result = api->texture.renderFencePoll(reinterpret_cast<const void*>(object)); break;
            case ResourceOperation::DestroyFence: api->texture.renderFenceDestroy(reinterpret_cast<void*>(object)); break;
            case ResourceOperation::CaptureScene: reinterpret_cast<SDK::USceneCaptureComponent2D*>(object)->CaptureScene(); break;
            }
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
    }

#if defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
    bool ServiceReadback(MiniMapReadbackNative::Job* job, uint64_t tick)
    {
        // The adapter catches enqueue C++ exceptions and preserves its evidence.
        // Catch native access faults here; other C++ exceptions reach Service's
        // conservative quarantine boundary without unwinding any native owner.
        __try { MiniMapReadbackNative::Service(job, tick); return true; }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
    }
#endif

    bool SpawnActor(uintptr_t world, uintptr_t type, const SDK::FTransform* transform, uintptr_t* result) noexcept
    {
        __try
        {
            *result = reinterpret_cast<uintptr_t>(SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
                reinterpret_cast<SDK::UObject*>(world), SDK::TSubclassOf<SDK::AActor>(reinterpret_cast<SDK::UClass*>(type)),
                *transform, SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn, nullptr, SDK::ESpawnActorScaleMethod::MultiplyWithRoot));
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
    }
    bool FinishActor(uintptr_t actor, const SDK::FTransform* transform, uintptr_t* result) noexcept
    {
        __try
        {
            *result = reinterpret_cast<uintptr_t>(SDK::UGameplayStatics::FinishSpawningActor(
                reinterpret_cast<SDK::AActor*>(actor), *transform, SDK::ESpawnActorScaleMethod::MultiplyWithRoot));
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
    }
    bool ActorSubclass(uintptr_t type, uintptr_t actor, bool* result) noexcept
    {
        __try
        {
            *result = SDK::UKismetMathLibrary::ClassIsChildOf(SDK::TSubclassOf<SDK::UObject>(reinterpret_cast<SDK::UClass*>(type)),
                SDK::TSubclassOf<SDK::UObject>(reinterpret_cast<SDK::UClass*>(actor)));
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
    }
    bool ActorBounds(uintptr_t actor, SDK::FVector* center, SDK::FVector* extent) noexcept
    {
        __try { SDK::UKismetSystemLibrary::GetActorBounds(reinterpret_cast<SDK::AActor*>(actor), center, extent); return true; }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
    }
    bool ConfigureResources(SDK::USceneCaptureComponent2D* capture, SDK::UTextureRenderTarget2D* target,
        const SDK::FVector* extent) noexcept
    {
        // SDK strings/arrays here borrow stack data; they do not own allocations.
        static_assert(std::is_trivially_destructible_v<SDK::FEngineShowFlagsSetting>);
        __try
        {
            MiniMapSpriteCapture::ConfigureSceneCapture(capture, *extent);
            SDK::FEngineShowFlagsSetting flags[11]{};
            const wchar_t* names[] = {L"Fog", L"Atmosphere", L"VolumetricFog", L"Cloud", L"MotionBlur", L"TemporalAA",
                L"AntiAliasing", L"PostProcessing", L"Bloom", L"EyeAdaptation", L"Lighting"};
            for (int i = 0; i < 11; ++i) { flags[i].ShowFlagName = SDK::FString(names[i]); flags[i].Enabled = false; }
            SDK::TArray<SDK::FEngineShowFlagsSetting> settings(flags, 11, 11);
            capture->SetShowFlagSettings(settings);
            target->bAutoGenerateMips = false;
            target->bSupportsUAV = false;
            uint32_t targetFlags{};
            if (!Read(reinterpret_cast<uintptr_t>(target) + 0x144, targetFlags)) return false;
            targetFlags &= ~uint32_t(1);
            std::memcpy(reinterpret_cast<uint8_t*>(target) + 0x144, &targetFlags, sizeof(targetFlags));
            target->ClearColor = SDK::FLinearColor(0, 0, 0, 1);
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
    }
    bool PositionCapture(SDK::USceneCaptureComponent2D* capture, SDK::UTextureRenderTarget2D* target,
        const SDK::FVector* center, const SDK::FVector* extent) noexcept
    {
        __try
        {
            capture->TextureTarget = target;
            const SDK::FVector location(center->X, center->Y, center->Z + extent->Z + 1000.0);
            const SDK::FRotator rotation(-90, -90, 0);
            capture->K2_SetWorldLocationAndRotation(location, rotation, false, nullptr, true);
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
    }

    struct ActiveRequest
    {
        ActiveRequest() = default;
        ActiveRequest(const ActiveRequest&) = delete;
        ActiveRequest& operator=(const ActiveRequest&) = delete;
        std::string BlueprintPath;
        RequestPhase Phase = RequestPhase::Waiting;
        // Each UObject has exactly one owning record. Class aliases are indexes.
        enum MetadataId { Generated, ActorType, Blueprint, DefaultObject, CaptureType, TargetType, MetadataCount };
        std::array<NativeReference, MetadataCount> Metadata{};
        NativeReference Actor, Capture, Target;
        SDK::FVector Center{}, Extent{};
        uint64_t PreparedTick{}, C0Tick{};
        bool CaptureSubmitted = false, Committed = false; // Native lifetime history, including while Retiring.
        std::vector<uint8_t> Rgba; // Full canvas; retained through native retirement and feedback delivery.
        std::optional<bool> FinalResult; // Set once, including write failure; never retry persistence.
        bool FeedbackSubmitted = false;
        enum class WaitReason { AdmissionGC, PreparationAdmission, ReadbackPrerequisite,
            GeneratedMetadata, ActorMetadata, CaptureMetadata, TargetMetadata, FindPrerequisite,
            GeneratedClass, Fence, ReadbackCompletion, RetirementReadback, Feedback,
            ApiSpawn, ApiFlags, ApiRegister, ApiUnregister, ApiDestroyComponent, ApiDestroyActor,
            ApiInitializeTarget, ApiTargetResource, ApiFenceConstruct, ApiFenceBegin, ApiFencePoll, ApiFenceDestroy };
        static_assert(unsigned(WaitReason::ApiFenceDestroy) < 32);
        uint32_t ReportedWaits = 0; // At most one message per reason for this request.
        bool FailureReported = false; // No diagnostic pointers survive their observation.
        void Wait(WaitReason reason, const char* detail) noexcept
        {
            const auto bit = uint32_t(1) << unsigned(reason);
            if (ReportedWaits & bit) return;
            ReportedWaits |= bit;
            Diagnostic("MiniMap: sprite request waiting: canonical=%s requestPhase=%u reason=%s; active FIFO slot retained, no skip/requeue",
                BlueprintPath.c_str(), unsigned(Phase), detail);
        }
        template<class... Args> static void Diagnostic(const char* format, Args... args) noexcept
        {
            try { if (auto* self = GetSelf(); self && self->logger && self->logger->Warn) self->logger->Warn(self, format, args...); }
            catch (...) {} // Reporting cannot affect native request progression.
        }
        void Rejected(const char* reason) noexcept
        {
            if (FailureReported) return;
            FailureReported = true;
            Diagnostic("MiniMap: sprite request rejected: canonical=%s requestPhase=%u reason=%s; terminal publication failure, native retirement still required",
                BlueprintPath.c_str(), unsigned(Phase), reason);
        }
#if defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
        MiniMapReadbackNative::Job* Job{};
#endif
        MiniMapCaptureRetirement::FenceOwner Fence; // Sole fence; no automatic native destruction.

        struct FenceReleaseInterrupted {};
        struct FenceOperations
        {
            ActiveRequest& Owner;
            bool Available()
            {
                if (Owner.FenceAvailable()) return true;
                Owner.Quarantine("capture retirement renderer or fence prerequisites unavailable"); return false;
            }
            bool Construct(void* storage) { return Owner.Call(ResourceOperation::ConstructFence, reinterpret_cast<uintptr_t>(storage)); }
            bool Begin(void* storage) { return Owner.Call(ResourceOperation::BeginFence, reinterpret_cast<uintptr_t>(storage)); }
            bool Poll(void* storage)
            {
                uintptr_t complete{};
                return Owner.Call(ResourceOperation::PollFence, reinterpret_cast<uintptr_t>(storage), 0, &complete) && complete;
            }
            void Destroy(void* storage)
            {
                // FenceOwner's destructor operation has a void contract. Unwind
                // Release before it can replace Uncertain with Destroyed on fault.
                if (!Owner.Call(ResourceOperation::DestroyFence, reinterpret_cast<uintptr_t>(storage)))
                    throw FenceReleaseInterrupted{};
            }
        };

        void Quarantine(const char* reason)
        {
            if (Phase != RequestPhase::Quarantined)
                Diagnostic("MiniMap: sprite request quarantine: canonical=%s requestPhase=%u reason=%s; native ownership retained",
                    BlueprintPath.c_str(), unsigned(Phase), reason);
            Phase = RequestPhase::Quarantined; g_nativeWorld.Quarantine(reason);
        }
        bool Ready(ReadinessDiagnostic* diagnostic = nullptr)
        {
            if (g_nativeWorld.Phase != WorldPhase::Ready)
            {
                if (diagnostic) diagnostic->Check(false, "native world phase", uint64_t(WorldPhase::Ready), uint64_t(g_nativeWorld.Phase));
                return false;
            }
            if (diagnostic) diagnostic->Predicate = "GC admission";
            if (!QuietGC(diagnostic)) return false;
            if (diagnostic) diagnostic->Predicate = "gameplay baseline observation/comparison";
            if (!g_nativeWorld.GameplayUnchanged(diagnostic)) return false;
            if (diagnostic)
            {
                diagnostic->Predicate = "private-world ownership/isolation validation";
                diagnostic->Detail = "private-world validation/observation failed";
            }
            return g_nativeWorld.PrivateState(true, diagnostic);
        }
        // Only resumable entry boundaries defer. Inner native-operation guards
        // still fail closed: a partially executed operation must never be replayed.
        bool Admit(const char* reason, bool* gcDeferred = nullptr)
        {
            if (gcDeferred) *gcDeferred = false;
            ReadinessDiagnostic diagnostic;
            if (Ready(&diagnostic)) return true;
            if (diagnostic.TransientGC)
            {
                Wait(WaitReason::AdmissionGC, diagnostic.Detail);
                if (gcDeferred) *gcDeferred = true;
                return false;
            }
            if (g_nativeWorld.Phase != WorldPhase::Quarantined)
                LOG_ERROR("MiniMap: capture readiness failed: requestPhase=%u worldPhase=%u predicate=%s detail=%s outcome=%s valuesAvailable=%u expected=0x%llX current=0x%llX address=0x%llX reason=%s",
                    unsigned(Phase), unsigned(g_nativeWorld.Phase), diagnostic.Predicate, diagnostic.Detail, diagnostic.Outcome,
                    unsigned(diagnostic.ValuesAvailable),
                    static_cast<unsigned long long>(diagnostic.Expected), static_cast<unsigned long long>(diagnostic.Current),
                    static_cast<unsigned long long>(diagnostic.Address), reason);
            Quarantine(reason); return false;
        }
        bool FenceAvailable()
        {
            bool available{};
            if (!Ready() || !ReadbackAvailable(false, available) || !available) return false;
            const auto& api = N::Get()->texture;
            return api.renderFenceConstruct && api.renderFenceBegin && api.renderFencePoll && api.renderFenceDestroy;
        }
        bool Call(ResourceOperation operation, uintptr_t object, uintptr_t argument = 0, uintptr_t* result = nullptr)
        {
            const bool admitted = Ready();
            if (admitted && ResourceCall(operation, object, argument, result)) return true;
            Diagnostic("MiniMap: sprite native operation failed: canonical=%s requestPhase=%u operation=%u C0Submitted=%u C10Committed=%u reason=%s",
                BlueprintPath.c_str(), unsigned(Phase), unsigned(operation), unsigned(CaptureSubmitted), unsigned(Committed),
                admitted ? "native operation returned failure/interrupted" : "Ready admission failed; operation not invoked");
            Quarantine("request native operation interrupted or world ownership changed"); return false;
        }
        bool ReleaseFence()
        {
            FenceOperations operations{*this};
            try { if (Fence.Release(operations)) return true; }
            catch (const FenceReleaseInterrupted&) {} // Storage stays Uncertain.
            Quarantine("capture retirement fence release uncertain"); return false;
        }
        bool DropMetadata()
        {
            for (size_t i = Metadata.size(); i > 0; --i)
                if (!g_nativeWorld.Drop(Metadata[i - 1])) return false;
            return true;
        }
        uintptr_t Lookup(const wchar_t* path, uintptr_t filter, bool exact)
        {
            auto* hooks = GetHooks();
            auto* engine = hooks ? hooks->Engine : nullptr;
            const auto find = engine && engine->GetStaticFindObjectByNameAddress ? engine->GetStaticFindObjectByNameAddress() : 0;
            uintptr_t object{};
            if (!find) { Wait(WaitReason::FindPrerequisite, "resident object lookup address unavailable"); return 0; }
            if (!Ready() || !FindResident(find, filter, path, exact, object))
                Quarantine("request resident lookup interrupted or world changed");
            return object;
        }
        bool Keep(uintptr_t object, NativeReference& ref)
        {
            if (Ready() && g_nativeWorld.Hold(object, ref)) return true;
            Quarantine("request native reference acquisition uncertain"); return false;
        }
        bool ClassMetadata(const wchar_t* path, MetadataId id, size_t expectedSize)
        {
            const auto object = Lookup(path, g_nativeWorld.MetadataClasses[NativeWorldOwner::Class].Ref.Object.Bits, true);
            if (Phase == RequestPhase::Quarantined || !object)
            {
                if (Phase != RequestPhase::Quarantined)
                {
                    const auto reason = id == Generated ? WaitReason::GeneratedMetadata : id == ActorType ? WaitReason::ActorMetadata :
                        id == CaptureType ? WaitReason::CaptureMetadata : WaitReason::TargetMetadata;
                    Wait(reason, id == Generated ? "BlueprintGeneratedClass metadata lookup returned no object" : id == ActorType ?
                        "Actor metadata lookup returned no object" : id == CaptureType ? "SceneCaptureComponent2D metadata lookup returned no object" :
                        "TextureRenderTarget2D metadata lookup returned no object");
                }
                return false;
            }
            ObjectIdentity identity; uintptr_t type{}; int32_t size{};
            if (!g_nativeWorld.Identify(object, identity, sizeof(SDK::UClass)) || !Read(object + 0x10, type) ||
                type != g_nativeWorld.MetadataClasses[NativeWorldOwner::Class].Ref.Object.Bits ||
                !Read(object + offsetof(SDK::UStruct, Size), size) || size != int32_t(expectedSize))
            { Quarantine("request class metadata layout or identity invalid"); return false; }
            return Keep(object, Metadata[id]);
        }
        bool Level(uintptr_t target, bool expected, uint64_t* hash = nullptr, int32_t* count = nullptr)
        {
            struct PointerArray { uintptr_t Data{}; int32_t Count{}, Capacity{}; } actors;
            if (!g_nativeWorld.CheckOwned(g_nativeWorld.PersistentLevel) ||
                !Read(g_nativeWorld.PersistentLevel.Object.Bits + 0xa0, actors) || actors.Count < 0 ||
                actors.Capacity < actors.Count || actors.Capacity > 4096 ||
                (actors.Count && !N::IsMemoryRangeReadable(actors.Data, size_t(actors.Count) * 8, 8))) return false;
            bool found = false; uint64_t value{}; int32_t total{};
            for (int32_t i = 0; i < actors.Count; ++i)
            {
                uintptr_t actor{}; ObjectIdentity identity;
                if (!Read(actors.Data + size_t(i) * 8, actor)) return false;
                if (!actor) continue;
                if (!g_nativeWorld.Identify(actor, identity, sizeof(SDK::AActor))) return false;
                found |= actor == target; value ^= actor * 1099511628211ULL; ++total;
            }
            if (hash) *hash = value;
            if (count) *count = total;
            return found == expected;
        }
        bool ActorState(bool finished)
        {
            const auto actor = Actor.Object.Bits;
            uint8_t a{}, b{}; uintptr_t type{}, outer{}, owner{}, instigator{};
            return g_nativeWorld.CheckOwned(Actor) && g_nativeWorld.CheckOwned(Metadata[Blueprint]) &&
                Read(actor + 0x10, type) && type == Metadata[Blueprint].Object.Bits &&
                Read(actor + 0x20, outer) && outer == g_nativeWorld.PersistentLevel.Object.Bits &&
                Read(actor + offsetof(SDK::AActor, Owner), owner) && !owner &&
                Read(actor + offsetof(SDK::AActor, Instigator), instigator) && !instigator &&
                Read(actor + 0x5c, a) && bool(a & 1) == finished && !(a & 2) &&
                Read(actor + 0x5d, b) && !(b & 0x1a) && Ready() && Level(actor, true);
        }
        bool NewResource(MetadataId id, NativeReference& ref, size_t size)
        {
            if (!g_nativeWorld.CheckOwned(Metadata[id])) { Quarantine("resource class ownership invalid"); return false; }
            if (!Call(ResourceOperation::New, g_nativeWorld.Private.Object.Bits, Metadata[id].Object.Bits, &ref.Object.Bits)) return false;
            ObjectIdentity identity; uintptr_t type{}, outer{};
            if (!ref.Object.Bits || !Keep(ref.Object.Bits, ref) || !g_nativeWorld.Identify(ref.Object.Bits, identity, size) ||
                !Read(ref.Object.Bits + 0x10, type) || type != Metadata[id].Object.Bits ||
                !Read(ref.Object.Bits + 0x20, outer) || outer != g_nativeWorld.Private.Object.Bits)
            { Quarantine("native resource factory returned uncertain ownership"); return false; }
            return Call(ResourceOperation::Flags, ref.Object.Bits);
        }
        // Used only immediately after this owner successfully destroys an object.
        // Live-object Identify deliberately rejects the pending-kill state here.
        bool ReleaseDestroyed(NativeReference& ref)
        {
            Slots::Slot slot; SlotReader reader; int32_t index{}, count{}; uint32_t flags{};
            const auto bits = ref.Object.Bits;
            if (ref.State != Ownership::Held || !QuietGC() ||
                Slots::ReadSlot(reader, reinterpret_cast<uintptr_t>(N::Get()->asset.objectArray), ref.Object.Index, slot) != Slots::Status::Success ||
                slot.Object != bits || (slot.Flags & 0x10000000) || !(slot.Flags & 0x20000000) || !(slot.Flags & 0x00200000) ||
                !N::IsMemoryRangeReadable(bits, 0x28, 8) || !Read(bits + 0xc, index) || index != ref.Object.Index ||
                !Read(bits + 8, flags) || (flags & 0x1b400) || !Read(slot.Item + 0x14, count) || count < 1) return false;
            if (slot.Serial != ref.Object.Serial)
            {
                if (ref.Object.Serial != 0 || slot.Serial <= 0) return false;
                ref.Object.Serial = slot.Serial;
            }
            ref.State = Ownership::Uncertain;
            if (!Invoke(Operation::Release, bits)) return false;
            ref = {}; return true;
        }
        bool HasNativeOwnership() const
        {
            const auto held = [](const NativeReference& ref) { return ref.State != Ownership::Empty || ref.Object.Bits != 0; };
            const auto fence = Fence.GetState();
            bool owned = (fence != MiniMapCaptureRetirement::FenceState::Empty && fence != MiniMapCaptureRetirement::FenceState::Destroyed) ||
                held(Actor) || held(Capture) || held(Target);
            for (const auto& ref : Metadata) owned |= held(ref);
#if defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
            owned |= Job != nullptr;
#endif
            return owned;
        }
        // CPU-only completion. Retired/Failed alone never proves native retirement.
        bool Complete(MiniMapSpriteQueues::SpriteFeedbackQueue& feedback)
        {
            if ((Phase != RequestPhase::Retired && Phase != RequestPhase::Failed) || HasNativeOwnership() ||
                g_nativeWorld.Phase == WorldPhase::Quarantined) return false;
            if (FeedbackSubmitted) return true;
            if (!FinalResult.has_value())
            {
                FinalResult = false; // Commit the one-time attempt before allocating or writing.
                try
                {
                    std::array<wchar_t, 1025> wide{};
                    MiniMapBuildingCache::SpatialBounds bounds{
                        {Center.X - Extent.X, Center.Y - Extent.Y, Center.Z - Extent.Z},
                        {Center.X + Extent.X, Center.Y + Extent.Y, Center.Z + Extent.Z}};
                    bool validBounds = true;
                    for (size_t axis = 0; axis < 3; ++axis)
                        validBounds &= std::isfinite(bounds.Min[axis]) && std::isfinite(bounds.Max[axis]) &&
                            bounds.Min[axis] <= bounds.Max[axis];
                    if (Phase == RequestPhase::Retired && CaptureSubmitted && Committed &&
                        Path(BlueprintPath, wide) && Rgba.size() == 256 * 256 * 4 && validBounds)
                        FinalResult = MiniMapBuildingCache::Write(BlueprintPath, Rgba.data(), 256, 256, bounds);
                    else if (!FailureReported)
                        Rejected(Rgba.size() != 256 * 256 * 4 ? "capture produced no acceptable 256x256 RGBA result" :
                            !validBounds ? "cache publication bounds nonfinite/reversed" : "cache publication phase/submission/path prerequisites not satisfied");
                }
                catch (...) { Rejected("CPU/cache exception during final publication"); } // No remaining native ownership.
            }
            try
            {
                feedback.Add({BlueprintPath, *FinalResult});
                FeedbackSubmitted = true;
            }
            catch (...) { Wait(WaitReason::Feedback, "feedback delivery allocation/queue exception; result retained without another write"); return false; }
            return true;
        }

        bool DetachCapture()
        {
            if (Capture.State != Ownership::Empty)
            {
                uint8_t flags{};
                if (!g_nativeWorld.CheckOwned(Capture) || !Read(Capture.Object.Bits + 0x94, flags) ||
                    ((flags & 1) && !Call(ResourceOperation::Unregister, Capture.Object.Bits)) ||
                    !Read(Capture.Object.Bits + 0x94, flags) || (flags & 3))
                { Quarantine("capture component detachment uncertain"); return false; }
            }
            return true;
        }
        bool RetireObjects()
        {
            const auto fence = Fence.GetState();
            if (!Ready() || (fence != MiniMapCaptureRetirement::FenceState::Empty && fence != MiniMapCaptureRetirement::FenceState::Destroyed))
            { Quarantine("object retirement attempted before fence release or with invalid world"); return false; }
#if defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
            if (Job) { Quarantine("object retirement attempted with a live readback Job"); return false; }
#endif
            if (Actor.State != Ownership::Empty)
            {
                uint8_t a{}, b{}; uintptr_t destroyed{};
                if (!Read(Actor.Object.Bits + 0x5c, a) || !ActorState(bool(a & 1)) ||
                    !Call(ResourceOperation::DestroyActor, Actor.Object.Bits, 0, &destroyed) || !destroyed ||
                    !Read(Actor.Object.Bits + 0x5c, a) || (a & 2) || !Read(Actor.Object.Bits + 0x5d, b) ||
                    !(b & 2) || (b & 0x18) || !Level(Actor.Object.Bits, false) || !ReleaseDestroyed(Actor))
                { Quarantine("Actor destruction or reference release uncertain"); return false; }
            }
            if (Capture.State != Ownership::Empty)
            {
                uint8_t init{}, flags{};
                if (!g_nativeWorld.CheckOwned(Capture) || !Read(Capture.Object.Bits + 0x97, init) || (init & 0x28) ||
                    !Call(ResourceOperation::DestroyComponent, Capture.Object.Bits) ||
                    !Read(Capture.Object.Bits + 0x94, flags) || (flags & 3) || !ReleaseDestroyed(Capture))
                { Quarantine("component destruction or reference release uncertain"); return false; }
            }
            if (!g_nativeWorld.Drop(Target) || !DropMetadata() || !Ready())
            { Quarantine("capture dependency release uncertain"); return false; }
            if (Phase != RequestPhase::Failed)
                Phase = CaptureSubmitted && Rgba.empty() ? RequestPhase::Failed : RequestPhase::Retired;
            return true;
        }

        bool FinishRetirement()
        {
            using MiniMapCaptureRetirement::FenceState;
            if (Phase != RequestPhase::Retiring || !Ready() ||
                (Fence.GetState() != FenceState::Inserted && Fence.GetState() != FenceState::Complete))
            { Quarantine("post-capture retirement state invalid"); return false; }
            FenceOperations operations{*this};
            if (!Fence.Poll(operations))
            {
                if (Phase != RequestPhase::Quarantined) Wait(WaitReason::Fence, "retirement fence not complete");
                return false; // Nonblocking; faults quarantine through operations.
            }
#if defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
            if (Job)
            {
                if (!Committed)
                {
                    // C0 still requires the scene fence, although its Job never ran.
                    if (!MiniMapReadbackNative::DiscardExternalBeforeCapture(Job))
                    { Quarantine("C0-only readback Job disposal failed"); return false; }
                }
                else
                {
                    if (MiniMapReadbackNative::ExternalRejectedWithoutReadback(Job) &&
                        !MiniMapReadbackNative::CompleteExternalCaptureRetirement(Job))
                    { Quarantine("rejected readback Job retirement failed"); return false; }
                    const auto report = MiniMapReadbackNative::Inspect(Job);
                    if (report.Quarantined || report.Enqueue == MiniMapCaptureRetirement::EnqueueState::Uncertain)
                    { Quarantine("readback ownership uncertain during retirement"); return false; }
                    if (report.Current != MiniMapReadbackNative::Phase::CleanupComplete || report.Callables || report.Outstanding)
                    { Wait(WaitReason::RetirementReadback, "readback cleanup/callables/native work still outstanding during retirement"); return false; }
                    if (report.Result == MiniMapReadbackNative::Outcome::Pending)
                    { Quarantine("readback retirement lacks a terminal outcome"); return false; }
                    // The adapter additionally verifies that no readback or RHI
                    // texture is owned; a scene fence alone is insufficient.
                    if (!MiniMapReadbackNative::DestroyIfRetired(Job))
                    { Quarantine("readback native owners not retired"); return false; }
                }
            }
#endif
            return ReleaseFence() && RetireObjects();
        }
        bool Retire(bool* gcDeferred = nullptr)
        {
            if (gcDeferred) *gcDeferred = false;
            if (Phase == RequestPhase::Quarantined) return false;
            if (!HasNativeOwnership())
            {
                if (Phase != RequestPhase::Failed)
                    Phase = CaptureSubmitted && Rgba.empty() ? RequestPhase::Failed : RequestPhase::Retired;
                return true;
            }
            if (!Admit("capture retirement world admission failed", gcDeferred)) return false;
            if (Phase == RequestPhase::Retiring) return FinishRetirement();
            if (!DetachCapture()) return false;
            if (CaptureSubmitted)
            {
                // CaptureScene may enqueue scene work independently of the Job.
                // Detach first, then order an RHI-depth fence behind that work.
                FenceOperations operations{*this};
                if (!Fence.Insert(operations, true))
                { Quarantine("capture retirement fence insertion uncertain"); return false; }
                Phase = RequestPhase::Retiring;
                return FinishRetirement();
            }
            // Never-captured preparation retains the established disposal path.
#if defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
            if (!MiniMapReadbackNative::DiscardExternalBeforeCapture(Job))
            { Quarantine("unsubmitted readback Job could not be discarded"); return false; }
#endif
            return ReleaseFence() && RetireObjects();
        }
        void Fail(const char* reason)
        { Rejected(reason); Phase = RequestPhase::Failed; Retire(); }

        bool CaptureState()
        {
            if (!Ready() || !ActorState(true) || !g_nativeWorld.CheckOwned(Capture) || !g_nativeWorld.CheckOwned(Target) ||
                Fence.GetState() != MiniMapCaptureRetirement::FenceState::Constructed || !FenceAvailable()) return false;
            for (auto& ref : Metadata) if (!g_nativeWorld.CheckOwned(ref)) return false;
            uint8_t registered{}; uintptr_t world{}, target{};
            if (!Read(Capture.Object.Bits + 0x94, registered) || !(registered & 1) ||
                !Read(Capture.Object.Bits + 0xa8, world) || world != g_nativeWorld.Private.Object.Bits ||
                !Read(Capture.Object.Bits + offsetof(SDK::USceneCaptureComponent2D, TextureTarget), target) || target != Target.Object.Bits) return false;
#if defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
            if (!Job) return false;
            const auto report = MiniMapReadbackNative::Inspect(Job);
            return report.Current == MiniMapReadbackNative::Phase::ReadyToSubmit && !report.Quarantined &&
                !report.Callables && !report.Outstanding && !report.NativeEnqueueEverInvoked &&
                report.Result == MiniMapReadbackNative::Outcome::Pending;
#else
            return false;
#endif
        }
#if defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
        void ConvertReadback(const MiniMapReadbackNative::Report& report)
        {
            using ReadbackPhase = MiniMapReadbackNative::Phase;
            constexpr auto required = (1u << uint32_t(ReadbackPhase::ReadbackReady)) | (1u << uint32_t(ReadbackPhase::PixelsCopied)) | (1u << uint32_t(ReadbackPhase::Verified));
            const auto* pixels = MiniMapReadbackNative::FloatPixels(Job);
            const auto* stats = MiniMapReadbackNative::FloatStats(Job);
            MiniMapCapturePng::Image image;
            // FloatPixels is packed by the adapter, independent of staging pitch.
            // Same predicates and short-circuit order; no extra pixel pass or native observation.
            const char* rejected = nullptr;
            if (!report.HasPixels) rejected = "readback report has no pixels";
            else if (!report.NativeEnqueueEverInvoked) rejected = "readback native enqueue was never invoked";
            else if (report.Enqueue != MiniMapCaptureRetirement::EnqueueState::ReadbackCompleted) rejected = "readback enqueue state is not completed";
            else if ((report.Visited & required) != required) rejected = "required readback verification phases not visited";
            else if (!pixels || !stats) rejected = "readback pixel buffer/statistics unavailable";
            else if (pixels->size() != 256 * 256 * 8) rejected = "unexpected packed 256x256 float RGBA byte length";
            else if (!MiniMapCapturePng::Convert(pixels->data(), pixels->size(), 256, 256, 256, 256, image)) rejected = "RGBA pixel conversion failed";
            else if (image.Rgba.size() != 256 * 256 * 4) rejected = "unexpected converted 256x256 RGBA byte length";
            else if (image.Sanitized) rejected = "converted pixels required sanitization";
            else if (stats->Nonfinite) rejected = "readback contains nonfinite pixel data";
            else if (!stats->Foreground) rejected = "readback has no foreground pixels";
            else if (!stats->Background) rejected = "readback has no background pixels";
            if (rejected) { Rejected(rejected); return; }
            Rgba = std::move(image.Rgba); // Completion may persist only after native retirement.
        }
#endif
        // One iteration advances only this request; preparation always returns
        // before C0, and C10 retains the ten-service-tick minimum interval.
        void Service()
        {
            if (Phase == RequestPhase::Quarantined || Phase == RequestPhase::Failed || Phase == RequestPhase::Retired) return;
            if (!GameThread() || g_captureServiceTick == (std::numeric_limits<uint64_t>::max)())
            { Quarantine("capture service thread or tick counter invalid"); return; }
            try
            {
                if (!Admit("capture service native world changed")) return;
                // A GC-blocked iteration neither advances this clock nor replays
                // preparation, C0, C10 commitment, readback service, or retirement.
                ++g_captureServiceTick;
                if (Phase == RequestPhase::Waiting) { Prepare(); return; }
                if (Phase == RequestPhase::Retiring) { Retire(); return; }
#if defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
                if (Phase == RequestPhase::Prepared || Phase == RequestPhase::C0)
                {
                    if (g_captureServiceTick <= PreparedTick || (Phase == RequestPhase::C0 && g_captureServiceTick - C0Tick < 10)) return;
                    if (!CaptureState()) { Quarantine("capture submission native ownership or prerequisites invalid"); return; }
                    if (Phase == RequestPhase::Prepared)
                    {
                        CaptureSubmitted = true; // BEFORE CaptureScene can enqueue anything.
                        if (!Call(ResourceOperation::CaptureScene, Capture.Object.Bits)) return;
                        C0Tick = g_captureServiceTick;
                        Phase = RequestPhase::C0;
                        return;
                    }
                    if (!MiniMapReadbackNative::MarkExternalCaptureCommitted(Job))
                    { Quarantine("C10 readback commitment uncertain"); return; }
                    Committed = true;
                    if (!Call(ResourceOperation::CaptureScene, Capture.Object.Bits)) return;
                    Phase = RequestPhase::Readback;
                    if (!ServiceReadback(Job, g_captureServiceTick)) Quarantine("C10 readback submission interrupted");
                    return;
                }
                if (Phase == RequestPhase::Readback)
                {
                    if (!Committed || !Job) { Quarantine("readback request commitment invalid"); return; }
                    if (!ServiceReadback(Job, g_captureServiceTick)) { Quarantine("readback service interrupted"); return; }
                    const auto report = MiniMapReadbackNative::Inspect(Job);
                    if (report.Quarantined || report.Enqueue == MiniMapCaptureRetirement::EnqueueState::Uncertain)
                    {
                        if (MiniMapReadbackNative::ExternalRejectedWithoutReadback(Job))
                        { Rejected(report.Failure ? report.Failure : "external readback rejected without submission"); Retire(); }
                        else Quarantine("readback submission or native ownership uncertain");
                        return;
                    }
                    if (report.Current != MiniMapReadbackNative::Phase::CleanupComplete || report.Callables || report.Outstanding)
                    { Wait(WaitReason::ReadbackCompletion, "readback cleanup/callables/native work still outstanding"); return; }
                    if (report.Result == MiniMapReadbackNative::Outcome::Pending)
                    { Quarantine("readback cleanup completed without a terminal outcome"); return; }
                    if (report.Result == MiniMapReadbackNative::Outcome::Verified)
                    {
                        // Allocation/conversion failure is a CPU result failure,
                        // not uncertain GPU ownership: still retire native work.
                        try { ConvertReadback(report); }
                        catch (...) { Rejected("CPU exception converting verified readback pixels"); Rgba.clear(); }
                    }
                    else Rejected(report.Failure ? report.Failure : "readback terminal outcome was not Verified");
                    Retire(); // Rgba stays owned until every native dependency retires.
                    return;
                }
#endif
                Quarantine("capture service request phase invalid");
            }
            catch (...) { Quarantine("capture service exception with retained native ownership"); }
        }

        static bool Path(const std::string& path, std::array<wchar_t, 1025>& wide)
        {
            if (path.size() > 1024 || !path.starts_with("/Game/") || !path.ends_with("_C") || path.find('\0') != std::string::npos) return false;
            const auto dot = path.find('.'), slash = path.rfind('/');
            if (dot == std::string::npos || dot <= slash + 1 || path.find('.', dot + 1) != std::string::npos ||
                path.substr(slash + 1, dot - slash - 1) != path.substr(dot + 1, path.size() - dot - 3)) return false;
            for (size_t i = 1; i < path.size(); ++i)
            {
                const auto c = static_cast<unsigned char>(path[i]);
                if (c < 128 && !(c >= 'A' && c <= 'Z') && !(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '_' && c != '/' && c != '.') return false;
                if (c == '/' && (i > dot || path[i - 1] == '/')) return false;
            }
            return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(), -1, wide.data(), int(wide.size())) > 1;
        }
        // Exactly one privately owned FIFO path, prepared on the game thread.
        void Prepare()
        {
            if (Phase != RequestPhase::Waiting) return;
            std::array<wchar_t, 1025> wide{};
            if (!Path(BlueprintPath, wide)) { Fail("canonical Blueprint path rejected by capture validator"); return; }
            ReadinessDiagnostic preparationDiagnostic;
            if (!Ready(&preparationDiagnostic)) { Wait(WaitReason::PreparationAdmission, preparationDiagnostic.Detail); return; }
            const auto& api = *N::Get();
            // Preserve the original prerequisite order and short-circuiting.
            if (!api.asset.spawnObject) { Wait(WaitReason::ApiSpawn, "spawnObject API unavailable"); return; }
            if (!api.asset.setObjectFlags) { Wait(WaitReason::ApiFlags, "setObjectFlags API unavailable"); return; }
            if (!api.player.registerComponent) { Wait(WaitReason::ApiRegister, "registerComponent API unavailable"); return; }
            if (!api.player.unregisterComponent) { Wait(WaitReason::ApiUnregister, "unregisterComponent API unavailable"); return; }
            if (!api.player.destroyComponent) { Wait(WaitReason::ApiDestroyComponent, "destroyComponent API unavailable"); return; }
            if (!api.world.destroyActor) { Wait(WaitReason::ApiDestroyActor, "destroyActor API unavailable"); return; }
            if (!api.texture.initRenderTarget) { Wait(WaitReason::ApiInitializeTarget, "initRenderTarget API unavailable"); return; }
            if (!api.texture.getRenderTargetResource) { Wait(WaitReason::ApiTargetResource, "getRenderTargetResource API unavailable"); return; }
            if (!api.texture.renderFenceConstruct) { Wait(WaitReason::ApiFenceConstruct, "renderFenceConstruct API unavailable"); return; }
            if (!api.texture.renderFenceBegin) { Wait(WaitReason::ApiFenceBegin, "renderFenceBegin API unavailable"); return; }
            if (!api.texture.renderFencePoll) { Wait(WaitReason::ApiFencePoll, "renderFencePoll API unavailable"); return; }
            if (!api.texture.renderFenceDestroy) { Wait(WaitReason::ApiFenceDestroy, "renderFenceDestroy API unavailable"); return; }
            bool available{};
            if (!ReadbackAvailable(true, available)) { Quarantine("readback prerequisite inspection interrupted"); return; }
            if (!available) { Wait(WaitReason::ReadbackPrerequisite, "readback prerequisite inspection succeeded but service unavailable"); return; }
            if (!ClassMetadata(L"/Script/Engine.BlueprintGeneratedClass", Generated, sizeof(SDK::UBlueprintGeneratedClass)) ||
                !ClassMetadata(L"/Script/Engine.Actor", ActorType, sizeof(SDK::AActor)) ||
                !ClassMetadata(L"/Script/Engine.SceneCaptureComponent2D", CaptureType, sizeof(SDK::USceneCaptureComponent2D)) ||
                !ClassMetadata(L"/Script/Engine.TextureRenderTarget2D", TargetType, sizeof(SDK::UTextureRenderTarget2D)))
            {
                if (Phase != RequestPhase::Quarantined && !DropMetadata()) Quarantine("waiting metadata release uncertain");
                return;
            }
            const auto object = Lookup(wide.data(), Metadata[Generated].Object.Bits, true);
            if (Phase == RequestPhase::Quarantined) return;
            if (!object) // Valid generated-class path, not resident: retain the request, not pointers.
            {
                Wait(WaitReason::GeneratedClass, "generated-class lookup returned no object; resident-only lookup does not load the asset");
                if (!DropMetadata()) Quarantine("waiting class metadata release uncertain"); return;
            }
            ObjectIdentity identity; uintptr_t type{}, cdo{};
            if (!g_nativeWorld.Identify(object, identity, sizeof(SDK::UBlueprintGeneratedClass)) || !Read(object + 0x10, type))
            { Quarantine("resident generated-class identity invalid"); return; }
            if (type != Metadata[Generated].Object.Bits) { Fail("resident object type is not the expected BlueprintGeneratedClass"); return; }
            if (!Keep(object, Metadata[Blueprint])) return;
            bool isActor{};
            if (!ActorSubclass(object, Metadata[ActorType].Object.Bits, &isActor)) { Quarantine("Actor inheritance call interrupted"); return; }
            if (!isActor) { Fail("Blueprint-generated class is not an Actor subclass"); return; }
            if (!Read(object + offsetof(SDK::UClass, ClassDefaultObject), cdo) || !cdo ||
                !g_nativeWorld.Identify(cdo, identity) || !Read(cdo + 0x10, type) || type != object)
            { Fail("Blueprint CDO read/identity/type validation failed"); return; }
            if (!Keep(cdo, Metadata[DefaultObject])) return;
            Phase = RequestPhase::Preparing;
            if (ReportedWaits & (uint32_t(1) << unsigned(WaitReason::GeneratedClass)))
                Diagnostic("MiniMap: sprite residency wait cleared: canonical=%s; generated class and CDO validated, preparation resumed", BlueprintPath.c_str());
            FenceOperations operations{*this};
            if (!Fence.Prepare(operations)) { Quarantine("capture retirement fence preparation uncertain"); return; }
            uint64_t before{}, after{}; int32_t countBefore{}, countAfter{};
            if (!Level(0, false, &before, &countBefore) || !Ready()) { Quarantine("private level invalid before spawn"); return; }
            SDK::FTransform transform{};
            transform.Rotation.W = 1; transform.Scale3D = SDK::FVector(1, 1, 1);
            if (!SpawnActor(g_nativeWorld.Private.Object.Bits, object, &transform, &Actor.Object.Bits))
            { Quarantine("deferred Actor spawn interrupted"); return; }
            if (!Actor.Object.Bits)
            {
                if (!Ready() || !Level(0, false, &after, &countAfter) || before != after || countBefore != countAfter)
                { Quarantine("null spawn changed native world ownership"); return; }
                Fail("deferred Actor spawn returned null with unchanged level ownership"); return;
            }
            if (!Keep(Actor.Object.Bits, Actor) || !ActorState(false)) { Quarantine("deferred Actor ownership invalid"); return; }
            uintptr_t finished{};
            if (!FinishActor(Actor.Object.Bits, &transform, &finished) || finished != Actor.Object.Bits || !ActorState(true))
            { Quarantine("Blueprint construction interrupted or ownership invalid"); return; }
            if (!ActorBounds(Actor.Object.Bits, &Center, &Extent)) { Quarantine("Actor bounds call interrupted"); return; }
            const float width = float(2.0 * (std::max)(Extent.X, Extent.Y) * 1.1);
            if (!std::isfinite(Center.X) || !std::isfinite(Center.Y) || !std::isfinite(Center.Z) ||
                !std::isfinite(Extent.X) || !std::isfinite(Extent.Y) || !std::isfinite(Extent.Z) ||
                !(Extent.X > 0 && Extent.Y > 0 && Extent.Z >= 0) || !std::isfinite(Center.Z + Extent.Z + 1000.0) ||
                !std::isfinite(width) || !(width > 0))
            { Center = SDK::FVector(0, 0, 0); Extent = SDK::FVector(500, 500, 500); }
            if (!Ready() || !ActorState(true)) { Quarantine("world changed after construction"); return; }
            if (!NewResource(CaptureType, Capture, sizeof(SDK::USceneCaptureComponent2D)) ||
                !NewResource(TargetType, Target, sizeof(SDK::UTextureRenderTarget2D))) return;
            if (!ConfigureResources(reinterpret_cast<SDK::USceneCaptureComponent2D*>(Capture.Object.Bits),
                reinterpret_cast<SDK::UTextureRenderTarget2D*>(Target.Object.Bits), &Extent))
            { Quarantine("capture configuration interrupted"); return; }
            if (!Call(ResourceOperation::InitializeTarget, Target.Object.Bits)) return;
            if (!Ready() || !PositionCapture(reinterpret_cast<SDK::USceneCaptureComponent2D*>(Capture.Object.Bits),
                reinterpret_cast<SDK::UTextureRenderTarget2D*>(Target.Object.Bits), &Center, &Extent))
            { Quarantine("capture camera preparation interrupted"); return; }
            if (!Call(ResourceOperation::Register, Capture.Object.Bits, g_nativeWorld.Private.Object.Bits)) return;
            uint8_t registered{}; uintptr_t world{}, resource{};
            if (!Read(Capture.Object.Bits + 0x94, registered) || !(registered & 1) ||
                !Read(Capture.Object.Bits + 0xa8, world) || world != g_nativeWorld.Private.Object.Bits)
            { Quarantine("capture registration postconditions invalid"); return; }
            if (!Call(ResourceOperation::Resource, Target.Object.Bits, 0, &resource)) return;
            if (!resource) { Fail("render-target resource getter returned null"); return; }
#if defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
            Job = MiniMapReadbackNative::CreateExternalJob(reinterpret_cast<void*>(resource), 256, 256);
            if (!Job) { Fail("external readback Job creation returned null"); return; }
#endif
            if (!Ready() || !ActorState(true)) { Quarantine("prepared request ownership invalid"); return; }
            PreparedTick = g_captureServiceTick;
            Phase = RequestPhase::Prepared;
        }
    };

    MiniMapSpriteQueues::SpriteFifo* g_fifo = nullptr;
    MiniMapSpriteQueues::SpriteFeedbackQueue* g_feedback = nullptr; // Borrowed scheduler queue, not another queue.
    bool g_sessionActive = false;
    std::optional<ActiveRequest> g_activeRequest;
    constexpr unsigned kWorldCreationAttempts = 3;
    constexpr unsigned kWorldRetryTicks = 120;
    unsigned g_worldCreationAttempts = 0, g_worldRetryTicks = 0;

    // Caller holds LifecycleMutex; only this game-thread path can write cache
    // files or submit feedback. Session end closes publication under that lock.
    void ServiceActiveRequest()
    {
        if (!g_sessionActive || !g_feedback || !g_activeRequest || !GameThread() ||
            g_nativeWorld.Phase == WorldPhase::Quarantined) return;
        auto& request = *g_activeRequest;
        if (request.Phase == RequestPhase::Failed && request.HasNativeOwnership())
            request.Retire();
        else
            request.Service();
        if (g_sessionActive && request.Complete(*g_feedback))
            g_activeRequest.reset(); // World remains owned and ready for the next request.
    }
}

namespace MiniMapBuildingSpriteRenderer
{
    void Initialize(MiniMapSpriteQueues::SpriteFifo& fifo, MiniMapSpriteQueues::SpriteFeedbackQueue& feedback)
    {
        if (g_sessionActive || g_activeRequest) return; // Do not redirect a retained request's feedback.
        g_fifo = &fifo;
        g_feedback = &feedback;
        g_sessionActive = false;
    }

    bool InitializeSession()
    {
        if (!g_fifo || !g_feedback || g_sessionActive || g_activeRequest || !g_nativeWorld.CanStartSession()) return false;
        g_worldCreationAttempts = g_worldRetryTicks = 0;
        g_sessionActive = true;
        return true;
    }

    void Tick(float deltaTime)
    {
        (void)deltaTime;
        if (!g_fifo || !g_feedback || !g_sessionActive || !MiniMapMap::GameplayReady() ||
            g_nativeWorld.Phase == WorldPhase::Quarantined) return;
        if (!g_activeRequest && g_nativeWorld.Phase != WorldPhase::Ready && !g_fifo->HasPending()) return;
        if (!GameThread())
        {
            if (g_activeRequest || !g_nativeWorld.CanStartSession())
            {
                if (g_activeRequest) g_activeRequest->Quarantine("capture service lost its game-thread prerequisite");
                else g_nativeWorld.Quarantine("capture service lost its game-thread prerequisite");
            }
            else if (g_worldCreationAttempts < kWorldCreationAttempts)
            {
                g_worldCreationAttempts = kWorldCreationAttempts;
                g_nativeWorld.Phase = WorldPhase::Unavailable;
                LOG_WARN("MiniMap: building capture game-thread binding unavailable; FIFO preserved and capture paused for this session");
            }
            return;
        }
        // Retirement and undelivered feedback take priority. Even if this clears
        // the request, admission waits until the next service iteration.
        if (g_activeRequest) { ServiceActiveRequest(); return; }
        if (g_nativeWorld.Phase != WorldPhase::Ready)
        {
            if (!g_nativeWorld.CanStartSession())
            { g_nativeWorld.Quarantine("capture service encountered an incomplete world transition"); return; }
            if (g_worldCreationAttempts >= kWorldCreationAttempts) return;
            if (g_worldRetryTicks) { --g_worldRetryTicks; return; }
            ReadinessDiagnostic diagnostic;
            const bool quiet = QuietGC(&diagnostic);
            if (!quiet && diagnostic.TransientGC) return; // GC does not consume a creation attempt.
            ++g_worldCreationAttempts;
            if (quiet)
            {
                try { g_nativeWorld.Create(); }
                catch (...) { g_nativeWorld.Quarantine("private-world creation exception"); return; }
            }
            else g_nativeWorld.Phase = WorldPhase::Unavailable; // No ownership was acquired.
            if (g_nativeWorld.Phase != WorldPhase::Ready)
            {
                g_worldRetryTicks = kWorldRetryTicks;
                if (g_nativeWorld.Phase != WorldPhase::Quarantined && g_worldCreationAttempts == kWorldCreationAttempts)
                    LOG_WARN("MiniMap: building capture preflight unavailable after three attempts; FIFO preserved until next session");
                return;
            }
        }
        // An idle, retained world can wait for GC to become quiet. Never dequeue
        // until its current gameplay identity and private ownership are verified.
        ReadinessDiagnostic diagnostic;
        if (!QuietGC(&diagnostic))
        {
            if (diagnostic.TransientGC) return;
            LOG_ERROR("MiniMap: idle capture GC admission failed: detail=%s outcome=%s valuesAvailable=%u expected=0x%llX current=0x%llX address=0x%llX",
                diagnostic.Detail, diagnostic.Outcome, unsigned(diagnostic.ValuesAvailable),
                static_cast<unsigned long long>(diagnostic.Expected), static_cast<unsigned long long>(diagnostic.Current),
                static_cast<unsigned long long>(diagnostic.Address));
            g_nativeWorld.Quarantine("idle capture GC observation or prerequisites invalid"); return;
        }
        if (!g_nativeWorld.GameplayUnchanged() || !g_nativeWorld.PrivateState(true))
        { g_nativeWorld.Quarantine("idle capture world identity or isolation changed"); return; }
        try
        {
            static_assert(std::is_nothrow_move_assignable_v<std::string>);
            g_activeRequest.emplace(); // Allocate the private slot BEFORE removing a FIFO entry.
            if (!g_fifo->Remove(g_activeRequest->BlueprintPath))
            { g_activeRequest.reset(); return; }
        }
        catch (...)
        {
            // SpriteFifo moves a std::string then pops it without allocation;
            // allocation/lock failure precedes removal. No native request began.
            g_activeRequest.reset();
            LOG_WARN("MiniMap: building capture FIFO admission unavailable; queued work retained");
            return;
        }
        ServiceActiveRequest();
    }

    void EndSession()
    {
        g_sessionActive = false; // Cancel CPU publication too: no late cache write or feedback.
        bool gcDeferred = false;
        if (g_activeRequest && !g_activeRequest->Retire(&gcDeferred))
        {
            // No native cleanup began. Keep the canceled request/world intact;
            // a later lifecycle call may retry, but no background work is added.
            if (gcDeferred) return;
            // No background session-end service. A pending fence/Job preserves
            // the request and dependent world; cancellation is not retirement.
            if (g_activeRequest->Phase != RequestPhase::Quarantined)
                g_activeRequest->Quarantine("session ended before native capture retirement completed");
            return;
        }
        if (g_nativeWorld.Retire(g_activeRequest && g_activeRequest->HasNativeOwnership()))
            g_activeRequest.reset();
    }

    void Shutdown()
    {
        EndSession();
        g_fifo = nullptr;
        g_feedback = nullptr;
    }
}

#endif
