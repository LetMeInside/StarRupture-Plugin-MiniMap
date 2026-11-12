#pragma once

#if defined(MODLOADER_CLIENT_BUILD) && !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
#include "BuildingLayer.h"
#endif

#include <cstdint>

struct IPluginSelf;
namespace Services::Map::Buildings { class BuildingMapService; }
namespace MiniMap::Services { class ITextures; }

namespace MiniMapUI
{
    bool Initialize(IPluginSelf* self);

#if !MINIMAP_USE_LEGACY_BUILDINGS
    /** Creates this gameplay session's layer using the plugin-owned service and texture backend. */
    bool BeginBuildingLayerSession(
        ::Services::Map::Buildings::BuildingMapService& service,
        MiniMap::Services::ITextures& textures);
    /** Advances MiniMap's viewport and its CPU/GPU presentation on the existing game-thread tick. */
    void UpdateBuildingLayerSession();
    /** Detaches and shuts down this session's layer, retaining it when texture release must retry. */
    bool EndBuildingLayerSession();
#endif

#if defined(MODLOADER_CLIENT_BUILD) && !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
    /** Drains a completed game-thread presentation interval from the current layer. */
    bool TakeBuildingLayerPresentationDiagnosticsSnapshot(
        BuildingLayer::DiagnosticsSnapshot& output) noexcept;
    /** Drains a completed UI render interval from the current layer. */
    bool TakeBuildingLayerRenderDiagnosticsSnapshot(
        BuildingLayer::RenderDiagnosticsSnapshot& output) noexcept;
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
