#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include <cstddef>
#include <cstdint>
#include <vector>
#include <filesystem>

namespace MiniMapTerrainCache
{
    bool GetBuildingDiagnosticsDirectory(std::filesystem::path&);
    bool TryLoadChunk(
        int radiationLevel,
        int gridX,
        int gridY,
        int sourceWidth,
        int sourceHeight,
        uint8_t pixelFormat,
        bool sRGB,
        int chunkX,
        int chunkY,
        std::vector<uint8_t>& compressedChunk);

    bool StoreSourceTile(
        int radiationLevel,
        int gridX,
        int gridY,
        int sourceWidth,
        int sourceHeight,
        uint8_t pixelFormat,
        bool sRGB,
        const uint8_t* sourceData,
        size_t sourceDataSize);
}

#endif
