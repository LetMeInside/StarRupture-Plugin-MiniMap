#pragma once
#include "DiagnosticExports.h"

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS && defined(MODLOADER_CLIENT_BUILD)

struct IPluginSelf;
typedef void* PluginTextureHandle;

namespace MiniMapTerrainDiagnostics
{
    bool Initialize(IPluginSelf* self);
    void Shutdown();

    bool RegisterDiagnostics(IPluginSelf* self);
    void UnregisterDiagnostics();

    void CancelPendingDiagnostic();

    PluginTextureHandle GetTerrainTexture();
}

#endif
