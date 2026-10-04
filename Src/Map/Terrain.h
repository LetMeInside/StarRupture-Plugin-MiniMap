#pragma once

struct IPluginSelf;
typedef void* PluginTextureHandle;

namespace MiniMapTerrain
{
    bool Initialize(IPluginSelf* self);
    void Shutdown();

    bool RegisterDiagnostics(IPluginSelf* self);
    void UnregisterDiagnostics();

    void CancelPendingDiagnostic();

    PluginTextureHandle GetTerrainTexture();
}