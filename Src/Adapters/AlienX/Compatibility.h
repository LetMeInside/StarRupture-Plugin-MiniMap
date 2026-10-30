#pragma once

#include <windows.h>
#include "plugin_interface.h"

namespace MiniMap::Services { class IRequiredScanner; }

namespace MiniMap::Adapters::AlienX
{
    // Borrowed access for subsystems awaiting their own migration stage.
    // The bootstrap is the sole owner of the retained loader identity.
    IPluginSelf* GetSelf() noexcept;
    bool KeyInputAvailable() noexcept;
    bool WheelInputAvailable() noexcept;
    void CapturePluginInitHookBaseline(IPluginSelf* self) noexcept;
    void LogTerrainHookAvailability(IPluginSelf* self) noexcept;

    // Transitional orchestration in plugin.cpp; the scan session stays borrowed.
    void ResolveNativeDependencies(Services::IRequiredScanner& scanner);
}
