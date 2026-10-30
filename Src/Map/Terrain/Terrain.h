#pragma once
#include "Services/Viewport.h"
#include "DiagnosticExports.h"

#include "plugin_interface.h"

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
