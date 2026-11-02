#pragma once

#include "Textures.h"
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace MiniMap::Services
{
    // Optional UI mapping data. WorldCorners remain authoritative and are kept
    // at double precision for destinations that do not use screen coordinates.
    struct ImageDestinationMapping
    {
        double TrackingWorldX = 0.0, TrackingWorldY = 0.0;
        double SinRotation = 0.0, CosRotation = 1.0;
        double PixelsPerWorldUnit = 0.0, PixelsPerWorldUnitY = 0.0;
        float CenterX = 0.0f, CenterY = 0.0f;
        double ClipMinX = 0.0, ClipMinY = 0.0, ClipMaxX = 0.0, ClipMaxY = 0.0;
        bool Valid = false;
    };

    struct GpuImageBinding
    {
        Texture TextureIdentity{}; // Borrowed identity; valid only during SubmitImage.
        std::array<double, 4> Region{};
        std::uint64_t Session = 0, Revision = 0;
    };

    enum class ImagePixelFormat { Rgba8 };

    struct ImageDrawingCommand
    {
        std::shared_ptr<const std::vector<std::uint8_t>> Pixels;
        ImagePixelFormat Format = ImagePixelFormat::Rgba8;
        std::uint32_t Width = 0, Height = 0;
        std::array<double, 4> SourceRegion{}; // Half-open coordinates in Pixels.
        std::array<std::array<double, 2>, 4> WorldCorners{};
        ImageDestinationMapping Destination{};
        bool HasGpuBinding = false;
        GpuImageBinding GpuBinding{};
    };

    enum class ImageSubmission { Submitted, Unavailable, InvalidGeometry, Culled, Rejected };

    // Submits an immutable image and its source-to-world correspondence.
    // Implementations may use Pixels directly or require GpuBinding; Pixels is
    // always independently owned and does not imply a GPU resource is available.
    // Submission is synchronous. Implementations must not retain the borrowed GPU identity.
    class IImageDestination
    {
    public:
        virtual ~IImageDestination() = default;
        virtual ImageSubmission SubmitImage(const ImageDrawingCommand&) = 0;
    };
}
