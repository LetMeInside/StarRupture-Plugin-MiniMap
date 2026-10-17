#pragma once

#include <array>
#include <string>
#include <unordered_map>

namespace MiniMapSpriteStatus
{
    enum class Location
    {
        Queued,
        Failed,
        FileCache,
        Atlas
    };

    struct AtlasEntry
    {
        // Non-owning GPU texture handle.
        void* Texture = nullptr;

        // Normalized region within the texture.
        std::array<double, 4> Region{0.0, 0.0, 1.0, 1.0};

        // Original capture-space bounds.
        std::array<double, 3> Min{};
        std::array<double, 3> Max{};
    };

    struct Status
    {
        Location Where = Location::Queued;
        AtlasEntry Atlas{};
    };

    using Dictionary = std::unordered_map<std::string, Status>;
}
