#pragma once

#include "plugin_interface.h"

#include "Map/MapTransform.h"

struct IPluginSelf;

namespace MiniMapTerrain
{
    bool Initialize(IPluginSelf* self);
    void Shutdown();
    void InvalidatePresentation(); // Values only; safe for visibility callbacks.

    bool RegisterDiagnostics(IPluginSelf* self);
    void UnregisterDiagnostics();

    void CancelPendingDiagnostic();

    void AdjustZoom(float wheelDelta);

    bool Render(
        IModLoaderImGui* ui,
        float windowX,
        float windowY,
        float windowWidth,
        float windowHeight,
        MiniMapMap::Transform& outTransform);
}
