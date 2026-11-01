#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingComplexWorld.h"
#include "Map/Map.h"
#include "Map/Terrain/TerrainSource.h"
#include "Native/MassLifecycle.h"
#include "Native/NativeApi.h"
#include "Services/Objects.h"
#include "Services/Runtime.h"
#include <SDK/Chimera_classes.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>

namespace
{
    namespace B = MiniMapBuildingComplexIndex;
    namespace L = MiniMapMassLifecycle;
    using Event = L::Event;
    using Handle = MiniMapNative::MassEntityHandle;
    constexpr size_t kQuerySize = 0x350;
    constexpr size_t kQueryAlignment = 0x10;
    constexpr size_t kEventBatch = 256;

    B::Index Index;
    MiniMapBuildingComplexWorld::Status Current;
    SDK::UScriptStruct* BuildingTag = nullptr;
    bool BuildingTagRooted = false;
    uint64_t AppliedEpoch = 0;
    uint64_t AppliedSequence = 0;

    bool AcquireBuildingTag()
    {
        if (BuildingTag) return true;
        auto* objects = MiniMap::Services::TryObjects();
        const auto* native = MiniMapNative::Get();
        if (!objects || !objects->Ready() || !native || !native->asset.retainObject ||
            !native->asset.releaseObject) return false;
        try
        {
            const auto found = objects->FindResident(nullptr, nullptr,
                L"/Script/Chimera.CrMassBuildingTag", false);
            auto* tag = found.value ? static_cast<SDK::UScriptStruct*>(*found.value) : nullptr;
            if (!tag) return false;
            native->asset.retainObject(tag);
            BuildingTag = tag;
            BuildingTagRooted = true;
            return true;
        }
        catch (...) { return false; }
    }

    struct Query
    {
        alignas(kQueryAlignment) std::array<std::byte, kQuerySize> Storage{};
        bool Constructed = false;
        const MiniMapNative::NativeApi* Native = nullptr;
        ~Query() { if (Constructed && Native && Native->mass.queryDestruct) Native->mass.queryDestruct(Storage.data()); }
    };

    bool BuildCensus(uint64_t generation, L::SeedToken token)
    {
        const auto* native = MiniMapNative::Get();
        auto* world = MiniMapMap::GetWorld();
        if (!native || !world || !AcquireBuildingTag() ||
            !native->mass.getMassEntitySubsystem || !native->mass.queryConstruct ||
            !native->mass.queryDestruct || !native->mass.addTagRequirement ||
            !native->mass.getMatchingEntityHandles || !native->mass.isEntityValid ||
            !native->texture.memoryFree) return false;

        B::WorldOrigin origin;
        if (!MiniMapTerrainSource::TryGetWorldOrigin(origin.X, origin.Y)) return false;
        origin.Valid = true;

        auto* subsystem = native->mass.getMassEntitySubsystem(world);
        const void* manager = nullptr;
        // Match the established MiniMap Mass query contract: the subsystem's
        // manager shared-pointer storage is supplied to the typed query API.
        if (subsystem) std::memcpy(&manager, reinterpret_cast<const uint8_t*>(subsystem) + 0x38, sizeof(manager));
        if (!manager) return false;

        Query query; query.Native = native;
        try
        {
            native->mass.queryConstruct(query.Storage.data(), reinterpret_cast<const uint8_t*>(subsystem) + 0x38);
            query.Constructed = true;
            native->mass.addTagRequirement(query.Storage.data(), BuildingTag, SDK::EMassFragmentPresence::All);
        }
        catch (...) { return false; }

        MiniMapNative::MassEntityHandleArray handles{};
        struct HandlesOwner
        {
            const MiniMapNative::NativeApi& Native;
            MiniMapNative::MassEntityHandleArray& Handles;
            ~HandlesOwner() { if (Handles.Data) Native.texture.memoryFree(Handles.Data); }
        } handlesOwner{*native, handles};

        MiniMapNative::MassEntityHandleArray* returned = nullptr;
        try { returned = native->mass.getMatchingEntityHandles(query.Storage.data(), &handles); }
        catch (...) { return false; }
        if (returned != &handles || handles.Num < 0 || handles.Max < handles.Num ||
            (handles.Num && !handles.Data)) return false;

        const auto lifecycle = L::Status();
        if (!L::InAdmittedService() || lifecycle.WorldGeneration != generation ||
            lifecycle.Epoch != token.Epoch || lifecycle.LossRevision != token.LossRevision ||
            MiniMapMap::GetWorldGeneration() != generation || MiniMapMap::GetWorld() != world) return false;

        if (!Index.BeginWorld(generation, {origin.X, origin.Y, true})) return false;
        Current.State = MiniMapBuildingComplexWorld::MembershipState::CensusInProgress;

        for (int32_t i = 0; i < handles.Num; ++i)
        {
            const Handle id = handles.Data[i];
            if (id.Index < 0 || id.SerialNumber <= 0 || !native->mass.isEntityValid(manager, id))
            { Index.ResetWorld(generation); return false; }

            auto source = std::make_shared<B::SourcePayload>();
            source->SourceGeneration = generation;
            source->Support = B::SupportState::Unknown;
            B::BuildingRecord record;
            record.Key = {id.Index, id.SerialNumber};
            record.WorldGeneration = generation;
            record.Source = std::move(source);
            B::ChangeSet changes;
            const auto result = Index.Upsert(std::move(record), changes);
            if (result == B::UpsertResult::InvalidInput || result == B::UpsertResult::AllocationFailure)
            { Index.ResetWorld(generation); return false; }
        }

        // Replay only events strictly after the seed watermark. AcceptSeed is
        // the lifecycle's atomic empty-journal handoff to incremental service.
        uint64_t replaySequence = token.Watermark;
        std::array<Event, kEventBatch> events{};
        for (;;)
        {
            const size_t count = L::Drain(events);
            if (!count) break;
            for (size_t i = 0; i < count; ++i)
            {
                const Event& event = events[i];
                if (event.Epoch != token.Epoch)
                { Index.ResetWorld(generation); return false; }
                if (event.Sequence <= token.Watermark) continue;
                if (replaySequence == (std::numeric_limits<uint64_t>::max)() || event.Sequence != replaySequence + 1)
                { Index.ResetWorld(generation); return false; }
                replaySequence = event.Sequence;

                B::ChangeSet changes;
                if (event.Kind == L::EventKind::Add)
                {
                    // The observer event establishes BuildingTag membership;
                    // recheck only liveness so a stale Add cannot resurrect a
                    // removed handle. Visual fragments may still be absent.
                    auto* currentSubsystem = native->mass.getMassEntitySubsystem(world);
                    const void* currentManager = nullptr;
                    if (currentSubsystem) std::memcpy(&currentManager,
                        reinterpret_cast<const uint8_t*>(currentSubsystem) + 0x38, sizeof(currentManager));
                    if (currentManager != manager || !native->mass.isEntityValid(manager, event.Entity))
                        continue;
                    auto source = std::make_shared<B::SourcePayload>();
                    source->SourceGeneration = generation;
                    source->Support = B::SupportState::Unknown;
                    B::BuildingRecord record;
                    record.Key = {event.Entity.Index, event.Entity.SerialNumber};
                    record.WorldGeneration = generation;
                    record.Source = std::move(source);
                    const auto result = Index.Upsert(std::move(record), changes);
                    if (result == B::UpsertResult::InvalidInput || result == B::UpsertResult::AllocationFailure)
                    { Index.ResetWorld(generation); return false; }
                }
                else
                {
                    const auto result = Index.Remove({event.Entity.Index, event.Entity.SerialNumber}, generation, changes);
                    if (result == B::RemoveResult::AllocationFailure)
                    { Index.ResetWorld(generation); return false; }
                }
            }
        }

        // The existing lifecycle API takes the current loading predicate. The
        // accepted admission contract here is its world/experience and observed
        // restore barriers; no private save state or extra readiness probe.
        if (!L::AcceptSeed(token, false) || MiniMapMap::GetWorldGeneration() != generation ||
            MiniMapMap::GetWorld() != world)
        { Index.ResetWorld(generation); return false; }
        AppliedEpoch = token.Epoch;
        AppliedSequence = replaySequence;
        Current.RestoreEpoch = AppliedEpoch;
        Current.AppliedSequence = AppliedSequence;
        Current.State = MiniMapBuildingComplexWorld::MembershipState::MembershipEstablished;
        return true;
    }

    bool ApplyIncremental(const MiniMapNative::NativeApi& native, const Event& event, uint64_t generation)
    {
        if (AppliedSequence == (std::numeric_limits<uint64_t>::max)() ||
            event.Epoch != AppliedEpoch || event.Sequence != AppliedSequence + 1 ||
            event.Entity.Index < 0 || event.Entity.SerialNumber <= 0) return false;
        B::ChangeSet changes;
        if (event.Kind == L::EventKind::Remove)
        {
            if (Index.Remove({event.Entity.Index, event.Entity.SerialNumber}, generation, changes) == B::RemoveResult::AllocationFailure)
                return false;
        }
        else
        {
            auto* world = MiniMapMap::GetWorld();
            auto* subsystem = native.mass.getMassEntitySubsystem ? native.mass.getMassEntitySubsystem(world) : nullptr;
            const void* manager = nullptr;
            if (subsystem) std::memcpy(&manager, reinterpret_cast<const uint8_t*>(subsystem) + 0x38, sizeof(manager));
            if (!manager || !native.mass.isEntityValid || !native.mass.isEntityValid(manager, event.Entity))
            {
                // A candidate Add that is no longer live invalidates the
                // incremental handoff; the next census reconciles membership.
                return false;
            }
            auto source = std::make_shared<B::SourcePayload>();
            source->SourceGeneration = generation;
            source->Support = B::SupportState::Unknown;
            B::BuildingRecord record;
            record.Key = {event.Entity.Index, event.Entity.SerialNumber};
            record.WorldGeneration = generation;
            record.Source = std::move(source);
            const auto result = Index.Upsert(std::move(record), changes);
            if (result == B::UpsertResult::InvalidInput || result == B::UpsertResult::AllocationFailure) return false;
        }
        AppliedSequence = event.Sequence;
        Current.AppliedSequence = AppliedSequence;
        return true;
    }

    void ServiceImpl()
    {
        auto lifecycle = L::Status();
        const uint64_t generation = Current.WorldGeneration;
        if (!generation || !MiniMapMap::GameplayReady() || MiniMapMap::GetWorldGeneration() != generation ||
            lifecycle.WorldGeneration != generation || !L::InAdmittedService()) return;

        if (lifecycle.ObservationLost || lifecycle.SeedRequired ||
            Current.State != MiniMapBuildingComplexWorld::MembershipState::MembershipEstablished ||
            lifecycle.Epoch != AppliedEpoch)
        {
            Current.State = MiniMapBuildingComplexWorld::MembershipState::ReseedRequired;
            Index.ResetWorld(generation);
            const auto token = L::BeginSeed(false);
            if (!token.Epoch) return;
            Current.State = MiniMapBuildingComplexWorld::MembershipState::CensusInProgress;
            if (!BuildCensus(generation, token))
            {
                if (MiniMapMap::GetWorldGeneration() == generation) Index.ResetWorld(generation);
                Current.State = MiniMapBuildingComplexWorld::MembershipState::ReseedRequired;
            }
            return;
        }

        const auto* native = MiniMapNative::Get();
        if (!native)
        {
            Index.ResetWorld(generation);
            Current.State = MiniMapBuildingComplexWorld::MembershipState::ReseedRequired;
            return;
        }
        std::array<Event, kEventBatch> events{};
        for (;;)
        {
            const size_t count = L::Drain(events);
            if (!count) break;
            for (size_t i = 0; i < count; ++i)
                if (!ApplyIncremental(*native, events[i], generation))
                {
                    Index.ResetWorld(generation);
                    Current.State = MiniMapBuildingComplexWorld::MembershipState::ReseedRequired;
                    return;
                }
        }
        lifecycle = L::Status();
        if (lifecycle.ObservationLost || lifecycle.Epoch != AppliedEpoch ||
            (lifecycle.Sequence != AppliedSequence && lifecycle.Queued == 0))
        {
            Index.ResetWorld(generation);
            Current.State = MiniMapBuildingComplexWorld::MembershipState::ReseedRequired;
        }
    }

    void Service() noexcept
    {
        try { ServiceImpl(); }
        catch (...)
        {
            if (Current.WorldGeneration) Index.ResetWorld(Current.WorldGeneration);
            Current.State = MiniMapBuildingComplexWorld::MembershipState::ReseedRequired;
        }
    }
}

namespace MiniMapBuildingComplexWorld
{
    void BeginWorld(uint64_t generation) noexcept
    {
        Current = {};
        Current.WorldGeneration = generation;
        Current.State = generation ? MembershipState::InitialCensusPending : MembershipState::WorldUnavailable;
        AppliedEpoch = AppliedSequence = 0;
        Index.ResetWorld(generation);
        if (!generation || !L::SetConsumer(&Service))
            Current.State = MembershipState::WorldUnavailable;
        else
        {
            const auto lifecycle = MiniMapMassLifecycle::Status();
            if (!lifecycle.Available || lifecycle.Cancelled || !lifecycle.Consumer)
                Current.State = MembershipState::WorldUnavailable;
        }
    }

    void EndWorld(uint64_t nextGeneration) noexcept
    {
        Index.ResetWorld(nextGeneration);
        Current = {};
        Current.WorldGeneration = nextGeneration;
        Current.State = nextGeneration ? MembershipState::InitialCensusPending : MembershipState::WorldUnavailable;
        AppliedEpoch = AppliedSequence = 0;
    }

    void Shutdown() noexcept
    {
        EndWorld();
        if (BuildingTag && BuildingTagRooted)
        {
            if (const auto* native = MiniMapNative::Get(); native && native->asset.releaseObject)
                native->asset.releaseObject(BuildingTag);
        }
        BuildingTag = nullptr;
        BuildingTagRooted = false;
    }

    Status GetStatus() noexcept
    {
        Status result = Current;
        if (result.State == MembershipState::MembershipEstablished)
        {
            const auto lifecycle = L::Status();
            if (lifecycle.WorldGeneration != result.WorldGeneration || lifecycle.ObservationLost ||
                lifecycle.SeedRequired || lifecycle.Epoch != AppliedEpoch ||
                lifecycle.Sequence != AppliedSequence || lifecycle.Queued || lifecycle.InFlight)
                result.State = MembershipState::EventReplayPending;
        }
        return result;
    }
    MiniMapBuildingComplexIndex::Index& GetIndex() noexcept { return Index; }
}
#endif
