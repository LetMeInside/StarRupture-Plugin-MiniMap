// Private CPU BC1 validation, compressed-region copying and temporary pixel conversion.
// Owns no provider, cache, GPU resource or persistent uncompressed image.
#pragma once

#include "../TerrainImage.h"
#include <array>

namespace Services::Map::Terrain::Processing
{
    /** Stateless packed BC1 operations; borrowed input is consumed synchronously, never retained. */
    class TerrainBc1Codec final
    {
    public:
        /** Requires exact tightly packed BC1 size and the existing TerrainImage dimension limits. */
        static bool Validate(std::span<const std::uint8_t> blocks,
            std::uint32_t width, std::uint32_t height) noexcept;
        /** Copies a block-aligned origin and logical extent contained in source; leaves output on rejection.
         * Unused final-block pixels remain source padding, never additional represented coverage.
         */
        static bool Extract(std::span<const std::uint8_t> source, std::uint32_t sourceWidth,
            std::uint32_t sourceHeight, std::uint32_t x, std::uint32_t y,
            std::uint32_t width, std::uint32_t height, std::vector<std::uint8_t>& output);
        /** Decodes logical pixels only into owned row-major RGBA, bounded to 64 MiB temporary output.
         * Opaque presentation preserves three-color/black RGB. false retains legacy selector-3 alpha.
         * Rejects invalid input without changing output; allocation exceptions propagate.
         */
        static bool Decode(std::span<const std::uint8_t> blocks, std::uint32_t width,
            std::uint32_t height, std::vector<std::uint8_t>& output, bool opaque = true);
        /** Validates borrowed RGB8 rows, then encodes opaque BC1 with clamped final-block pixels.
         * Requires only a 64-byte stack pixel block; output is unchanged on invalid input.
         */
        static bool EncodeRgb(std::span<const std::uint8_t> pixels, std::uint32_t width,
            std::uint32_t height, std::size_t rowStride, std::vector<std::uint8_t>& output);

    private:
        /** Expands a little-endian RGB565 endpoint using the existing native decoder's rounding. */
        static std::array<std::uint8_t, 4> DecodeEndpoint(std::uint16_t value) noexcept;
        /** Builds the BC1 palette with legacy interpolation and explicit presentation alpha policy. */
        static std::array<std::array<std::uint8_t, 4>, 4> DecodePalette(
            const std::uint8_t* block, bool opaque) noexcept;
        /** Decodes one block's valid logical pixels into the supplied full-image temporary allocation. */
        static void DecodeBlock(const std::uint8_t* block, std::uint32_t x, std::uint32_t y,
            std::uint32_t width, std::uint32_t height, std::uint8_t* output, bool opaque) noexcept;
        /** Packs one clamped RGB8 block for the encoder, setting every temporary alpha byte to 255. */
        static void ReadRgbBlock(std::span<const std::uint8_t> pixels, std::uint32_t width,
            std::uint32_t height, std::size_t stride, std::uint32_t x, std::uint32_t y,
            std::array<std::uint8_t, 64>& block) noexcept;
        /** Quantizes one RGB8 pixel to nearest RGB565 components using integer rounding. */
        static std::uint16_t QuantizeEndpoint(const std::uint8_t* pixel) noexcept;
        /** Returns unweighted squared RGB distance; alpha is intentionally excluded. */
        static std::uint32_t CalculateColorError(const std::uint8_t* pixel,
            const std::uint8_t* color) noexcept;
        /** Finds the most separated source pixel pair; equal distances keep the first row-major pair. */
        static void FindEndpointPair(const std::array<std::uint8_t, 64>& block,
            std::uint16_t& endpoint0, std::uint16_t& endpoint1) noexcept;
        /** Packs ordered endpoints and nearest opaque selectors, returning total squared RGB error.
         * Writes exactly eight bytes; equal endpoints use selector zero exclusively.
         */
        static std::uint32_t EvaluateEndpoints(const std::array<std::uint8_t, 64>& block,
            std::uint16_t endpoint0, std::uint16_t endpoint1,
            std::array<std::uint8_t, 8>& encoded) noexcept;
        /** Chooses the lower-error seed from a separated pixel pair and component-wise RGB bounds.
         * Writes the winning encoded block and returns its error; ties retain the pixel-pair seed.
         */
        static std::uint32_t SelectEndpoints(const std::array<std::uint8_t, 64>& block,
            std::array<std::uint8_t, 8>& encoded) noexcept;
        /** Refines one packed endpoint component in both directions at a fixed lattice distance.
         * Retains only strictly lower-error candidates, updating the encoded block and its error.
         */
        static void RefineEndpointComponent(const std::array<std::uint8_t, 64>& block,
            std::size_t endpoint, unsigned shift, int maximum, int step,
            std::array<std::uint8_t, 8>& encoded, std::uint32_t& error) noexcept;
        /** Encodes one RGB block with two seeds and three bounded RGB565 coordinate-search passes.
         * Copies the winning eight-byte BC1 representation to destination; allocates no heap storage.
         */
        static void EncodeBlock(const std::array<std::uint8_t, 64>& block,
            std::uint8_t* destination) noexcept;
    };
}
