#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include "../plugin.h"
#include "MapTransform.h"

namespace MiniMapAlienActors
{
    bool Initialize(IPluginSelf* self);
    void Reset();
    void OnExperienceLoadComplete();

    // Copies viewport geometry only; acquisition remains on the engine tick.
    void SetViewport(const MiniMapMap::Transform& transform);
    void Render(IModLoaderImGui* ui, const MiniMapMap::Transform& transform);
    void Shutdown();
}

#endif
