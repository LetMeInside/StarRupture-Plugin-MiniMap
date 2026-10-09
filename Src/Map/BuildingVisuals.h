#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "../plugin.h"
#include "MapTransform.h"
#include <string>

namespace MiniMapBuildingVisuals
{
    bool Initialize(IPluginSelf* self);
    void Reset();
    void Shutdown();
    void Render(IModLoaderImGui* ui, const MiniMapMap::Transform& transform);
    // Already formatted by this subsystem; appended by the collector producer.
    std::string GetDebugText();
}
#endif
