#pragma once

#if defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
#include "BuildingLayer.h"
#endif

#include <cstdint>

struct IPluginSelf;
namespace Services::Map::Buildings { class BuildingMapService; }

namespace MiniMapUI
{
    bool Initialize(IPluginSelf* self);

    /** Creates this gameplay session's GPU layer using the plugin-owned service. */
    bool BeginBuildingLayerSession(
        ::Services::Map::Buildings::BuildingMapService& service);
    /** Advances the GPU viewport on the existing game-thread scheduling tick. */
    void UpdateBuildingLayerSession();
    /** Detaches the GPU layer and closes its viewport before service session teardown. */
    bool EndBuildingLayerSession();

#if defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
    /** Drains GPU layer lifecycle and callback counters from the current layer. */
    bool TakeBuildingLayerDiagnosticsSnapshot(BuildingLayer::DiagnosticsSnapshot& output) noexcept;
#endif

    /** Stops UI input and reports whether overlay callbacks are quiescent for resource shutdown. */
    bool Shutdown();
    void StopInput();
    bool RefreshKeybinds(); // Existing live-edit callers may ignore a failed absent-action request.

    void Show();

    void Hide();

    bool IsVisible();
    void RefreshVisibility(); // Syncs the widget property from the authoritative visibility state.
    uint32_t SetBlockingMenuActive(uint32_t menuBit, bool active);
    uint32_t ClearBlockingMenuSuppression();
}
