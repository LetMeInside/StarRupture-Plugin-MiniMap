#pragma once

#if defined(MODLOADER_CLIENT_BUILD)
#include "../plugin.h"
#include <array>
#include <cstdint>

// Development-only independent worldwide reference. No retained inventory or rendering.
namespace MiniMapBuildingInventory
{
    struct Record
    {
        int32_t Index = 0;
        int32_t SerialNumber = 0;
        bool HasTransform = false;
        bool HasSpline = false;
        std::array<double, 3> Position = {};
        std::array<double, 4> Rotation = {}; // Quaternion X/Y/Z/W.
        std::array<double, 3> Scale = {};
    };
    bool Initialize(IPluginSelf* self);
    void Reset();
    void OnExperienceLoadComplete();
    void Shutdown();
}
#endif
