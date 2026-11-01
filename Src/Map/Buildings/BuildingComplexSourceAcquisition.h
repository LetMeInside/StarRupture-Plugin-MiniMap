#pragma once
#if defined(MODLOADER_CLIENT_BUILD)

#include "BuildingComplexIndex.h"
#include "Native/NativeApi.h"

namespace MiniMapBuildingComplexSourceAcquisition
{
    bool Initialize() noexcept;
    void Shutdown() noexcept;

    // Copies current SDK values for one exact Mass identity. No live pointer is
    // returned or retained. complete means all available identity/transform/
    // spline source fields were read; it does not mean render support is known.
    bool TryAcquire(const MiniMapNative::NativeApi& native, const void* manager,
        MiniMapNative::MassEntityHandle entity, uint64_t worldGeneration,
        MiniMapBuildingComplexIndex::SourcePayload& output, bool& complete) noexcept;
    bool IsSameSource(const MiniMapBuildingComplexIndex::SourcePayload& a,
        const MiniMapBuildingComplexIndex::SourcePayload& b) noexcept;
}
#endif
