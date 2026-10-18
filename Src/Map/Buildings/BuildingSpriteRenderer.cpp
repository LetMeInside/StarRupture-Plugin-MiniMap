#if defined(MODLOADER_CLIENT_BUILD)

#include "Map/Buildings/BuildingSpriteRenderer.h"
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

    template<class T> bool Read(uintptr_t address, T& value)
    { return N::ReadMemory(address, &value, sizeof(value)); }
    struct SlotReader
    {
        template<class T> bool Read(uintptr_t address, T& value)
        { return N::ReadMemory(address, &value, sizeof(value)); }
    };

    bool GameThread() noexcept
    {
        const auto* api = N::Get();
        __try { return api && api->player.isGameThread && api->player.isGameThread(); }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
    }

    bool Facilities()
    {
        const auto* api = N::Get();
        return N::IsInitialized() && api && GameThread() && api->asset.objectArray &&
            api->asset.retainObject && api->asset.releaseObject && api->asset.exitPurge &&
            api->asset.activeGarbageCollection && api->player.engineStorage &&
            api->world.createWorld && api->world.initWorld && api->world.updateWorldComponents &&
            api->world.cleanupWorld && api->world.getWorldContextFromWorld && api->world.getSubsystemInternal &&
            api->world.isGarbageCollectingAndLocking && api->world.isIncrementalPurgePending &&
            api->world.isIncrementalReachabilityPending && api->world.getTickableTickType &&
            api->world.isVisibilityRequestPending;
    }

    bool QuietGC() noexcept
    {
        if (!Facilities()) return false;
        const auto* api = N::Get();
        uint8_t exitPurge{}; bool collecting{};
        __try
        {
            return Read(reinterpret_cast<uintptr_t>(api->asset.exitPurge), exitPurge) && !exitPurge &&
                Read(reinterpret_cast<uintptr_t>(api->asset.activeGarbageCollection), collecting) && !collecting &&
                !api->world.isGarbageCollectingAndLocking() && !api->world.isIncrementalPurgePending() &&
                !api->world.isIncrementalReachabilityPending();
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
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

        void Quarantine(const char* reason)
        {
            if (Phase != WorldPhase::Quarantined)
                LOG_ERROR("MiniMap: standalone capture world quarantined: %s; native ownership retained", reason);
            Phase = WorldPhase::Quarantined;
        }
        bool CanStartSession() const
        { return Phase == WorldPhase::Inactive || Phase == WorldPhase::Released || Phase == WorldPhase::Unavailable; }

        bool Identify(uintptr_t object, ObjectIdentity& identity, size_t extent = 0x28)
        {
            const auto* api = N::Get();
            if (!api || !N::ValidateObjectIdentity(object, reinterpret_cast<uintptr_t>(api->asset.objectArray), extent)) return false;
            int32_t index{}; Slots::Slot slot; SlotReader reader;
            if (!Read(object + 0xc, index) ||
                Slots::ReadSlot(reader, reinterpret_cast<uintptr_t>(api->asset.objectArray), index, slot) != Slots::Status::Success ||
                slot.Object != object || (slot.Flags & 0x10200000)) return false;
            identity = {object, index, slot.Serial};
            return true;
        }
        bool CheckOwned(NativeReference& ref)
        {
            if (ref.State != Ownership::Held || !GameThread()) return false;
            ObjectIdentity now;
            if (!Identify(ref.Object.Bits, now) || now.Index != ref.Object.Index) return false;
            if (now.Serial == ref.Object.Serial) return true;
            // UE may assign a serial to a continuously retained zero-serial item.
            // Never allocate one or adopt a changed nonzero serial.
            Slots::Slot slot; SlotReader reader;
            if (ref.Object.Serial != 0 || now.Serial <= 0 ||
                Slots::ReadSlot(reader, reinterpret_cast<uintptr_t>(N::Get()->asset.objectArray), now.Index, slot) != Slots::Status::Success ||
                slot.Object != now.Bits || slot.Serial != now.Serial || !(slot.Flags & 0x20000000)) return false;
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
        bool Is(uintptr_t object, ClassId expected)
        {
            uintptr_t type{}; ObjectIdentity identity;
            auto& anchor = MetadataClasses[expected].Ref;
            if (!CheckOwned(anchor) || !Identify(object, identity) || !Read(object + 0x10, type)) return false;
            for (unsigned depth = 0; type && depth < 64; ++depth)
            {
                if (!Identify(type, identity, sizeof(SDK::UClass))) return false;
                if (type == anchor.Object.Bits) return true;
                uintptr_t parent{};
                if (!Read(type + 0x40, parent) || parent == type) return false;
                type = parent;
            }
            return false;
        }
        bool NoForeignRoot(NativeReference& ref)
        {
            uint32_t flags{}; Slots::Slot slot; SlotReader reader;
            return CheckOwned(ref) && Read(ref.Object.Bits + 8, flags) && !(flags & 2) &&
                Slots::ReadSlot(reader, reinterpret_cast<uintptr_t>(N::Get()->asset.objectArray), ref.Object.Index, slot) == Slots::Status::Success &&
                !(slot.Flags & 0x40000000);
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
        bool ObserveGameplay(Baseline& baseline)
        {
            const auto game = reinterpret_cast<uintptr_t>(MiniMapMap::GetWorld());
            uintptr_t engine{}, instance{}, contexts{}, current{}; int32_t capacity{}; uint8_t flags{};
            if (!Read(reinterpret_cast<uintptr_t>(N::Get()->player.engineStorage), engine) ||
                !Is(engine, Engine) || !Is(game, World) || !Read(game + 0x18d, flags) || (flags & 0x20) ||
                !Read(game + 0x18b, flags) || !(flags & 0x40) ||
                !Identify(game, baseline.Gameplay) || !Identify(engine, baseline.EngineObject) ||
                !Read(game + 0x230, instance) || !Is(instance, GameInstance) || !Identify(instance, baseline.GameInstanceObject) ||
                !Invoke(Operation::Context, engine, game, &baseline.Context) || !baseline.Context ||
                !Read(baseline.Context + 0x2c0, current) || current != game ||
                !Read(engine + 0x11c8, contexts) || !Read(engine + 0x11d0, baseline.ContextCount) ||
                !Read(engine + 0x11d4, capacity) || baseline.ContextCount < 1 || capacity < baseline.ContextCount || capacity > 4096 ||
                !N::IsMemoryRangeReadable(contexts, size_t(baseline.ContextCount) * 8, 8)) return false;
            baseline.ContextHash = 1469598103934665603ULL;
            for (int32_t i = 0; i < baseline.ContextCount; ++i)
            {
                uintptr_t context{};
                if (!Read(contexts + size_t(i) * 8, context) || !N::IsMemoryRangeReadable(context, 0x2c8, 8) ||
                    !Read(context + 0x2c0, current)) return false;
                baseline.ContextHash = ((baseline.ContextHash ^ context) * 1099511628211ULL ^ current) * 1099511628211ULL;
            }
            return true;
        }
        bool GameplayUnchanged()
        { Baseline after; return ObserveGameplay(after) && after == Before; }
        bool Collection(uintptr_t world, bool initialized)
        {
            const auto collection = world + 0x9f0;
            uintptr_t outer{}; bool populating{}, iterating{}; int32_t entries{}, free{}, arrays{}, arraysFree{};
            return Read(collection + 0xb0, outer) && Read(collection + 0xb8, populating) && Read(collection + 0xb9, iterating) &&
                Read(collection + 0x10, entries) && Read(collection + 0x3c, free) &&
                Read(collection + 0x60, arrays) && Read(collection + 0x8c, arraysFree) &&
                entries >= 0 && free >= 0 && free <= entries && arrays >= 0 && arraysFree >= 0 && arraysFree <= arrays &&
                (initialized || (entries == free && arrays == arraysFree)) &&
                outer == (initialized ? world : 0) && !populating && !iterating;
        }
        bool PrivateState(bool initialized)
        {
            const auto world = Private.Object.Bits;
            uintptr_t package{}, gameplayPackage{}, level{}, owner{}, settings{}, type{}, scene{}, gameplayScene{}, instance{}, net{}, context{};
            uint8_t worldType{}, a{}, b{}, c{}, ever{};
            if (!CheckOwned(Private) || world == Before.Gameplay.Bits || !Is(world, World) ||
                !NoForeignRoot(Private) || !CheckOwned(Package) || !NoForeignRoot(Package) ||
                !Read(world + 0x20, package) || package != Package.Object.Bits ||
                !Read(Before.Gameplay.Bits + 0x20, gameplayPackage) || package == gameplayPackage ||
                !Read(world + 0x18a, worldType) || worldType != 5 || !Read(world + 0x18b, a) ||
                !Read(world + 0x18c, b) || !Read(world + 0x18d, c) || !Read(world + 0x18f, ever) ||
                bool(a & 0x40) != initialized || (b & 0x80) || (c & 0x21) || bool(ever & 2) != initialized ||
                !Read(world + 0x258, scene) || bool(scene) != initialized ||
                !Read(world + 0x230, instance) || instance || !Read(world + 0x38, net) || net ||
                !Invoke(Operation::Context, Before.EngineObject.Bits, world, &context) || context ||
                !CheckOwned(PersistentLevel) || !CheckOwned(WorldSettings) ||
                !Read(world + 0x30, level) || level != PersistentLevel.Object.Bits ||
                !Read(level + 0xc0, owner) || owner != world || !Read(level + 0x2c8, settings) || settings != WorldSettings.Object.Bits ||
                !Read(settings + 0x10, type) || type != MetadataClasses[Settings].Ref.Object.Bits || !Collection(world, initialized)) return false;
            if (initialized)
            {
                if ((Scene && Scene != scene) || !N::IsMemoryRangeReadable(scene, 8, 8) ||
                    !Read(Before.Gameplay.Bits + 0x258, gameplayScene) || !gameplayScene || scene == gameplayScene) return false;
                Scene = scene;
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

        // Intentionally has NO call site in this stage. Activation requires the
        // legacy native owner to be removed first; Tick must remain inert.
        [[maybe_unused]] void Create()
        {
            if (!CanStartSession()) return;
            Before = {};
            Scene = 0;
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

        bool CleanupPreconditions()
        {
            const auto world = Private.Object.Bits;
            int32_t count{}; uintptr_t visibility{};
            return QuietGC() && GameplayUnchanged() && PrivateState(true) &&
                Read(world + 0x98, count) && !count && Read(world + 0xa8, count) && !count &&
                Read(world + 0xb8, count) && !count && Read(world + 0xc0, count) && !count &&
                Invoke(Operation::Visibility, world, 0, &visibility) && !visibility;
        }
        bool CleanupPostconditions()
        {
            const auto world = Private.Object.Bits;
            uint8_t flags{}; bool ticking{}; uintptr_t tickType{}, instance{}, net{}, scene{}, context{};
            if (!QuietGC() || !GameplayUnchanged() || !CheckOwned(Private) ||
                !Read(world + 0x18b, flags) || (flags & 0x40) || !Collection(world, false) ||
                !CheckOwned(Subsystems[0]) || !Read(Subsystems[0].Object.Bits + 0x38, ticking) || ticking ||
                !Invoke(Operation::TickType, Subsystems[0].Object.Bits + 0x30, 0, &tickType) || tickType != 2 ||
                !Read(world + 0x230, instance) || instance || !Read(world + 0x38, net) || net ||
                !Read(world + 0x258, scene) || scene != Scene || !NoForeignRoot(Private) || !NoForeignRoot(Package) ||
                !Invoke(Operation::Context, Before.EngineObject.Bits, world, &context) || context) return false;
            for (auto& ref : Subsystems)
            {
                uintptr_t collection{};
                if (!CheckOwned(ref) || !Read(ref.Object.Bits + 0x28, collection) || collection) return false;
            }
            return GameplayUnchanged() && QuietGC();
        }
        bool Retire(bool requestActive)
        {
            if (CanStartSession()) return true; // No native ownership remains in these states.
            if (Phase == WorldPhase::Quarantined) return false;
            // Request resources must be retired before world-only teardown.
            if (requestActive || Phase != WorldPhase::Ready || !Facilities() || !CleanupPreconditions())
            { Quarantine("world cleanup admission or active-resource exclusion failed"); return false; }
            Phase = WorldPhase::Retiring;
            if (!Invoke(Operation::Cleanup, Private.Object.Bits) || !CleanupPostconditions())
            { Quarantine("world cleanup interrupted or subsystem detachment invalid"); return false; }
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
        std::vector<uint8_t> Rgba; // Full canvas; retained with Center/Extent for later cache persistence.
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
        { Phase = RequestPhase::Quarantined; g_nativeWorld.Quarantine(reason); }
        bool Ready()
        { return g_nativeWorld.Phase == WorldPhase::Ready && QuietGC() && g_nativeWorld.GameplayUnchanged() && g_nativeWorld.PrivateState(true); }
        bool FenceAvailable()
        {
            bool available{};
            if (!Ready() || !ReadbackAvailable(false, available) || !available) return false;
            const auto& api = N::Get()->texture;
            return api.renderFenceConstruct && api.renderFenceBegin && api.renderFencePoll && api.renderFenceDestroy;
        }
        bool Call(ResourceOperation operation, uintptr_t object, uintptr_t argument = 0, uintptr_t* result = nullptr)
        {
            if (Ready() && ResourceCall(operation, object, argument, result)) return true;
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
            if (!find) return 0;
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
            if (Phase == RequestPhase::Quarantined || !object) return false;
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
            if (!Fence.Poll(operations)) return false; // Nonblocking; faults quarantine through operations.
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
                    if (report.Current != MiniMapReadbackNative::Phase::CleanupComplete || report.Callables || report.Outstanding) return false;
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
        bool Retire()
        {
            if (Phase == RequestPhase::Quarantined) return false;
            if (!HasNativeOwnership())
            {
                if (Phase != RequestPhase::Failed)
                    Phase = CaptureSubmitted && Rgba.empty() ? RequestPhase::Failed : RequestPhase::Retired;
                return true;
            }
            if (!Ready()) { Quarantine("capture retirement world admission failed"); return false; }
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
        void Fail()
        { Phase = RequestPhase::Failed; Retire(); }

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
            if (!report.HasPixels || !report.NativeEnqueueEverInvoked ||
                report.Enqueue != MiniMapCaptureRetirement::EnqueueState::ReadbackCompleted || (report.Visited & required) != required ||
                !pixels || !stats || pixels->size() != 256 * 256 * 8 ||
                !MiniMapCapturePng::Convert(pixels->data(), pixels->size(), 256, 256, 256, 256, image) ||
                image.Rgba.size() != 256 * 256 * 4 || image.Sanitized || stats->Nonfinite || !stats->Foreground || !stats->Background) return;
            Rgba = std::move(image.Rgba); // CPU result only; persistence is a later stage.
        }
#endif
        // Deliberately uncalled by public Tick. One iteration services only this
        // request; world creation, FIFO admission and callbacks remain disabled.
        [[maybe_unused]] void Service()
        {
            if (Phase == RequestPhase::Quarantined || Phase == RequestPhase::Failed || Phase == RequestPhase::Retired) return;
            if (!GameThread() || g_captureServiceTick == (std::numeric_limits<uint64_t>::max)())
            { Quarantine("capture service thread or tick counter invalid"); return; }
            ++g_captureServiceTick;
            try
            {
                if (Phase == RequestPhase::Waiting) { Prepare(); return; }
                if (!Ready()) { Quarantine("capture service native world changed"); return; }
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
                        if (MiniMapReadbackNative::ExternalRejectedWithoutReadback(Job)) Retire();
                        else Quarantine("readback submission or native ownership uncertain");
                        return;
                    }
                    if (report.Current != MiniMapReadbackNative::Phase::CleanupComplete || report.Callables || report.Outstanding) return;
                    if (report.Result == MiniMapReadbackNative::Outcome::Pending)
                    { Quarantine("readback cleanup completed without a terminal outcome"); return; }
                    if (report.Result == MiniMapReadbackNative::Outcome::Verified)
                    {
                        // Allocation/conversion failure is a CPU result failure,
                        // not uncertain GPU ownership: still retire native work.
                        try { ConvertReadback(report); }
                        catch (...) { Rgba.clear(); }
                    }
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
        // Reached only by the dormant private service. Future activation supplies
        // exactly one copied FIFO path on the game thread.
        [[maybe_unused]] void Prepare()
        {
            if (Phase != RequestPhase::Waiting) return;
            std::array<wchar_t, 1025> wide{};
            if (!Path(BlueprintPath, wide)) { Fail(); return; }
            if (!Ready()) return;
            const auto& api = *N::Get();
            if (!api.asset.spawnObject || !api.asset.setObjectFlags || !api.player.registerComponent ||
                !api.player.unregisterComponent || !api.player.destroyComponent || !api.world.destroyActor ||
                !api.texture.initRenderTarget || !api.texture.getRenderTargetResource ||
                !api.texture.renderFenceConstruct || !api.texture.renderFenceBegin ||
                !api.texture.renderFencePoll || !api.texture.renderFenceDestroy) return;
            bool available{};
            if (!ReadbackAvailable(true, available)) { Quarantine("readback prerequisite inspection interrupted"); return; }
            if (!available) return;
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
            { if (!DropMetadata()) Quarantine("waiting class metadata release uncertain"); return; }
            ObjectIdentity identity; uintptr_t type{}, cdo{};
            if (!g_nativeWorld.Identify(object, identity, sizeof(SDK::UBlueprintGeneratedClass)) || !Read(object + 0x10, type))
            { Quarantine("resident generated-class identity invalid"); return; }
            if (type != Metadata[Generated].Object.Bits) { Fail(); return; }
            if (!Keep(object, Metadata[Blueprint])) return;
            bool isActor{};
            if (!ActorSubclass(object, Metadata[ActorType].Object.Bits, &isActor)) { Quarantine("Actor inheritance call interrupted"); return; }
            if (!isActor) { Fail(); return; }
            if (!Read(object + offsetof(SDK::UClass, ClassDefaultObject), cdo) || !cdo ||
                !g_nativeWorld.Identify(cdo, identity) || !Read(cdo + 0x10, type) || type != object)
            { Fail(); return; }
            if (!Keep(cdo, Metadata[DefaultObject])) return;
            Phase = RequestPhase::Preparing;
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
                Fail(); return;
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
            if (!resource) { Fail(); return; }
#if defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
            Job = MiniMapReadbackNative::CreateExternalJob(reinterpret_cast<void*>(resource), 256, 256);
            if (!Job) { Fail(); return; }
#endif
            if (!Ready() || !ActorState(true)) { Quarantine("prepared request ownership invalid"); return; }
            PreparedTick = g_captureServiceTick;
            Phase = RequestPhase::Prepared;
        }
    };

    MiniMapSpriteQueues::SpriteFifo* g_fifo = nullptr;
    bool g_sessionActive = false;
    std::optional<ActiveRequest> g_activeRequest;
}

namespace MiniMapBuildingSpriteRenderer
{
    void Initialize(MiniMapSpriteQueues::SpriteFifo& fifo)
    {
        g_fifo = &fifo;
        g_sessionActive = false;
    }

    void InitializeSession()
    {
        if (!g_fifo || g_sessionActive || g_activeRequest || !g_nativeWorld.CanStartSession()) return;
        g_sessionActive = true;
    }

    void Tick(float deltaTime)
    {
        (void)deltaTime;
        if (!g_fifo || !g_sessionActive) return;
        // Stage 7.5C.2C: deliberately no world creation, request preparation,
        // FIFO consumption, scene capture or native service.
    }

    void EndSession()
    {
        g_sessionActive = false;
        if (g_activeRequest && !g_activeRequest->Retire())
        {
            // No background session-end service. A pending fence/Job preserves
            // the request and dependent world; cancellation is not retirement.
            if (g_activeRequest->Phase != RequestPhase::Quarantined)
                g_activeRequest->Quarantine("session ended before native capture retirement completed");
            return;
        }
        if (g_nativeWorld.Retire(false))
            g_activeRequest.reset();
    }

    void Shutdown()
    {
        EndSession();
        g_fifo = nullptr;
    }
}

#endif
