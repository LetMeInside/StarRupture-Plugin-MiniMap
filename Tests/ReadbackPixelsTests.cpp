#include "../Src/Experiments/ReadbackPixels.h"

#include <cstdio>
#include <vector>

namespace P = MiniMapAsyncReadbackExperiment::Pixels;
#define CHECK(condition) do { if (!(condition)) { std::printf("FAIL line %d: %s\n", __LINE__, #condition); return 1; } } while (false)

std::vector<uint8_t> Pattern(int pitch)
{
    std::vector<uint8_t> bytes(size_t(pitch) * P::Height * 4, 0xCD);
    for (int y = 0; y < P::Height; ++y)
        for (int x = 0; x < P::Width; ++x)
        {
            const auto c = P::Expected(x, y);
            auto* pixel = bytes.data() + (size_t(y) * pitch + x) * 4;
            pixel[0] = c.B; pixel[1] = c.G; pixel[2] = c.R; pixel[3] = c.A;
        }
    return bytes;
}

int main()
{
    const auto tight = Pattern(P::Width);
    const auto padded = Pattern(64); // 256-byte D3D12 row alignment example.
    const auto a = P::CopyAndVerify(tight, P::Width, P::Height);
    const auto b = P::CopyAndVerify(padded, 64, P::Height);
    CHECK(a.Failure == P::Error::None && a.Mismatches == 0);
    CHECK(b.Failure == P::Error::None && b.Mismatches == 0);
    CHECK(a.Bgra == b.Bgra && a.Bgra.size() == 3404);

    auto swap = padded;
    for (int y = 0; y < P::Height; ++y)
        for (int x = 0; x < P::Width; ++x)
        {
            const size_t i = (size_t(y) * 64 + x) * 4;
            std::swap(swap[i], swap[i + 2]);
        }
    CHECK(P::CopyAndVerify(swap, 64, P::Height).Mismatches == 851);

    auto alpha = padded;
    alpha[3] ^= 1;
    CHECK(P::CopyAndVerify(alpha, 64, P::Height).Mismatches == 1);

    auto boundary = padded;
    const auto wrong = P::Expected(P::SplitX, 0);
    const size_t edge = (P::SplitX - 1) * 4;
    boundary[edge] = wrong.B; boundary[edge + 1] = wrong.G;
    boundary[edge + 2] = wrong.R; boundary[edge + 3] = wrong.A;
    const auto c = P::CopyAndVerify(boundary, 64, P::Height);
    CHECK(c.Mismatches == 1 && c.FirstX == 12 && c.FirstY == 0);

    auto last = padded;
    last[((P::Height - 1) * 64 + P::Width - 1) * 4] ^= 1;
    const auto d = P::CopyAndVerify(last, 64, P::Height);
    CHECK(d.Mismatches == 1 && d.FirstX == 36 && d.FirstY == 22);
    CHECK(P::CopyAndVerify(padded, 36, P::Height).Failure == P::Error::InvalidPitch);
    CHECK(P::CopyAndVerify(padded, 64, 22).Failure == P::Error::InvalidHeight);
    CHECK(P::CopyAndVerify(std::span(padded).first(100), 64, P::Height).Failure == P::Error::TruncatedBuffer);
    CHECK(P::CopyAndVerify({}, 64, P::Height).Failure == P::Error::TruncatedBuffer);
    std::puts("ReadbackPixels CPU checks passed. No GPU operations exercised.");
    return 0;
}
