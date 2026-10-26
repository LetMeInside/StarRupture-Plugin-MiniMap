#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include "plugin.h"
#include "Map/MapTransform.h"

namespace MiniMapConfig
{
    struct FoundableRenderSettings;
    struct FogOfWarSettings;
}

namespace MiniMapFoundables
{
    bool Initialize(IPluginSelf* self);
    void Reset();
    void OnExperienceLoadComplete();

    void Render(
        IModLoaderImGui* ui,
        const MiniMapMap::Transform& transform,
        const MiniMapConfig::FoundableRenderSettings& settings,
        const MiniMapConfig::FogOfWarSettings& fog);

    void Shutdown();
}

#endif
