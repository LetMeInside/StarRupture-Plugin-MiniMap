#pragma once
#include "Services/Viewport.h"

#include <cstdint>

#if defined(MODLOADER_CLIENT_BUILD)

namespace MiniMapMap
{
    struct Transform;
}

namespace MiniMapConfig { struct FogOfWarSettings; }

struct IPluginSelf;

namespace MiniMapFogOfWar
{
    /** Publishes the validated FOWData offset for snapshot reads. */
    void SetFowDataOffset(std::int32_t offset) noexcept;

    bool Initialize(
        IPluginSelf* self);

    void Reset();

    bool IsNativeWorldPositionRevealed(
        double worldX,
        double worldY);
    // Conservative entire AABB, bounded copied mask read; never native scanning.
    bool IsNativeWorldRectangleRevealed(double minX,double minY,double maxX,double maxY);

    // Bounded sufficient opacity proof. Unknown data or budget exhaustion returns false.
    bool IsWorldRectangleFullyObscured(double minX,double minY,double maxX,double maxY,
        const MiniMapMap::Transform& transform,
        const MiniMapConfig::FogOfWarSettings& fog);

    // Effective visual exposure, including the temporary local feather.
    // Disabled fog bypasses the copied snapshot after input validation.
    // Enabled fog reads it; neither policy changes persistent exploration.
    float GetRenderedVisibilityAtWorldPosition(
        double worldX,
        double worldY,
        const MiniMapMap::Transform& transform,
        const MiniMapConfig::FogOfWarSettings& fog);

    void Render(
        MiniMap::Services::ICanvas& ui,
        const MiniMapMap::Transform& transform,
        const MiniMapConfig::FogOfWarSettings& fog);

    void Shutdown();
}

#endif
