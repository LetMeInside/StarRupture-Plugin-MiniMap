#pragma once
#if defined(MODLOADER_CLIENT_BUILD)

#include "BuildingComplexIndex.h"
#include "BuildingComplexCellRasterizer.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace MiniMapBuildingCache { struct Sprite; }

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
    // Returns the immutable CPU capture associated with this exact world/source
    // identity. Image lifetime is owned separately from sparse index records.
    bool TryGetCaptureImage(MiniMapBuildingComplexIndex::MassKey key,
        uint64_t worldGeneration, uint64_t sourceRevision,
        std::shared_ptr<const MiniMapBuildingCache::Sprite>& output,
        uint64_t* imageRevision = nullptr) noexcept;

    enum class CellCompositionResult : uint8_t { Complete, Partial, Failed, Stale, Pending };
    // Game-thread-only synchronous CPU request over immutable index/capture
    // snapshots. Reuses a current shared immutable RGBA16F result before image
    // acquisition/rasterization. Identical reentry returns Pending with no image;
    // retry after the owning call returns (no wait or asynchronous job is added).
    // Off-thread calls fail. Consumers revalidate before using returned images.
    CellCompositionResult ComposeCell(MiniMapBuildingComplexIndex::CellKey key,
        uint64_t worldGeneration,
        std::shared_ptr<const MiniMapBuildingComplexCellRasterizer::CellImage>& output) noexcept;
    // Uses the reusable CPU path and queues only Complete images
    // for the independent native publisher. RequestId is not GPU readiness.
    CellCompositionResult RequestCellPublication(MiniMapBuildingComplexIndex::CellKey key,
        uint64_t worldGeneration, uint64_t& requestId) noexcept;
    bool ValidateCellImage(const MiniMapBuildingComplexCellRasterizer::CellImage& image) noexcept;
    bool ValidateCellImage(const MiniMapBuildingComplexCellRasterizer::CellImageIdentity& identity) noexcept;

    struct TileKey
    {
        int64_t X = 0, Y = 0;
        bool operator==(const TileKey&) const = default;
        bool operator<(const TileKey& rhs) const noexcept
        { return X < rhs.X || (X == rhs.X && Y < rhs.Y); }
    };
    enum class TileState : uint8_t { Pending, Partial, Complete, Empty, Failed, Stale };
    struct TileIdentity
    {
        uint64_t WorldGeneration = 0, RestoreEpoch = 0;
        MiniMapBuildingComplexIndex::WorldOrigin Origin;
        TileKey Key;
        uint32_t PolicyId = 1, ContentSide = 0;
        bool RenderDroneRails = true;
        TileState State = TileState::Failed;
        std::vector<MiniMapBuildingComplexCellRasterizer::CellImageIdentity> Inputs;
    };
    struct GroupedTileImage
    {
        TileIdentity Identity;
        uint32_t Width = 0, Height = 0; // (N+2) including one texel of guard.
        std::vector<uint8_t> Rgba16F; // Empty results carry no pixel allocation.
    };
    struct TileResult
    {
        TileState State = TileState::Failed;
        std::shared_ptr<const GroupedTileImage> Image;
    };

    int64_t FloorDiv4(int64_t value) noexcept;
    TileKey TileForCell(MiniMapBuildingComplexIndex::CellKey key) noexcept;
    uint32_t SelectTileResolution(double projectedTilePixels, uint32_t currentSide = 0) noexcept;
    TileResult ComposeTile(TileKey key, uint64_t worldGeneration, uint32_t contentSide) noexcept;
    bool ValidateTile(const TileIdentity& identity) noexcept;
}
#endif
