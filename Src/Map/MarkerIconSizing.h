#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include "Map/MapTransform.h"

#include <cmath>

namespace MiniMapMap
{
    inline constexpr float kBasePOIIconSizePixels =
        32.0f;

    inline constexpr float kMinZoomPOIIconScale =
        0.50f;

    inline constexpr float kMaxZoomPOIIconScale =
        0.75f;

    inline float CalculateMapMarkerIconSize(
        double metersPerPixel, float userScale)
    {
        const double normalizedZoom =
            std::log(
                MaximumMetersPerPixel /
                metersPerPixel) /
            std::log(
                MaximumMetersPerPixel /
                MinimumMetersPerPixel);

        const float zoomScale =
            std::lerp(
                kMinZoomPOIIconScale,
                kMaxZoomPOIIconScale,
                static_cast<float>(
                    normalizedZoom));

        return
            kBasePOIIconSizePixels *
            zoomScale *
            userScale;
    }
}

#endif
