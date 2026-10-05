#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include "plugin_interface.h"

namespace MiniMapMap
{
    struct Transform;
}

namespace MiniMapOverlays
{
    void Render(
        IModLoaderImGui* ui,
        const MiniMapMap::Transform& transform);
}

#endif