#pragma once
#include "Services/Viewport.h"
#include "DiagnosticExports.h"

#include "Map/MapTransform.h"

struct IPluginSelf;

namespace MiniMapConfig
{
    struct RenderSettings;
}

namespace MiniMapTerrain
{
    bool Initialize(IPluginSelf* self);
    void Shutdown();
    void InvalidatePresentation(); // Values only; safe for visibility callbacks.
    /** Reads the current valid terrain view and returns its rotated world-space AABB in native centimetres. */
    bool TryGetVisibleWorldBounds(double& x1, double& y1, double& x2, double& y2);

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
    bool RegisterDiagnostics(IPluginSelf* self);
    void UnregisterDiagnostics();

    void CancelPendingDiagnostic();
#endif

    void AdjustZoom(float wheelDelta);

    bool Render(
        MiniMap::Services::ICanvas& ui,
        float windowX,
        float windowY,
        float windowWidth,
        float windowHeight,
        MiniMapMap::Transform& outTransform,
        const MiniMapConfig::RenderSettings& settings);
}
