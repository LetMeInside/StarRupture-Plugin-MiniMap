#pragma once
#if defined(MODLOADER_CLIENT_BUILD)

#include "BuildingComplexIndex.h"
#include <cstdint>

namespace MiniMapBuildingComplexWorld
{
    enum class MembershipState : uint8_t
    {
        WorldUnavailable,
        InitialCensusPending,
        CensusInProgress,
        EventReplayPending,
        MembershipEstablished,
        ReseedRequired
    };

    struct Status
    {
        // MembershipEstablished certifies only entity membership and journal
        // handoff; unresolved Unknown-support records still block cell completeness.
        MembershipState State = MembershipState::WorldUnavailable;
        uint64_t WorldGeneration = 0;
        uint64_t RestoreEpoch = 0;
        uint64_t AppliedSequence = 0;
    };

    // Called from the existing admitted world lifecycle. All service work is
    // subsequently driven by MassLifecycle::Service on its game-thread scope.
    void BeginWorld(uint64_t worldGeneration) noexcept;
    void EndWorld(uint64_t nextWorldGeneration = 0) noexcept;
    void Shutdown() noexcept;
    // Game-thread lifecycle/status APIs. Consumers must gate index reads on
    // GetStatus().State == MembershipEstablished and inspect cell snapshots
    // separately for unresolved support.
    Status GetStatus() noexcept;
    MiniMapBuildingComplexIndex::Index& GetIndex() noexcept;
}
#endif
