#pragma once

struct IPluginSelf;

namespace MiniMapMouseWheel
{
    bool Initialize(IPluginSelf* self);
    void Shutdown();

    // Called from the MiniMap widget render callback after AlienX's
    // render/UI backend is known to be active.
    void SignalUiReady();

    // Enables or disables Ctrl+wheel zoom acceptance.
    // Disabling also discards any pending wheel input.
    void SetZoomEnabled(bool enabled);

    // Returns accumulated wheel movement in standard wheel-notch units.
    // Positive values mean wheel up; negative values mean wheel down.
    float DrainZoomDelta();

    void ClearPendingZoom();
}
