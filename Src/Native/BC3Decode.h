#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace MiniMapBC3
{
    namespace
    {
        struct RGBA8
        {
            uint8_t R;
            uint8_t G;
            uint8_t B;
            uint8_t A;
        };

        static inline uint8_t Expand5To8(uint16_t value)
        {
            return static_cast<uint8_t>((value * 255u + 15u) / 31u);
        }

        static inline uint8_t Expand6To8(uint16_t value)
        {
            return static_cast<uint8_t>((value * 255u + 31u) / 63u);
        }

        static inline RGBA8 DecodeRGB565(uint16_t value)
        {
            return {
                Expand5To8(static_cast<uint16_t>((value >> 11) & 0x1Fu)),
                Expand6To8(static_cast<uint16_t>((value >> 5) & 0x3Fu)),
                Expand5To8(static_cast<uint16_t>(value & 0x1Fu)),
                255
            };
        }

        static inline RGBA8 InterpolateRGB(
            const RGBA8& a,
            const RGBA8& b,
            uint32_t aWeight,
            uint32_t bWeight,
            uint32_t divisor)
        {
            return {
                static_cast<uint8_t>((aWeight * a.R + bWeight * b.R) / divisor),
                static_cast<uint8_t>((aWeight * a.G + bWeight * b.G) / divisor),
                static_cast<uint8_t>((aWeight * a.B + bWeight * b.B) / divisor),
                255
            };
        }

        static inline void DecodeBC3Block(
            const uint8_t* block,
            uint8_t* destination,
            int destinationStrideBytes)
        {
            uint8_t alphaValues[8] = {};
            alphaValues[0] = block[0];
            alphaValues[1] = block[1];

            if (alphaValues[0] > alphaValues[1])
            {
                for (uint32_t i = 1; i <= 6; ++i)
                {
                    alphaValues[i + 1] =
                        static_cast<uint8_t>(
                            ((7u - i) * alphaValues[0] +
                                i * alphaValues[1]) / 7u);
                }
            }
            else
            {
                for (uint32_t i = 1; i <= 4; ++i)
                {
                    alphaValues[i + 1] =
                        static_cast<uint8_t>(
                            ((5u - i) * alphaValues[0] +
                                i * alphaValues[1]) / 5u);
                }

                alphaValues[6] = 0;
                alphaValues[7] = 255;
            }

            uint64_t alphaIndices = 0;
            for (int i = 0; i < 6; ++i)
            {
                alphaIndices |=
                    static_cast<uint64_t>(block[2 + i]) << (8 * i);
            }

            const uint8_t* colorBlock = block + 8;

            const uint16_t color0 =
                static_cast<uint16_t>(
                    colorBlock[0] |
                    (static_cast<uint16_t>(colorBlock[1]) << 8));

            const uint16_t color1 =
                static_cast<uint16_t>(
                    colorBlock[2] |
                    (static_cast<uint16_t>(colorBlock[3]) << 8));

            RGBA8 colors[4] = {};
            colors[0] = DecodeRGB565(color0);
            colors[1] = DecodeRGB565(color1);
            colors[2] = InterpolateRGB(colors[0], colors[1], 2, 1, 3);
            colors[3] = InterpolateRGB(colors[0], colors[1], 1, 2, 3);

            const uint32_t colorIndices =
                static_cast<uint32_t>(colorBlock[4]) |
                (static_cast<uint32_t>(colorBlock[5]) << 8) |
                (static_cast<uint32_t>(colorBlock[6]) << 16) |
                (static_cast<uint32_t>(colorBlock[7]) << 24);

            for (int y = 0; y < 4; ++y)
            {
                uint8_t* row =
                    destination + y * destinationStrideBytes;

                for (int x = 0; x < 4; ++x)
                {
                    const uint32_t pixelIndex =
                        static_cast<uint32_t>(y * 4 + x);

                    const uint32_t colorIndex =
                        (colorIndices >> (pixelIndex * 2u)) & 0x3u;

                    const uint32_t alphaIndex =
                        static_cast<uint32_t>(
                            (alphaIndices >> (pixelIndex * 3u)) & 0x7u);

                    const RGBA8& color = colors[colorIndex];

                    uint8_t* pixel = row + x * 4;
                    pixel[0] = color.R;
                    pixel[1] = color.G;
                    pixel[2] = color.B;
                    pixel[3] = alphaValues[alphaIndex];
                }
            }
        }
    }

    static inline bool DecodeBC3Texture(
        const uint8_t* source,
        int width,
        int height,
        size_t sourceBytes,
        std::vector<uint8_t>& rgba)
    {
        if (source == nullptr ||
            width <= 0 ||
            height <= 0 ||
            width % 4 != 0 ||
            height % 4 != 0)
        {
            return false;
        }

        const int blocksX = width / 4;
        const int blocksY = height / 4;

        const size_t expectedBytes =
            static_cast<size_t>(blocksX) *
            static_cast<size_t>(blocksY) *
            16u;

        if (sourceBytes != expectedBytes)
        {
            return false;
        }

        rgba.resize(
            static_cast<size_t>(width) *
            static_cast<size_t>(height) *
            4u);

        const int strideBytes = width * 4;

        for (int blockY = 0; blockY < blocksY; ++blockY)
        {
            for (int blockX = 0; blockX < blocksX; ++blockX)
            {
                const size_t blockIndex =
                    static_cast<size_t>(blockY) *
                    static_cast<size_t>(blocksX) +
                    static_cast<size_t>(blockX);

                const uint8_t* sourceBlock =
                    source + blockIndex * 16u;

                uint8_t* destinationPixel =
                    rgba.data() +
                    static_cast<size_t>(blockY * 4) *
                        static_cast<size_t>(strideBytes) +
                    static_cast<size_t>(blockX * 4) * 4u;

                DecodeBC3Block(
                    sourceBlock,
                    destinationPixel,
                    strideBytes);
            }
        }

        return true;
    }
}
