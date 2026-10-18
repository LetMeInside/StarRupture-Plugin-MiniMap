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
        uint32_t width, uint32_t height, const SpatialBounds& bounds);
    // Full source canvas beside its .sprite; optional export cannot throw into
    // production publication. Enabled separately from native capture by MSBuild.
    bool WriteDiagnosticPng(const std::string& canonicalBlueprintPath,
        const uint8_t* rgba, uint32_t width, uint32_t height) noexcept;
    // Full published atlas only. Sequential, exclusive final-name publication;
    // logs its result and never changes GPU publication. Call outside lifecycle lock.
    bool WriteAtlasDiagnosticPng(const std::vector<uint8_t>& rgba,
        uint32_t width, uint32_t height, uint64_t revision) noexcept;
}

#endif
