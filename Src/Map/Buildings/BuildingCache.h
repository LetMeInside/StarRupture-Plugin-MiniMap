#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include <array>
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

    struct Sprite
    {
        uint32_t Width = 0;
        uint32_t Height = 0;
        std::vector<uint8_t> Rgba;
        SpatialBounds Bounds{};
    };

    // Encodes the UTF-8 bytes reversibly. The returned name includes .sprite.
    bool BlueprintFileName(const std::string& canonicalBlueprintPath, std::string& fileName);
    bool GetCacheRoot(std::filesystem::path& directory);
    bool Clear();
    bool Read(const std::string& canonicalBlueprintPath, Sprite& out);
    bool Write(const std::string& canonicalBlueprintPath, const uint8_t* rgba,
        uint32_t width, uint32_t height, const SpatialBounds& bounds);
    bool Exists(const std::string& canonicalBlueprintPath);
    bool WriteDiagnosticPng(const std::string& canonicalBlueprintPath,
        const uint8_t* rgba, uint32_t width, uint32_t height);
}

#endif
