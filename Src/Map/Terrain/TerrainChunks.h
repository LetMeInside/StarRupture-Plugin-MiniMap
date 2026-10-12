#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include <cstdint>
#include <vector>

namespace MiniMapTerrainChunks
{
    constexpr int kChunksPerAxis = 8;
    constexpr int kChunkSizePixels = 256;
    constexpr int kChunkWorldSizeMeters = 125;

    constexpr int kBC1BlockSizePixels = 4;
    constexpr int kBC1BytesPerBlock = 8;

    bool TryGetChunkCoordinates(
        double localU,
        double localV,
        int& outChunkX,
        int& outChunkY);

    bool ExtractBC1Chunk(
        const uint8_t* source,
        int sourceWidth,
        int sourceHeight,
        int chunkX,
        int chunkY,
        std::vector<uint8_t>& compressedChunk);

    bool DecodeBC1Chunk(
        const std::vector<uint8_t>& compressedChunk,
        std::vector<uint8_t>& rgbaChunk);
}

#endif
