#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingComplexWorld.h"
#include "BuildingComplexSourceAcquisition.h"
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
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <unordered_set>
#include <vector>

namespace
{
    namespace B = MiniMapBuildingComplexIndex;
    namespace L = MiniMapMassLifecycle;
    using Event = L::Event;
    using Handle = MiniMapNative::MassEntityHandle;
    constexpr size_t kQuerySize = 0x350;
    constexpr size_t kQueryAlignment = 0x10;
    constexpr size_t kEventBatch = 256;

    struct MassKeyHash
    {
        size_t operator()(B::MassKey key) const noexcept
        {
            const uint64_t packed = uint64_t(uint32_t(key.Index)) |
                (uint64_t(uint32_t(key.Serial)) << 32);
            return std::hash<uint64_t>{}(packed);
        }
    };

    B::Index Index;
    MiniMapBuildingComplexWorld::Status Current;
    SDK::UScriptStruct* BuildingTag = nullptr;
    bool BuildingTagRooted = false;
    uint64_t AppliedEpoch = 0;
    uint64_t AppliedSequence = 0;
    std::deque<B::MassKey> SourceQueue;
    std::unordered_set<B::MassKey, MassKeyHash> SourceQueued;

    void EnqueueSource(B::MassKey key)
    {
        if (SourceQueued.insert(key).second) SourceQueue.push_back(key);
    }

    void ClearSourceQueue() noexcept
    {
        SourceQueue.clear();
        SourceQueued.clear();
    }

    void ResetIndex(uint64_t generation) noexcept
    {
        Index.ResetWorld(generation);
        ClearSourceQueue();
    }

    bool EnsureMembershipRecord(B::MassKey key, uint64_t generation)
    {
        std::shared_ptr<const B::BuildingRecord> existing;
        if (Index.TryGetRecord(key, generation, existing))
        {
            EnqueueSource(key);
            return true;
        }
        auto source = std::make_shared<B::SourcePayload>();
        source->SourceGeneration = generation;
        source->Support = B::SupportState::Unknown;
        B::BuildingRecord record;
        record.Key = key;
        record.WorldGeneration = generation;
        record.Source = std::move(source);
        B::ChangeSet changes;
        const auto result = Index.Upsert(std::move(record), changes);
        if (result == B::UpsertResult::InvalidInput || result == B::UpsertResult::AllocationFailure) return false;
        EnqueueSource(key);
        return true;
    }

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
        ClearSourceQueue();
        Current.State = MiniMapBuildingComplexWorld::MembershipState::CensusInProgress;

        for (int32_t i = 0; i < handles.Num; ++i)
        {
            const Handle id = handles.Data[i];
            if (id.Index < 0 || id.SerialNumber <= 0 || !native->mass.isEntityValid(manager, id))
            { ResetIndex(generation); return false; }

            if (!EnsureMembershipRecord({id.Index, id.SerialNumber}, generation))
            { ResetIndex(generation); return false; }
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
                { ResetIndex(generation); return false; }
                if (event.Sequence <= token.Watermark) continue;
                if (replaySequence == (std::numeric_limits<uint64_t>::max)() || event.Sequence != replaySequence + 1)
                { ResetIndex(generation); return false; }
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
                    if (!EnsureMembershipRecord({event.Entity.Index, event.Entity.SerialNumber}, generation))
                    { ResetIndex(generation); return false; }
                }
                else
                {
                    const auto result = Index.Remove({event.Entity.Index, event.Entity.SerialNumber}, generation, changes);
                    if (result == B::RemoveResult::AllocationFailure)
                    { ResetIndex(generation); return false; }
                    SourceQueued.erase({event.Entity.Index, event.Entity.SerialNumber});
                }
            }
        }

        // The existing lifecycle API takes the current loading predicate. The
        // accepted admission contract here is its world/experience and observed
        // restore barriers; no private save state or extra readiness probe.
        if (!L::AcceptSeed(token, false) || MiniMapMap::GetWorldGeneration() != generation ||
            MiniMapMap::GetWorld() != world)
        { ResetIndex(generation); return false; }
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
            SourceQueued.erase({event.Entity.Index, event.Entity.SerialNumber});
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
            if (!EnsureMembershipRecord({event.Entity.Index, event.Entity.SerialNumber}, generation)) return false;
        }
        AppliedSequence = event.Sequence;
        Current.AppliedSequence = AppliedSequence;
        return true;
    }

    void ServiceOneSource(const MiniMapNative::NativeApi& native, uint64_t generation, B::MassKey key)
    {
        auto retry = [&]() { EnqueueSource(key); };
        if (!MiniMapBuildingComplexSourceAcquisition::Initialize()) { retry(); return; }
        std::shared_ptr<const B::BuildingRecord> before;
        if (!Index.TryGetRecord(key, generation, before) || !before || !before->Source) return;

        auto* world = MiniMapMap::GetWorld();
        if (!world || MiniMapMap::GetWorldGeneration() != generation ||
            !native.mass.getMassEntitySubsystem) { retry(); return; }
        auto* subsystem = native.mass.getMassEntitySubsystem(world);
        const void* manager = nullptr;
        if (subsystem) std::memcpy(&manager, reinterpret_cast<const uint8_t*>(subsystem) + 0x38, sizeof(manager));
        if (!manager) { retry(); return; }

        B::SourcePayload candidate;
        bool complete = false;
        const Handle entity{key.Index, key.Serial};
        if (!MiniMapBuildingComplexSourceAcquisition::TryAcquire(native, manager, entity,
            generation, candidate, complete)) { retry(); return; }

        auto* currentSubsystem = native.mass.getMassEntitySubsystem(world);
        const void* currentManager = nullptr;
        if (currentSubsystem) std::memcpy(&currentManager,
            reinterpret_cast<const uint8_t*>(currentSubsystem) + 0x38, sizeof(currentManager));
        if (MiniMapMap::GetWorldGeneration() != generation || MiniMapMap::GetWorld() != world ||
            currentManager != manager || !native.mass.isEntityValid(manager, entity) ||
            !native.mass.isEntityBuilt(manager, entity))
        { retry(); return; }
        std::shared_ptr<const B::BuildingRecord> current;
        if (!Index.TryGetRecord(key, generation, current) || current != before) { retry(); return; }

        if (!MiniMapBuildingComplexSourceAcquisition::IsSameSource(*before->Source, candidate))
        {
            if (before->Source->SourceRevision == (std::numeric_limits<uint64_t>::max)())
            { retry(); return; }
            candidate.SourceRevision = before->Source->SourceRevision + 1;
            B::BuildingRecord updated;
            updated.Key = key;
            updated.WorldGeneration = generation;
            updated.Source = std::make_shared<const B::SourcePayload>(std::move(candidate));
            B::ChangeSet changes;
            const auto result = Index.Upsert(std::move(updated), changes);
            if (result == B::UpsertResult::InvalidInput || result == B::UpsertResult::AllocationFailure)
            { retry(); return; }
        }
        if (!complete) retry();
    }

    void ServicePendingSources(const MiniMapNative::NativeApi& native, uint64_t generation)
    {
        // Source values are currently reacquired after membership activity or
        // reseeding; ordinary transform or spline edits are not tracked here.
        const size_t queuedAtStart = SourceQueue.size();
        std::vector<B::MassKey> pass;
        pass.reserve(queuedAtStart);
        for (size_t i = 0; i < queuedAtStart && !SourceQueue.empty(); ++i)
        {
            const B::MassKey key = SourceQueue.front();
            SourceQueue.pop_front();
            if (SourceQueued.erase(key) != 0) pass.push_back(key);
        }
        for (const B::MassKey key : pass)
            ServiceOneSource(native, generation, key);
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
            ResetIndex(generation);
            const auto token = L::BeginSeed(false);
            if (!token.Epoch) return;
            Current.State = MiniMapBuildingComplexWorld::MembershipState::CensusInProgress;
            if (!BuildCensus(generation, token))
            {
                if (MiniMapMap::GetWorldGeneration() == generation) ResetIndex(generation);
                Current.State = MiniMapBuildingComplexWorld::MembershipState::ReseedRequired;
            }
            return;
        }

        const auto* native = MiniMapNative::Get();
        if (!native)
        {
            ResetIndex(generation);
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
                    ResetIndex(generation);
                    Current.State = MiniMapBuildingComplexWorld::MembershipState::ReseedRequired;
                    return;
                }
        }
        if (!SourceQueue.empty()) ServicePendingSources(*native, generation);
        lifecycle = L::Status();
        if (lifecycle.ObservationLost || lifecycle.Epoch != AppliedEpoch ||
            (lifecycle.Sequence != AppliedSequence && lifecycle.Queued == 0))
        {
            ResetIndex(generation);
            Current.State = MiniMapBuildingComplexWorld::MembershipState::ReseedRequired;
        }
    }

    void Service() noexcept
    {
        try { ServiceImpl(); }
        catch (...)
        {
            if (Current.WorldGeneration) ResetIndex(Current.WorldGeneration);
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
        ResetIndex(generation);
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
        ResetIndex(nextGeneration);
        Current = {};
        Current.WorldGeneration = nextGeneration;
        Current.State = nextGeneration ? MembershipState::InitialCensusPending : MembershipState::WorldUnavailable;
        AppliedEpoch = AppliedSequence = 0;
    }

    void Shutdown() noexcept
    {
        EndWorld();
        MiniMapBuildingComplexSourceAcquisition::Shutdown();
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
