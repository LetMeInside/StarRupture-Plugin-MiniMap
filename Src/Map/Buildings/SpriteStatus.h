#pragma once

#include <array>
#include <cstdint>
#include <cmath>
#include <string>
#include <unordered_map>
#include "Services/Textures.h"

namespace MiniMapSpriteStatus
{
    struct AtlasUV { float U{}, V{}; };
    // Copied publication metadata. Only FromRegion can create a valid value;
    // backend callers cannot bypass UV validation with arbitrary coordinates.
    class PreparedAtlasUV
    {
    public:
        enum class State : uint8_t { Unprepared, Valid, InvalidRegion, InvalidRectangle };
        State Status() const noexcept { return Validation; }
        const std::array<AtlasUV, 4>& Corners() const noexcept { return Values; }
        static PreparedAtlasUV FromRegion(const std::array<double, 4>& r) noexcept
        {
            PreparedAtlasUV result;
            for (double value : r) if (!std::isfinite(value) || value < 0 || value > 1)
            { result.Validation = State::InvalidRegion; return result; }
            if (!(r[2] > r[0] && r[3] > r[1]))
            { result.Validation = State::InvalidRectangle; return result; }
            result.Values = {{{float(r[0]), float(r[1])}, {float(r[2]), float(r[1])},
                {float(r[2]), float(r[3])}, {float(r[0]), float(r[3])}}};
            result.Validation = State::Valid;
            return result;
        }
    private:
        std::array<AtlasUV, 4> Values{};
        State Validation = State::Unprepared;
    };

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
        // Non-owning neutral identity for the published atlas acquisition.
        MiniMap::Services::Texture Texture{};

        // Normalized region within the texture.
        std::array<double, 4> Region{0.0, 0.0, 1.0, 1.0};

        // Original capture-space bounds.
        std::array<double, 3> Min{};
        std::array<double, 3> Max{};
        // Original capture-pixel boundaries represented by the processed image:
        // left, top, right, bottom. Includes ceil-rounded transparent margins.
        std::array<double, 4> SourceRect{};
        // Prepared with Region before publication; replaced/copied with this entry.
        PreparedAtlasUV UVs;
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
