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
        // Distinguishes replacement of the captured sprite under one source revision.
        uint64_t ImageRevision = 0;
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

    // Compact immutable publication token copied from CellImage metadata.
    // It retains the exact capture manifest but no half-float pixel payload.
    struct CellImageIdentity
    {
        B::CellKey Key;
        uint64_t WorldGeneration = 0;
        uint64_t RestoreEpoch = 0;
        uint64_t SourceRevision = 0;
        uint64_t CellRevision = 0;
        uint64_t DependencyRevision = 0;
        bool RenderDroneRails = true;
        State Status = State::Failed;
        std::vector<CaptureRevision> CaptureRevisions;
    };

    inline CellImageIdentity Identify(const CellImage& image)
    {
        return {image.Key, image.WorldGeneration, image.RestoreEpoch, image.SourceRevision,
            image.CellRevision, image.DependencyRevision, image.RenderDroneRails, image.Status,
            image.CaptureRevisions};
    }

    struct SpriteLease
    {
        B::MassKey Building;
        uint64_t SourceRevision = 0;
        uint64_t ImageRevision = 0;
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
