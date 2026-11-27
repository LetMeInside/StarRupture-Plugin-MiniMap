// Implements canonical BC1 validation, lossless extraction and the authoritative terrain decoder.
// The original project encoder scores two geometric endpoint seeds and a bounded RGB565 lattice search.
// It uses integer squared RGB error and standard BC1 packing, with no external encoder or lookup tables.
#include "TerrainBc1Codec.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace Services::Map::Terrain::Processing
{
    /** Validates size without interpreting endpoint ordering as an opacity guarantee. */
    bool TerrainBc1Codec::Validate(std::span<const std::uint8_t> blocks,
        std::uint32_t width, std::uint32_t height) noexcept
    {
        const auto bytes = TerrainImage::CalculateByteCount(width, height);
        return bytes && blocks.size() == *bytes;
    }

    /** Copies complete compressed blocks; the caller keeps logical extent separate from storage padding. */
    bool TerrainBc1Codec::Extract(std::span<const std::uint8_t> source, std::uint32_t sourceWidth,
        std::uint32_t sourceHeight, std::uint32_t x, std::uint32_t y,
        std::uint32_t width, std::uint32_t height, std::vector<std::uint8_t>& output)
    {
        const auto bytes = TerrainImage::CalculateByteCount(width, height);
        if (!Validate(source, sourceWidth, sourceHeight) || !bytes || x % 4u || y % 4u ||
            x >= sourceWidth || y >= sourceHeight || width > sourceWidth - x || height > sourceHeight - y)
        {
            return false;
        }
        const std::size_t sourceStride = ((static_cast<std::size_t>(sourceWidth) + 3u) / 4u) * 8u;
        const std::size_t stride = ((static_cast<std::size_t>(width) + 3u) / 4u) * 8u;
        std::vector<std::uint8_t> result(*bytes);
        for (std::size_t row = 0; row < (height + 3u) / 4u; ++row)
        {
            const auto offset = (y / 4u + row) * sourceStride + (x / 4u) * 8u;
            std::memcpy(result.data() + row * stride, source.data() + offset, stride);
        }
        output = std::move(result);
        return true;
    }

    /** Retains the native decoder's endpoint expansion, rather than substituting bit replication. */
    std::array<std::uint8_t, 4> TerrainBc1Codec::DecodeEndpoint(std::uint16_t value) noexcept
    {
        return {static_cast<std::uint8_t>((((value >> 11) & 31u) * 255u + 15u) / 31u),
            static_cast<std::uint8_t>((((value >> 5) & 63u) * 255u + 31u) / 63u),
            static_cast<std::uint8_t>(((value & 31u) * 255u + 15u) / 31u), 255};
    }

    /** Preserves integer RGB interpolation and black selector 3 in three-color mode. */
    std::array<std::array<std::uint8_t, 4>, 4> TerrainBc1Codec::DecodePalette(
        const std::uint8_t* block, bool opaque) noexcept
    {
        const auto endpoint0 = static_cast<std::uint16_t>(block[0] | (block[1] << 8));
        const auto endpoint1 = static_cast<std::uint16_t>(block[2] | (block[3] << 8));
        std::array<std::array<std::uint8_t, 4>, 4> colors{};
        colors[0] = DecodeEndpoint(endpoint0);
        colors[1] = DecodeEndpoint(endpoint1);
        colors[2][3] = 255;
        colors[3][3] = opaque || endpoint0 > endpoint1 ? 255 : 0;
        for (std::size_t channel = 0; channel < 3; ++channel)
        {
            if (endpoint0 > endpoint1)
            {
                colors[2][channel] = static_cast<std::uint8_t>((2u * colors[0][channel] + colors[1][channel]) / 3u);
                colors[3][channel] = static_cast<std::uint8_t>((colors[0][channel] + 2u * colors[1][channel]) / 3u);
            }
            else
            {
                colors[2][channel] = static_cast<std::uint8_t>((colors[0][channel] + colors[1][channel]) / 2u);
            }
        }
        return colors;
    }

    /** Writes only valid pixels, avoiding any read of padded pixels outside the logical image. */
    void TerrainBc1Codec::DecodeBlock(const std::uint8_t* block, std::uint32_t x, std::uint32_t y,
        std::uint32_t width, std::uint32_t height, std::uint8_t* output, bool opaque) noexcept
    {
        const auto colors = DecodePalette(block, opaque);
        for (std::uint32_t row = 0; row < 4u && y + row < height; ++row)
        {
            for (std::uint32_t column = 0; column < 4u && x + column < width; ++column)
            {
                const auto selector = (block[4u + row] >> (column * 2u)) & 3u;
                const auto offset = (static_cast<std::size_t>(y + row) * width + x + column) * 4u;
                std::memcpy(output + offset, colors[selector].data(), 4u);
            }
        }
    }

    /** Allocates only the logical upload extent and never changes compressed source data. */
    bool TerrainBc1Codec::Decode(std::span<const std::uint8_t> blocks, std::uint32_t width,
        std::uint32_t height, std::vector<std::uint8_t>& output, bool opaque)
    {
        constexpr std::size_t maximumTemporaryBytes = 64u * 1024u * 1024u;
        if (!Validate(blocks, width, height) ||
            static_cast<std::size_t>(width) * height > maximumTemporaryBytes / 4u) return false;
        std::vector<std::uint8_t> result(static_cast<std::size_t>(width) * height * 4u);
        const std::size_t blocksX = (width + 3u) / 4u;
        for (std::uint32_t y = 0; y < height; y += 4u)
        {
            for (std::uint32_t x = 0; x < width; x += 4u)
            {
                const auto offset = (static_cast<std::size_t>(y / 4u) * blocksX + x / 4u) * 8u;
                DecodeBlock(blocks.data() + offset, x, y, width, height, result.data(), opaque);
            }
        }
        output = std::move(result);
        return true;
    }

    /** Clamps individual edge pixels, never repeats an entire last block as a padding approximation. */
    void TerrainBc1Codec::ReadRgbBlock(std::span<const std::uint8_t> pixels, std::uint32_t width,
        std::uint32_t height, std::size_t stride, std::uint32_t x, std::uint32_t y,
        std::array<std::uint8_t, 64>& block) noexcept
    {
        for (std::uint32_t row = 0; row < 4u; ++row)
        {
            const auto sourceRow = static_cast<std::size_t>((std::min)(y + row, height - 1u)) * stride;
            for (std::uint32_t column = 0; column < 4u; ++column)
            {
                const auto sourceOffset = sourceRow + (std::min)(x + column, width - 1u) * 3u;
                const auto destinationOffset = (row * 4u + column) * 4u;
                std::memcpy(block.data() + destinationOffset, pixels.data() + sourceOffset, 3u);
                block[destinationOffset + 3u] = 255;
            }
        }
    }

    /** Rounds RGB channels independently onto the five/six/five-bit endpoint lattice. */
    std::uint16_t TerrainBc1Codec::QuantizeEndpoint(const std::uint8_t* pixel) noexcept
    {
        const auto red = (static_cast<std::uint32_t>(pixel[0]) * 31u + 127u) / 255u;
        const auto green = (static_cast<std::uint32_t>(pixel[1]) * 63u + 127u) / 255u;
        const auto blue = (static_cast<std::uint32_t>(pixel[2]) * 31u + 127u) / 255u;
        return static_cast<std::uint16_t>((red << 11) | (green << 5) | blue);
    }

    /** Bounds one pixel's error by 3 * 255 squared, with no floating-point or alpha dependence. */
    std::uint32_t TerrainBc1Codec::CalculateColorError(const std::uint8_t* pixel,
        const std::uint8_t* color) noexcept
    {
        std::uint32_t error = 0;
        for (std::size_t channel = 0; channel < 3; ++channel)
        {
            const int difference = static_cast<int>(pixel[channel]) - color[channel];
            error += static_cast<std::uint32_t>(difference * difference);
        }
        return error;
    }

    /** Searches all 120 unordered source pairs, retaining an actual observed RGB direction. */
    void TerrainBc1Codec::FindEndpointPair(const std::array<std::uint8_t, 64>& block,
        std::uint16_t& endpoint0, std::uint16_t& endpoint1) noexcept
    {
        endpoint0 = QuantizeEndpoint(block.data());
        endpoint1 = endpoint0;
        std::uint32_t largestDistance = 0;
        for (std::size_t first = 0; first < 16; ++first)
        {
            for (std::size_t second = first + 1; second < 16; ++second)
            {
                const auto distance = CalculateColorError(block.data() + first * 4u,
                    block.data() + second * 4u);
                if (distance > largestDistance)
                {
                    largestDistance = distance;
                    endpoint0 = QuantizeEndpoint(block.data() + first * 4u);
                    endpoint1 = QuantizeEndpoint(block.data() + second * 4u);
                }
            }
        }
    }

    /** Scores the authoritative decoder's palette and packs row-major two-bit selectors little-endian. */
    std::uint32_t TerrainBc1Codec::EvaluateEndpoints(const std::array<std::uint8_t, 64>& block,
        std::uint16_t endpoint0, std::uint16_t endpoint1,
        std::array<std::uint8_t, 8>& encoded) noexcept
    {
        if (endpoint0 < endpoint1) std::swap(endpoint0, endpoint1);
        encoded[0] = static_cast<std::uint8_t>(endpoint0 & 255u);
        encoded[1] = static_cast<std::uint8_t>(endpoint0 >> 8);
        encoded[2] = static_cast<std::uint8_t>(endpoint1 & 255u);
        encoded[3] = static_cast<std::uint8_t>(endpoint1 >> 8);
        const auto colors = DecodePalette(encoded.data(), false);
        const std::uint32_t colorCount = endpoint0 == endpoint1 ? 1u : 4u;
        std::uint32_t totalError = 0;
        std::uint32_t selectors = 0;
        for (std::size_t pixel = 0; pixel < 16; ++pixel)
        {
            std::uint32_t bestSelector = 0;
            auto bestError = (std::numeric_limits<std::uint32_t>::max)();
            for (std::uint32_t selector = 0; selector < colorCount; ++selector)
            {
                const auto error = CalculateColorError(block.data() + pixel * 4u, colors[selector].data());
                if (error < bestError)
                {
                    bestError = error;
                    bestSelector = selector;
                }
            }
            totalError += bestError;
            selectors |= bestSelector << (pixel * 2u);
        }
        for (std::size_t byte = 0; byte < 4; ++byte)
        {
            encoded[4u + byte] = static_cast<std::uint8_t>((selectors >> (byte * 8u)) & 255u);
        }
        return totalError;
    }

    /** Tests an observed color direction against independent channel bounds; constant blocks need no branch. */
    std::uint32_t TerrainBc1Codec::SelectEndpoints(const std::array<std::uint8_t, 64>& block,
        std::array<std::uint8_t, 8>& encoded) noexcept
    {
        std::uint16_t endpoint0 = 0;
        std::uint16_t endpoint1 = 0;
        FindEndpointPair(block, endpoint0, endpoint1);
        auto error = EvaluateEndpoints(block, endpoint0, endpoint1, encoded);
        std::array<std::uint8_t, 3> lower{255, 255, 255};
        std::array<std::uint8_t, 3> upper{};
        for (std::size_t pixel = 0; pixel < 16; ++pixel)
        {
            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                lower[channel] = (std::min)(lower[channel], block[pixel * 4u + channel]);
                upper[channel] = (std::max)(upper[channel], block[pixel * 4u + channel]);
            }
        }
        std::array<std::uint8_t, 8> candidate{};
        const auto candidateError = EvaluateEndpoints(block, QuantizeEndpoint(upper.data()),
            QuantizeEndpoint(lower.data()), candidate);
        if (candidateError < error)
        {
            encoded = candidate;
            error = candidateError;
        }
        return error;
    }

    /** Evaluates symmetric component neighbors from the current pair, rejecting lattice overflow. */
    void TerrainBc1Codec::RefineEndpointComponent(const std::array<std::uint8_t, 64>& block,
        std::size_t endpoint, unsigned shift, int maximum, int step,
        std::array<std::uint8_t, 8>& encoded, std::uint32_t& error) noexcept
    {
        const std::array<std::uint16_t, 2> endpoints{
            static_cast<std::uint16_t>(encoded[0] | (encoded[1] << 8)),
            static_cast<std::uint16_t>(encoded[2] | (encoded[3] << 8))};
        const int component = (endpoints[endpoint] >> shift) & maximum;
        for (int direction = -1; direction <= 1; direction += 2)
        {
            const int adjusted = component + direction * step;
            if (adjusted < 0 || adjusted > maximum) continue;
            auto candidateEndpoints = endpoints;
            const auto mask = static_cast<std::uint32_t>(maximum) << shift;
            candidateEndpoints[endpoint] = static_cast<std::uint16_t>(
                (endpoints[endpoint] & ~mask) | (static_cast<std::uint32_t>(adjusted) << shift));
            std::array<std::uint8_t, 8> candidate{};
            const auto candidateError = EvaluateEndpoints(block, candidateEndpoints[0], candidateEndpoints[1], candidate);
            if (candidateError < error)
            {
                encoded = candidate;
                error = candidateError;
            }
        }
    }

    /** Uses one encoding policy: geometric seeding, then RGB component steps of four, two and one. */
    void TerrainBc1Codec::EncodeBlock(const std::array<std::uint8_t, 64>& block,
        std::uint8_t* destination) noexcept
    {
        std::array<std::uint8_t, 8> encoded{};
        auto error = SelectEndpoints(block, encoded);
        for (int step = 4; step >= 1; step /= 2)
        {
            for (std::size_t endpoint = 0; endpoint < 2; ++endpoint)
            {
                RefineEndpointComponent(block, endpoint, 11, 31, step, encoded, error);
                RefineEndpointComponent(block, endpoint, 5, 63, step, encoded, error);
                RefineEndpointComponent(block, endpoint, 0, 31, step, encoded, error);
            }
        }
        std::memcpy(destination, encoded.data(), encoded.size());
    }

    /** Encodes borrowed RGB8 synchronously with bounded integer endpoint selection per clamped block. */
    bool TerrainBc1Codec::EncodeRgb(std::span<const std::uint8_t> pixels, std::uint32_t width,
        std::uint32_t height, std::size_t rowStride, std::vector<std::uint8_t>& output)
    {
        const auto bytes = TerrainImage::CalculateByteCount(width, height);
        if (!bytes) return false;
        const auto rowBytes = static_cast<std::size_t>(width) * 3u;
        if (rowStride < rowBytes || (height > 1u &&
            rowStride > ((std::numeric_limits<std::size_t>::max)() - rowBytes) / (height - 1u))) return false;
        if (pixels.size() < static_cast<std::size_t>(height - 1u) * rowStride + rowBytes) return false;
        std::vector<std::uint8_t> result(*bytes);
        alignas(unsigned int) std::array<std::uint8_t, 64> block{};
        const std::size_t blocksX = (width + 3u) / 4u;
        for (std::uint32_t y = 0; y < height; y += 4u)
        {
            for (std::uint32_t x = 0; x < width; x += 4u)
            {
                ReadRgbBlock(pixels, width, height, rowStride, x, y, block);
                const auto offset = (static_cast<std::size_t>(y / 4u) * blocksX + x / 4u) * 8u;
                EncodeBlock(block, result.data() + offset);
            }
        }
        output = std::move(result);
        return true;
    }
}
