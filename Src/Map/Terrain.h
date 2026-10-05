#pragma once

#include "plugin_interface.h"

#include "MapTransform.h"

struct IPluginSelf;

namespace MiniMapTerrain
{
    bool Initialize(IPluginSelf* self);
    void Shutdown();

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
