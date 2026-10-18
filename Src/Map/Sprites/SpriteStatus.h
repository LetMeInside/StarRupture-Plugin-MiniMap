#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace MiniMapSpriteStatus
{
    enum class Location
    {
        Queued,
        Failed,
        FileCache,
        Atlas,
        PendingCache
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
        // Half-open source-canvas crop: X, Y, Width, Height (original 256x256).
        std::array<uint16_t, 4> Crop{0, 0, 256, 256};
    };

    struct Status
    {
        explicit Status(Location location)
            : Where(location)
        {
        }

        Location Where;
        AtlasEntry Atlas{};
    };

    using Dictionary = std::unordered_map<std::string, Status>;
}
