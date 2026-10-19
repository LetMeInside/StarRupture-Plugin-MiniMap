#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace MiniMapBuildingAtlasCore
{
    inline constexpr std::uint16_t SourceSize = 256; // Input dimension limit, never a reserved atlas slot.
    inline constexpr std::uint16_t Gutter = 2;
    inline constexpr std::size_t MaximumSources = 64;

    // Half-open rectangle in the processed input image. Its origin is zero;
    // capture-space anchoring belongs to the coordinator's spatial metadata.
    struct CropRect
    {
        std::uint16_t X{}, Y{}, Width{}, Height{};
    };
    struct Rect
    {
        std::uint16_t X{}, Y{}, Width{}, Height{};
    };
    struct UV { double U{}, V{}; };
    struct CroppedSource
    {
        std::string Identifier;
        CropRect Crop;
        std::vector<std::uint8_t> Rgba; // Processed Width * Height * 4; never resampled here.
    };
    struct PackedSource
    {
        std::string Identifier;
        CropRect Crop;
        Rect Allocation, Content;
        std::array<UV, 4> Corners{}; // TL, TR, BR, BL; content boundaries, excluding gutters.
    };
    struct Layout
    {
        std::uint16_t Width{}, Height{}; // Zero until the first successful repack.
        std::uint64_t Revision{};
        std::vector<PackedSource> Images; // Deterministic packing order.
        std::vector<std::uint8_t> Rgba;
        // Retained but not admitted by the last successful RepackSubset, in
        // unsigned identifier-byte order. Images identifies all admitted sources.
        // Add does not update this layout snapshot; complete-set Repack clears it.
        std::vector<std::string> DeferredIdentifiers;
    };
    enum class Result
    {
        Ok, UnsupportedDimensions, InvalidBuffer, EmptyIdentifier, DuplicateIdentifier,
        SourceLimit, EmptyAlpha, EmptySources, Overflow, AllocationFailure, Capacity, InternalError
    };

    // Single-owner CPU object. Callers serialize mutation and inspection.
    class Atlas
    {
    public:
        // Copies an already processed positive image bounded by SourceSize on
        // each axis. No crop/downscale is repeated; input ownership stays with caller.
        // Failure leaves sources and current layout unchanged; identifiers are opaque bytes.
        Result Add(const std::string& identifier, std::uint32_t width, std::uint32_t height,
            std::span<const std::uint8_t> rgba) noexcept;
        // Complete-set attempts at 512 then 1024, without shrinking the current
        // canvas. Failure preserves the last layout.
        Result Repack() noexcept;
        // Current().Images is mandatory. Consider remaining retained identifiers
        // in ascending unsigned-byte order, accepting each only if the entire
        // selection fits at 512 or 1024 without shrinking. Commit once; Ok can
        // include capacity-deferred sources in Current().DeferredIdentifiers.
        // Errors preserve all current metadata, pixels, revision and sources.
        Result RepackSubset() noexcept;
        // Read-only views: Sources references invalidate on a successful Add;
        // Current references into buffers invalidate on a successful Repack or
        // RepackSubset. Add never changes Current; neither repack changes Sources.
        const std::vector<CroppedSource>& Sources() const noexcept { return Retained; }
        const Layout& Current() const noexcept { return Packed; }

    private:
        std::vector<CroppedSource> Retained; // Sorted by unsigned identifier bytes, not status data.
        Layout Packed;
    };
}
