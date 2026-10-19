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
        // Original capture-pixel boundaries represented by the processed image:
        // left, top, right, bottom. Includes ceil-rounded transparent margins.
        std::array<double, 4> SourceRect{};
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
