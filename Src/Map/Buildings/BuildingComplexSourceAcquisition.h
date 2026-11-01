#pragma once
#if defined(MODLOADER_CLIENT_BUILD)

#include "BuildingComplexIndex.h"
#include "Native/NativeApi.h"

namespace MiniMapBuildingComplexSourceAcquisition
{
    bool Initialize() noexcept;
    void Shutdown() noexcept;

    // Copies current SDK values for one exact Mass identity. No live pointer is
    // returned or retained. complete means identity/transform, source curve,
    // and profile inputs were copied; pure MiniMap-equivalent geometry
    // preparation is a separate CPU step and does not certify full visual
    // support or complete occlusion.
    bool TryAcquire(const MiniMapNative::NativeApi& native, const void* manager,
        MiniMapNative::MassEntityHandle entity, uint64_t worldGeneration,
        MiniMapBuildingComplexIndex::SourcePayload& output, bool& complete) noexcept;
    // Pure CPU preparation over the copied BuildingComplex source snapshot.
    // It reproduces the current MiniMap spline representation at its fixed
    // finest sampling density and owns unoccluded world-space sections.
    bool PrepareSplineGeometry(MiniMapBuildingComplexIndex::MassKey key,
        MiniMapBuildingComplexIndex::SourcePayload& source) noexcept;
    bool IsSameSource(const MiniMapBuildingComplexIndex::SourcePayload& a,
        const MiniMapBuildingComplexIndex::SourcePayload& b) noexcept;
    // Used only to retain an already-owned ordinary capture across an
    // InteriorOnly policy transition; index/source revision still changes.
    bool IsSameSourceExceptInteriorPolicy(const MiniMapBuildingComplexIndex::SourcePayload& a,
        const MiniMapBuildingComplexIndex::SourcePayload& b) noexcept;
}
#endif
