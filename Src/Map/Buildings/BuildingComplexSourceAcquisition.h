#pragma once
#if defined(MODLOADER_CLIENT_BUILD)

#include "BuildingComplexIndex.h"
#include "Native/NativeApi.h"

namespace MiniMapBuildingComplexSourceAcquisition
{
    bool Initialize() noexcept;
    void Shutdown() noexcept;

    // Copies current SDK values for one exact Mass identity. No live pointer is
    // returned or retained. complete means available identity/transform, full
    // curve, and spline-profile inputs were copied; it does not certify complete
    // sprite/spline rendering support or world-space support bounds.
    bool TryAcquire(const MiniMapNative::NativeApi& native, const void* manager,
        MiniMapNative::MassEntityHandle entity, uint64_t worldGeneration,
        MiniMapBuildingComplexIndex::SourcePayload& output, bool& complete) noexcept;
    bool IsSameSource(const MiniMapBuildingComplexIndex::SourcePayload& a,
        const MiniMapBuildingComplexIndex::SourcePayload& b) noexcept;
}
#endif
