#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace MiniMapAsyncReadbackExperiment::Pixels
{
    constexpr int Width = 37;
    constexpr int Height = 23;
    constexpr int SplitX = 13;
    constexpr int SplitY = 7;
    constexpr size_t ByteCount = Width * Height * 4;
    static_assert(ByteCount == 3404);

    struct Rgba { uint8_t R, G, B, A; };
    constexpr std::array<Rgba, 4> Colors{{
        {17, 67, 131, 193}, {29, 151, 223, 109},
        {241, 83, 37, 157}, {101, 211, 59, 239}
    }};

    constexpr Rgba Expected(int x, int y)
    {
        return Colors[(y >= SplitY ? 2 : 0) + (x >= SplitX ? 1 : 0)];
    }

    enum class Error { None, InvalidPitch, InvalidHeight, TruncatedBuffer };
    struct Result
    {
        Error Failure = Error::None;
        size_t Mismatches = 0;
        int FirstX = -1, FirstY = -1;
        Rgba FirstExpected{}, FirstActual{};
        std::array<uint8_t, ByteCount> Bgra{};
    };

    // This accepts already CPU-owned bytes. It neither maps nor waits for a GPU.
    // Native lock/unlock and readiness must be implemented separately.
    inline Result CopyAndVerify(std::span<const uint8_t> staging, int pitchPixels, int bufferHeight)
    {
        Result result;
        // Explicit bounded arithmetic; this experiment never expects a huge map.
        if (pitchPixels < Width || pitchPixels > 16384)
        {
            result.Failure = Error::InvalidPitch;
            return result;
        }
        if (bufferHeight < Height)
        {
            result.Failure = Error::InvalidHeight;
            return result;
        }
        const size_t stride = size_t(pitchPixels) * 4;
        if (staging.size() < stride * (Height - 1) + Width * 4)
        {
            result.Failure = Error::TruncatedBuffer;
            return result;
        }
        for (int y = 0; y < Height; ++y)
        {
            auto* row = result.Bgra.data() + y * Width * 4;
            std::memcpy(row, staging.data() + y * stride, Width * 4);
            for (int x = 0; x < Width; ++x)
            {
                const auto expected = Expected(x, y);
                const auto* pixel = row + x * 4;
                const Rgba actual{pixel[2], pixel[1], pixel[0], pixel[3]};
                if (actual.R != expected.R || actual.G != expected.G ||
                    actual.B != expected.B || actual.A != expected.A)
                {
                    if (result.Mismatches++ == 0)
                    {
                        result.FirstX = x; result.FirstY = y;
                        result.FirstExpected = expected; result.FirstActual = actual;
                    }
                }
            }
        }
        return result;
    }
}
