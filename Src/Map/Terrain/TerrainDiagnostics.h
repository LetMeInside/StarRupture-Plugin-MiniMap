#pragma once
#include "DiagnosticExports.h"

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS && defined(MODLOADER_CLIENT_BUILD)

#include "Services/Textures.h"

struct IPluginSelf;

namespace MiniMapTerrainDiagnostics
{
    bool Initialize(IPluginSelf* self);
    void Shutdown();

    bool RegisterDiagnostics(IPluginSelf* self);
    void UnregisterDiagnostics();

    void CancelPendingDiagnostic();

    MiniMap::Services::Texture GetTerrainTexture();
}

#endif
