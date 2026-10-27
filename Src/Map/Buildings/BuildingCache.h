#pragma once
#include "DiagnosticExports.h"

#if defined(MODLOADER_CLIENT_BUILD)

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace MiniMapBuildingCache
{
    // Captured assembly bounds in the same world-space units used by the capture.
    struct SpatialBounds
    {
        std::array<double, 3> Min{};
        std::array<double, 3> Max{};
    };

    // Pixel boundaries in the original 256-square capture. Output pixel (u,v)
    // maps to (X + u*Step, Y + v*Step); ceil-rounded margins remain transparent.
    struct ImageMapping
    {
        uint16_t X = 0, Y = 0;
        double Step = 0;
    };
    inline bool ValidSpatial(uint32_t width, uint32_t height, const SpatialBounds& bounds,
        const ImageMapping& mapping) noexcept
    {
        if (!width || !height || width > 256 || height > 256 || mapping.X >= 256 || mapping.Y >= 256 ||
            !std::isfinite(mapping.Step) || mapping.Step < 1) return false;
        for (std::size_t i = 0; i < 3; ++i)
            if (!std::isfinite(bounds.Min[i]) || !std::isfinite(bounds.Max[i]) || bounds.Max[i] < bounds.Min[i]) return false;
        if (!(bounds.Max[0] > bounds.Min[0] && bounds.Max[1] > bounds.Min[1])) return false;
        const double ex = bounds.Max[0] * 0.5 - bounds.Min[0] * 0.5;
        const double ey = bounds.Max[1] * 0.5 - bounds.Min[1] * 0.5;
        const double orthoWidth = double(float(2 * (ex > ey ? ex : ey) * 1.1));
        if (!std::isfinite(orthoWidth) || !(orthoWidth > 0) ||
            !std::isfinite((orthoWidth / 256) * mapping.Step)) return false;
        // The last sample may extend beyond the source canvas by less than one
        // output pixel. Its start must still be in the canvas; do not clip its
        // spatial footprint and thereby stretch the last filtered texel.
        return mapping.X + (width - 1) * mapping.Step < 256 && mapping.Y + (height - 1) * mapping.Step < 256 &&
            std::isfinite(mapping.X + width * mapping.Step) && std::isfinite(mapping.Y + height * mapping.Step);
    }

    struct Sprite
    {
        uint32_t Width = 0;
        uint32_t Height = 0;
        std::vector<uint8_t> Rgba;
        SpatialBounds Bounds{};
        ImageMapping Mapping{};
    };

    // Encodes the UTF-8 bytes reversibly. The returned name includes .sprite.
    bool BlueprintFileName(const std::string& canonicalBlueprintPath, std::string& fileName);
    bool GetCacheRoot(std::filesystem::path& directory);
    bool Clear();
    enum class LoadStatus { Loaded, Missing, Invalid, IOError, Unavailable };
    struct LoadResult
    {
        LoadStatus Status;
        const char* Detail; // Static diagnostic text; Error is Win32/HRESULT as appropriate.
        uint32_t Error = 0;
    };
    // One open/read/decode; failures leave out unchanged. No existence precheck.
    LoadResult TryLoad(const std::string& canonicalBlueprintPath, Sprite& out) noexcept;
    bool Write(const std::string& canonicalBlueprintPath, const uint8_t* rgba,
        uint32_t width, uint32_t height, const SpatialBounds& bounds, const ImageMapping& mapping);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
    // Full source canvas beside its .sprite; optional export cannot throw into
    // production publication. Included only in development diagnostics.
    bool WriteDiagnosticPng(const std::string& canonicalBlueprintPath,
        const uint8_t* rgba, uint32_t width, uint32_t height) noexcept;
    // Full published atlas only. Sequential, exclusive final-name publication;
    // logs its result and never changes GPU publication. Call outside lifecycle lock.
    bool WriteAtlasDiagnosticPng(const std::vector<uint8_t>& rgba,
        uint32_t width, uint32_t height, uint64_t revision) noexcept;
#endif
}

#endif
