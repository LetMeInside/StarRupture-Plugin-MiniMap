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

    void Render(
        IModLoaderImGui* ui,
        const MiniMapMap::Transform& transform);

    void Shutdown();
}

#endif