// Keeps native chunk coordinates and legacy fixed-size entry points; generic BC1 work belongs to TerrainService.
#if defined(MODLOADER_CLIENT_BUILD)

#include "TerrainChunks.h"
#include "Services/Map/Terrain/Processing/TerrainBc1Codec.h"
#include <cmath>

namespace MiniMapTerrainChunks
{
    /** Maps native normalized tile coordinates to the existing eight-by-eight chunk grid. */
    bool TryGetChunkCoordinates(
        double localU,
        double localV,
        int& outChunkX,
        int& outChunkY)
    {
        outChunkX =
            static_cast<int>(
                std::floor(
                    localU *
                    kChunksPerAxis));

        outChunkY =
            static_cast<int>(
                std::floor(
                    localV *
                    kChunksPerAxis));

        return
            outChunkX >= 0 &&
            outChunkX < kChunksPerAxis &&
            outChunkY >= 0 &&
            outChunkY < kChunksPerAxis;
    }


    /** Forwards complete native chunks without changing their compressed bytes or fixed-size contract.
     * The legacy pointer has no length; its caller must supply the complete source allocation.
     */
    bool ExtractBC1Chunk(const uint8_t* source, int sourceWidth, int sourceHeight,
        int chunkX, int chunkY, std::vector<uint8_t>& compressedChunk)
    {
        if (!source || sourceWidth <= 0 || sourceHeight <= 0 || sourceWidth % 4 || sourceHeight % 4 ||
            chunkX < 0 || chunkY < 0 || sourceWidth < kChunkSizePixels || sourceHeight < kChunkSizePixels ||
            chunkX > (sourceWidth - kChunkSizePixels) / kChunkSizePixels ||
            chunkY > (sourceHeight - kChunkSizePixels) / kChunkSizePixels) return false;
        const auto bytes = Services::Map::Terrain::TerrainImage::CalculateByteCount(sourceWidth, sourceHeight);
        if (!bytes) return false;
        return Services::Map::Terrain::Processing::TerrainBc1Codec::Extract(
            std::span<const uint8_t>(source, *bytes), sourceWidth, sourceHeight,
            static_cast<uint32_t>(chunkX) * kChunkSizePixels,
            static_cast<uint32_t>(chunkY) * kChunkSizePixels,
            kChunkSizePixels, kChunkSizePixels, compressedChunk);
    }

    /** Preserves legacy RGB and alpha for the active renderer; M3 reviews opaque presentation cutover. */
    bool DecodeBC1Chunk(const std::vector<uint8_t>& compressedChunk, std::vector<uint8_t>& rgbaChunk)
    {
        return Services::Map::Terrain::Processing::TerrainBc1Codec::Decode(
            compressedChunk, kChunkSizePixels, kChunkSizePixels, rgbaChunk, false);
    }
}

#endif
