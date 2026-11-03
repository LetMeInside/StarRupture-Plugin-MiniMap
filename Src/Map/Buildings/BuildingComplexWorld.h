#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include <cstdint>
#include <vector>

namespace SDK { class UWorld; }

namespace MiniMapBuildingComplexWorld
{
    constexpr uint32_t kCellImageSizePixels = 256;
    struct CellKey
    {
        int64_t X = 0, Y = 0;
        bool operator==(const CellKey&) const = default;
        bool operator<(const CellKey& rhs) const noexcept
        { return X < rhs.X || (X == rhs.X && Y < rhs.Y); }
    };
    struct WorldOrigin { double X = 0, Y = 0; bool Valid = false; };
    struct CellValidity
    {
        uint64_t SafetyGeneration = 0, LocalEpoch = 0;
        bool operator==(const CellValidity&) const = default;
    };
    namespace Cell
    {
        enum class State : uint8_t { Complete, Partial, Failed };
        struct CaptureRevision
        {
            uint64_t SourceRevision = 0, ImageRevision = 0;
            bool operator==(const CaptureRevision&) const = default;
        };
        struct CellImage
        {
            CellKey Key;
            uint64_t WorldGeneration = 0, SourceRevision = 0, CellRevision = 0, DependencyRevision = 0;
            CellValidity Validity;
            WorldOrigin Origin;
            bool RenderDroneRails = true;
            uint32_t Width = kCellImageSizePixels, Height = kCellImageSizePixels;
            State Status = State::Failed;
            std::vector<CaptureRevision> CaptureRevisions, CaptureAvailability;
            std::vector<uint8_t> Rgba16F;
        };
        struct CellImageIdentity
        {
            CellKey Key;
            uint64_t WorldGeneration = 0, SourceRevision = 0, CellRevision = 0, DependencyRevision = 0;
            CellValidity Validity;
            WorldOrigin Origin;
            bool RenderDroneRails = true;
            State Status = State::Failed;
            std::vector<CaptureRevision> CaptureRevisions, CaptureAvailability;
        };
        inline CellImageIdentity Identify(const CellImage& image)
        {
            return {image.Key, image.WorldGeneration, image.SourceRevision, image.CellRevision,
                image.DependencyRevision, image.Validity, image.Origin, image.RenderDroneRails, image.Status,
                image.CaptureRevisions, image.CaptureAvailability};
        }
    }

    void Shutdown() noexcept;
    bool OpenCollectorSession(uint64_t viewportSession, SDK::UWorld* world,
        double originX, double originY) noexcept;
    bool OwnsCollectorSession(uint64_t viewportSession, SDK::UWorld* world) noexcept;
    void CloseCollectorSession(uint64_t viewportSession) noexcept;

    enum class TileState : uint8_t { Pending, Partial, Complete, Empty, Failed, Stale };
    struct TileKey
    {
        int64_t X = 0, Y = 0;
        bool operator==(const TileKey&) const = default;
        bool operator<(const TileKey& rhs) const noexcept
        { return X < rhs.X || (X == rhs.X && Y < rhs.Y); }
    };
    struct TileIdentity
    {
        uint64_t WorldGeneration = 0;
        WorldOrigin Origin;
        TileKey Key;
        uint32_t PolicyId = 1, ContentSide = 0;
        bool RenderDroneRails = true;
        TileState State = TileState::Failed;
        std::vector<Cell::CellImageIdentity> Inputs;
    };
    struct GroupedTileImage
    {
        TileIdentity Identity;
        uint32_t Width = 0, Height = 0;
        std::vector<uint8_t> Rgba16F;
    };
    bool ValidateCellImage(const Cell::CellImage&) noexcept;
    bool ValidateCellImage(const Cell::CellImageIdentity&) noexcept;
    bool ValidateTile(const TileIdentity&) noexcept;
    bool ValidateGroupedTileImage(const GroupedTileImage&) noexcept;
}
#endif

