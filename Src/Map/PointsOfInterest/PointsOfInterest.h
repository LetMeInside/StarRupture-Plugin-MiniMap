#pragma once
#include "Services/Viewport.h"

#if defined(MODLOADER_CLIENT_BUILD)

#include "Map/MarkerIconSizing.h"

namespace MiniMapMap
{
    struct Transform;
}

struct IPluginSelf;

namespace MiniMapConfig
{
    struct POIRenderSettings;
    struct FogOfWarSettings;
}

namespace MiniMapPointsOfInterest
{
    bool Initialize(IPluginSelf* self);
    void Reset();

    void OnExperienceLoadComplete();

    void Render(
        MiniMap::Services::ICanvas& ui,
        const MiniMapMap::Transform& transform,
        const MiniMapConfig::POIRenderSettings& settings,
        const MiniMapConfig::FogOfWarSettings& fog);

    void Shutdown();
}

#endif
