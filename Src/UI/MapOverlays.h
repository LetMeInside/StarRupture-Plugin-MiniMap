#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include "plugin_interface.h"

namespace MiniMapMap
{
    struct Transform;
}

namespace MiniMapOverlays
{
    extern bool g_showPlayerCoordinates;

    void Render(
        IModLoaderImGui* ui,
        const MiniMapMap::Transform& transform);
}

#endif