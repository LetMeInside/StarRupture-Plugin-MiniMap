#if defined(MODLOADER_CLIENT_BUILD)

#include "Map/Buildings/BuildingSpriteRenderer.h"
#include "Map/Map.h"
#include "Native/NativeApi.h"
#include "Native/DefinitionAcquisitionCore.h"
#include "plugin_helpers.h"

#include <Engine_classes.hpp>
#include <SDK/Chimera_classes.hpp>
#include <SDK/MassEntity_classes.hpp>
#include <SDK/MassReplication_classes.hpp>
#include <Windows.h>

#include <array>
#include <optional>
#include <string>
#include <type_traits>

namespace
{
    struct ActiveRequest
    {
        std::string BlueprintPath;
    };

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
            // No request resources exist yet. Future capture stages must retire
            // them before entering this world-only teardown.
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
        if (!g_fifo || g_sessionActive || !g_nativeWorld.CanStartSession()) return;
        g_activeRequest.reset();
        g_sessionActive = true;
    }

    void Tick(float deltaTime)
    {
        (void)deltaTime;
        if (!g_fifo || !g_sessionActive) return;
        // Stage 7.5C.2A: deliberately no world creation, FIFO consumption or native service.
    }

    void EndSession()
    {
        g_sessionActive = false;
        if (g_nativeWorld.Retire(g_activeRequest.has_value()))
            g_activeRequest.reset();
    }

    void Shutdown()
    {
        EndSession();
        g_fifo = nullptr;
    }
}

#endif
