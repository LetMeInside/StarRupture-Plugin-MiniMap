#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include "BuildingComplexIndex.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace MiniMapBuildingCache { struct Sprite; }

namespace MiniMapBuildingComplexCellRasterizer
{
    namespace B = MiniMapBuildingComplexIndex;

    enum class State : uint8_t { Complete, Partial, Failed };

    struct CaptureRevision
    {
        B::MassKey Building;
        uint64_t SourceRevision = 0;
        bool operator==(const CaptureRevision&) const = default;
    };

    // The canonical cell representation is little-endian RGBA16F, linear
    // associated RGB plus transmittance T, row-major from minimum world Y.
    struct CellImage
    {
        B::CellKey Key;
        uint64_t WorldGeneration = 0;
        uint64_t RestoreEpoch = 0;
        uint64_t SourceRevision = 0;
        uint64_t CellRevision = 0;
        uint64_t DependencyRevision = 0;
        bool RenderDroneRails = true;
        uint32_t Width = B::kCellImageSizePixels;
        uint32_t Height = B::kCellImageSizePixels;
        State Status = State::Failed;
        std::vector<CaptureRevision> CaptureRevisions;
        std::vector<uint8_t> Rgba16F;
    };

    struct SpriteLease
    {
        B::MassKey Building;
        uint64_t SourceRevision = 0;
        std::shared_ptr<const MiniMapBuildingCache::Sprite> Image;
    };

    // Pure CPU composition over immutable index/capture snapshots. Safe to run
    // on an owned worker handoff. Partial output retains known contributors
    // but never certifies missing inputs.
    State Compose(const B::CellSnapshot& snapshot,
        const std::vector<SpriteLease>& sprites, bool renderDroneRails,
        CellImage& output) noexcept;
}

#endif
