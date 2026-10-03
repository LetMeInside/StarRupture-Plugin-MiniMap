#pragma once

struct IPluginSelf;
typedef void* PluginTextureHandle;

namespace SDK
{
    class UWorld;
}

namespace MiniMapTerrain
{
    bool Initialize(IPluginSelf* self);
    void Shutdown();

    void SetWorld(SDK::UWorld* world);

    bool RegisterDiagnostics(IPluginSelf* self);
    void UnregisterDiagnostics();

    PluginTextureHandle GetTerrainTexture();
}