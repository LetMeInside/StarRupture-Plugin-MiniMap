#if defined(MODLOADER_CLIENT_BUILD)

#include "TerrainChunks.h"

#include <cmath>
#include <cstring>

namespace
{
    struct RGBA8
    {
        uint8_t R;
        uint8_t G;
        uint8_t B;
        uint8_t A;
    };


    uint8_t Expand5To8(
        uint16_t value)
    {
        return static_cast<uint8_t>(
            (value * 255u + 15u) / 31u);
    }


    uint8_t Expand6To8(
        uint16_t value)
    {
        return static_cast<uint8_t>(
            (value * 255u + 31u) / 63u);
    }


    RGBA8 DecodeRGB565(
        uint16_t value)
    {
        RGBA8 color{};

        color.R =
            Expand5To8(
                static_cast<uint16_t>(
                    (value >> 11) & 0x1Fu));

        color.G =
            Expand6To8(
                static_cast<uint16_t>(
                    (value >> 5) & 0x3Fu));

        color.B =
            Expand5To8(
                static_cast<uint16_t>(
                    value & 0x1Fu));

        color.A = 255;

        return color;
    }


    RGBA8 InterpolateColor(
        const RGBA8& a,
        const RGBA8& b,
        uint32_t aWeight,
        uint32_t bWeight,
        uint32_t divisor)
    {
        RGBA8 result{};

        result.R = static_cast<uint8_t>(
            (aWeight * a.R + bWeight * b.R) /
            divisor);

        result.G = static_cast<uint8_t>(
            (aWeight * a.G + bWeight * b.G) /
            divisor);

        result.B = static_cast<uint8_t>(
            (aWeight * a.B + bWeight * b.B) /
            divisor);

        result.A = static_cast<uint8_t>(
            (aWeight * a.A + bWeight * b.A) /
            divisor);

        return result;
    }


    void DecodeBC1Block(
        const uint8_t* block,
        uint8_t* rgba,
        int rgbaStrideBytes)
    {
        const uint16_t color0 =
            static_cast<uint16_t>(
                block[0] |
                (static_cast<uint16_t>(
                    block[1]) << 8));

        const uint16_t color1 =
            static_cast<uint16_t>(
                block[2] |
                (static_cast<uint16_t>(
                    block[3]) << 8));

        RGBA8 colors[4] = {};

        colors[0] =
            DecodeRGB565(
                color0);

        colors[1] =
            DecodeRGB565(
                color1);

        if (color0 > color1)
        {
            colors[2] =
                InterpolateColor(
                    colors[0],
                    colors[1],
                    2,
                    1,
                    3);

            colors[3] =
                InterpolateColor(
                    colors[0],
                    colors[1],
                    1,
                    2,
                    3);
        }
        else
        {
            colors[2] =
                InterpolateColor(
                    colors[0],
                    colors[1],
                    1,
                    1,
                    2);

            colors[3] = { 0, 0, 0, 0 };
        }

        const uint32_t indices =
            static_cast<uint32_t>(
                block[4]) |
            (static_cast<uint32_t>(
                block[5]) << 8) |
            (static_cast<uint32_t>(
                block[6]) << 16) |
            (static_cast<uint32_t>(
                block[7]) << 24);

        for (int y = 0; y < 4; ++y)
        {
            uint8_t* row =
                rgba +
                y * rgbaStrideBytes;

            for (int x = 0; x < 4; ++x)
            {
                const uint32_t pixelIndex =
                    static_cast<uint32_t>(
                        y * 4 + x);

                const uint32_t colorIndex =
                    (indices >>
                        (pixelIndex * 2u)) &
                    0x3u;

                const RGBA8& color =
                    colors[colorIndex];

                uint8_t* pixel =
                    row +
                    x * 4;

                pixel[0] = color.R;
                pixel[1] = color.G;
                pixel[2] = color.B;
                pixel[3] = color.A;
            }
        }
    }
}


namespace MiniMapTerrainChunks
{
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


    bool ExtractBC1Chunk(
        const uint8_t* source,
        int sourceWidth,
        int sourceHeight,
        int chunkX,
        int chunkY,
        std::vector<uint8_t>& compressedChunk)
    {
        if (source == nullptr ||
            sourceWidth <= 0 ||
            sourceHeight <= 0 ||
            sourceWidth % kBC1BlockSizePixels != 0 ||
            sourceHeight % kBC1BlockSizePixels != 0)
        {
            return false;
        }

        const int sourceBlocksPerRow =
            sourceWidth /
            kBC1BlockSizePixels;

        const int sourceBlockRows =
            sourceHeight /
            kBC1BlockSizePixels;

        const int chunkBlocksPerAxis =
            kChunkSizePixels /
            kBC1BlockSizePixels;

        const int startBlockX =
            chunkX *
            chunkBlocksPerAxis;

        const int startBlockY =
            chunkY *
            chunkBlocksPerAxis;

        if (startBlockX < 0 ||
            startBlockY < 0 ||
            startBlockX + chunkBlocksPerAxis >
            sourceBlocksPerRow ||
            startBlockY + chunkBlocksPerAxis >
            sourceBlockRows)
        {
            return false;
        }

        const size_t chunkRowBytes =
            static_cast<size_t>(
                chunkBlocksPerAxis) *
            kBC1BytesPerBlock;

        const size_t chunkByteCount =
            chunkRowBytes *
            static_cast<size_t>(
                chunkBlocksPerAxis);

        compressedChunk.resize(
            chunkByteCount);

        for (int blockRow = 0;
            blockRow < chunkBlocksPerAxis;
            ++blockRow)
        {
            const size_t sourceOffset =
                (static_cast<size_t>(
                    startBlockY +
                    blockRow) *
                    static_cast<size_t>(
                        sourceBlocksPerRow) +
                    static_cast<size_t>(
                        startBlockX)) *
                kBC1BytesPerBlock;

            const size_t destinationOffset =
                static_cast<size_t>(
                    blockRow) *
                chunkRowBytes;

            std::memcpy(
                compressedChunk.data() +
                destinationOffset,
                source +
                sourceOffset,
                chunkRowBytes);
        }

        return true;
    }


    bool DecodeBC1Chunk(
        const std::vector<uint8_t>& compressedChunk,
        std::vector<uint8_t>& rgbaChunk)
    {
        constexpr int blocksPerAxis =
            kChunkSizePixels /
            kBC1BlockSizePixels;

        const size_t expectedCompressedBytes =
            static_cast<size_t>(
                blocksPerAxis) *
            static_cast<size_t>(
                blocksPerAxis) *
            kBC1BytesPerBlock;

        if (compressedChunk.size() !=
            expectedCompressedBytes)
        {
            return false;
        }

        rgbaChunk.resize(
            static_cast<size_t>(
                kChunkSizePixels) *
            static_cast<size_t>(
                kChunkSizePixels) *
            4u);

        const int rgbaStrideBytes =
            kChunkSizePixels * 4;

        for (int blockY = 0;
            blockY < blocksPerAxis;
            ++blockY)
        {
            for (int blockX = 0;
                blockX < blocksPerAxis;
                ++blockX)
            {
                const size_t blockIndex =
                    static_cast<size_t>(
                        blockY) *
                    blocksPerAxis +
                    static_cast<size_t>(
                        blockX);

                const uint8_t* sourceBlock =
                    compressedChunk.data() +
                    blockIndex *
                    kBC1BytesPerBlock;

                uint8_t* destinationPixel =
                    rgbaChunk.data() +
                    static_cast<size_t>(
                        blockY *
                        kBC1BlockSizePixels) *
                    rgbaStrideBytes +
                    static_cast<size_t>(
                        blockX *
                        kBC1BlockSizePixels) *
                    4u;

                DecodeBC1Block(
                    sourceBlock,
                    destinationPixel,
                    rgbaStrideBytes);
            }
        }

        return true;
    }
}

#endif
