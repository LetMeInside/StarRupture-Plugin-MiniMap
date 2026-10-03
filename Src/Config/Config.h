#pragma once
#include "plugin.h"

namespace MiniMapConfig
{
    constexpr bool DefaultEnabled = true;
    bool Initialize(IPluginSelf* self);
    bool IsEnabled();
}