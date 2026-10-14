#pragma once
#include "plugin.h"

namespace MiniMapConfig
{
    constexpr bool DefaultEnabled = true;
    constexpr bool DefaultSplineRenderingEnabled = false;
    bool Initialize(IPluginSelf* self);
    bool IsEnabled();
    bool IsSplineRenderingEnabled();
}
