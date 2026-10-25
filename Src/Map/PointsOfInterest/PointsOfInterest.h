#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include "Map/MarkerIconSizing.h"
#include "plugin_interface.h"

namespace MiniMapMap
{
    struct Transform;
}

struct IPluginSelf;

namespace MiniMapPointsOfInterest
{
    bool Initialize(IPluginSelf* self);
    void Reset();

    void OnExperienceLoadComplete();

    void Render(
        IModLoaderImGui* ui,
        const MiniMapMap::Transform& transform);

    void Shutdown();
}

#endif
