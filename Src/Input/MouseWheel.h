#pragma once

struct IPluginSelf;

namespace MiniMapMouseWheel
{
    bool Initialize(IPluginSelf* self);
    void Shutdown();

    // Enables or disables LeftCtrl+wheel zoom for effective gameplay visibility.
    // Disabling also discards any pending wheel input.
    void SetZoomEnabled(bool enabled);

    // Returns accumulated wheel movement in standard wheel-notch units.
    // Positive values mean wheel up; negative values mean wheel down.
    float DrainZoomDelta();

    void ClearPendingZoom();
}
