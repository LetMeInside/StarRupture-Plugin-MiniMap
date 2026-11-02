#pragma once

#include "ImageDestination.h"
#include "../Map/Buildings/BuildingImageRasterizationCore.h"

namespace MiniMap::Services
{
    // Neutral world grid for an offscreen destination. Cell coordinates identify
    // an integer tile in the global pixel lattice; they carry no game cell identity.
    struct ImageWorldGrid
    {
        double WorldOriginX = 0, WorldOriginY = 0;
        int64_t CellX = 0, CellY = 0;
        uint32_t Width = 0, Height = 0;
        double WorldUnitsPerPixel = 0;
    };

    class Rgba16fImageDestination final : public IImageDestination
    {
    public:
        explicit Rgba16fImageDestination(ImageWorldGrid grid) noexcept;
        ImageSubmission SubmitImage(const ImageDrawingCommand& command) override;

        // Submits one world-space solid triangle into the same multisample
        // working buffer as SubmitImage. Vertex X/Y are world coordinates and
        // Z is ignored. RGB uses the low 24 bits as 0xRRGGBB in sRGB; opacity
        // is straight 8-bit alpha (255 matches existing spline fills).
        // Returns Culled for an off-grid XY bounding box, InvalidGeometry for
        // non-finite XY, Rejected on raster failure, and poisons Finalize then.
        ImageSubmission SubmitSolidTriangle(
            const std::array<std::array<double, 3>, 3>& worldTriangle,
            uint32_t rgb, uint8_t opacity = 255) noexcept;

        // Finalizes the complete working buffer as row-major little-endian RGBA16F.
        // Returns false and clears output after any failed submission or allocation.
        bool Finalize(std::vector<uint8_t>& output) const noexcept;
        const ImageWorldGrid& Grid() const noexcept { return m_grid; }

    private:
        ImageWorldGrid m_grid{};
        MiniMapBuildingImageRasterizationCore::Grid m_rasterGrid{};
        MiniMapBuildingImageRasterizationCore::SampleBuffer m_samples;
        bool m_initialized = false;
        bool m_failed = false;
    };
}
