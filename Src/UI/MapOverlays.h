#pragma once
#include "Services/Viewport.h"

#if defined(MODLOADER_CLIENT_BUILD)

#include "plugin_interface.h"

namespace MiniMapConfig { struct FogOfWarSettings; }

namespace MiniMapMap
{
    struct Transform;
}

namespace MiniMapOverlays
{
    void Render(
        MiniMap::Services::ICanvas& ui,
        const MiniMapMap::Transform& transform, bool showCoordinates, bool coordsInUEUnits, bool showCompass,
        const MiniMapConfig::FogOfWarSettings& fog);
}

#endif
