#pragma once

#include <windows.h>
#include "plugin_interface.h"

namespace MiniMap::Adapters::AlienX
{
    // Borrowed access for subsystems awaiting their own migration stage.
    // The bootstrap is the sole owner of the retained loader identity.
    IPluginSelf* GetSelf() noexcept;
    void CapturePluginInitHookBaseline(IPluginSelf* self) noexcept;
    void LogTerrainHookAvailability(IPluginSelf* self) noexcept;

    // Existing scan chain remains in plugin.cpp until the scanner stage.
    void ResolveNativeDependencies(IPluginSelf* self, IPluginHookScanner* scanner);
}
