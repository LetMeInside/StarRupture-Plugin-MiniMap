#if defined(MODLOADER_CLIENT_BUILD)

#include "Terrain.h"
#include "TerrainDiagnostics.h"

namespace MiniMapTerrain
{
    bool Initialize(IPluginSelf* self)
    {
        return MiniMapTerrainDiagnostics::Initialize(
            self);
    }


    void Shutdown()
    {
        MiniMapTerrainDiagnostics::Shutdown();
    }


    bool RegisterDiagnostics(IPluginSelf* self)
    {
        return MiniMapTerrainDiagnostics::RegisterDiagnostics(
            self);
    }


    void UnregisterDiagnostics()
    {
        MiniMapTerrainDiagnostics::UnregisterDiagnostics();
    }


    void CancelPendingDiagnostic()
    {
        MiniMapTerrainDiagnostics::CancelPendingDiagnostic();
    }


    PluginTextureHandle GetTerrainTexture()
    {
        return MiniMapTerrainDiagnostics::GetTerrainTexture();
    }
}

#endif
