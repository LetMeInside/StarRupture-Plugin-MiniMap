#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include "plugin_interface.h"

namespace MiniMapMap
{
    struct Transform;
}

struct IPluginSelf;

namespace MiniMapFogOfWar
{
    bool Initialize(
        IPluginSelf* self);

    void Reset();

    bool IsNativeWorldPositionRevealed(
        double worldX,
        double worldY);
    // Conservative entire AABB, bounded copied mask read; never native scanning.
    bool IsNativeWorldRectangleRevealed(double minX,double minY,double maxX,double maxY);

    // Effective visual exposure, including the temporary local feather.
    // Reads the copied FOW snapshot; does not change persistent exploration.
    float GetRenderedVisibilityAtWorldPosition(
        double worldX,
        double worldY,
        const MiniMapMap::Transform& transform);

    void Render(
        IModLoaderImGui* ui,
        const MiniMapMap::Transform& transform);

    void Shutdown();
}

#endif
