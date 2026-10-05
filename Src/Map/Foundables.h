#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include "../plugin.h"
#include "MapTransform.h"

namespace MiniMapFoundables
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