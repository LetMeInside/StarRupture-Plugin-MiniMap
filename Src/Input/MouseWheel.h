#pragma once
#include <cstdint>

struct IPluginSelf;

namespace MiniMapMouseWheel
{
    bool Initialize(IPluginSelf* self);
    void Shutdown();

    // Versioned config application, separate from current visibility/readiness.
    void Configure(bool enabled, uint32_t modifier, uint64_t revision);
    // Discard input across visibility/readiness transitions. Admission reads
    // current atomics, never a caller's stale visibility snapshot.
    void RefreshVisibility();

    // Returns accumulated wheel movement in standard wheel-notch units.
    // Positive values mean wheel up; negative values mean wheel down.
    float DrainZoomDelta();

    void ClearPendingZoom();
}
