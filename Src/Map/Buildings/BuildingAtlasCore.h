#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace MiniMapBuildingAtlasCore
{
    inline constexpr std::uint16_t SourceSize = 256;
    inline constexpr std::uint16_t Gutter = 2;
    inline constexpr std::size_t MaximumSources = 64;

    // Half-open rectangle. Crop coordinates refer to the original source canvas;
    // allocation/content coordinates refer to the atlas canvas.
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
        std::vector<std::uint8_t> Rgba; // Crop.Width * Crop.Height * 4, no full source retained.
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
        // Copies only the guarded crop. The caller retains ownership of input.
        // Failure leaves sources and current layout unchanged; identifiers are opaque bytes.
        Result Add(const std::string& identifier, std::uint32_t width, std::uint32_t height,
            std::span<const std::uint8_t> rgba) noexcept;
        // Complete-set attempts at 512 then 1024. Failure preserves the last layout.
        Result Repack() noexcept;
        // Read-only views: Sources references invalidate on a successful Add;
        // Current references into buffers invalidate on a successful Repack.
        // Add never changes Current, and Repack never changes Sources.
        const std::vector<CroppedSource>& Sources() const noexcept { return Retained; }
        const Layout& Current() const noexcept { return Packed; }

    private:
        std::vector<CroppedSource> Retained; // Sorted by unsigned identifier bytes, not status data.
        Layout Packed;
    };
}
