#pragma once

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
        IModLoaderImGui* ui,
        const MiniMapMap::Transform& transform, bool showCoordinates, bool showCompass,
        const MiniMapConfig::FogOfWarSettings& fog);
}

#endif