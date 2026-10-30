#pragma once
#include "Services/Viewport.h"

#if defined(MODLOADER_CLIENT_BUILD)

#include "Map/MapTransform.h"

struct IPluginSelf;

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
        MiniMap::Services::ICanvas& ui,
        const MiniMapMap::Transform& transform,
        const MiniMapConfig::FoundableRenderSettings& settings,
        const MiniMapConfig::FogOfWarSettings& fog);

    void Shutdown();
}

#endif
