#if defined(MODLOADER_CLIENT_BUILD)

#include "MapOverlays.h"

#include "Map/MapTransform.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace
{
    constexpr float kCompassInsetPixels =
        14.0f;

    constexpr float kMarkerTipDistance =
        11.0f;

    constexpr float kMarkerRearDistance =
        7.0f;

    constexpr float kMarkerHalfWidth =
        6.5f;

    constexpr uint32_t kMarkerFillColor =
        0xFFFFFFFFu;

    constexpr uint32_t kMarkerOutlineColor =
        0xE0181818u;

    constexpr uint32_t kCompassTextColor =
        0xFFFFFFFFu;

    constexpr uint32_t kCompassShadowColor =
        0xC0181818u;

    constexpr double kDirectionEpsilon =
        1.0e-9;

    constexpr double kDegreesToRadians =
        3.1415926535897932384626433832795 /
        180.0;


    struct CompassDirection
    {
        const char* Label = nullptr;
        double WorldX = 0.0;
        double WorldY = 0.0;
    };


    // MiniMap convention:
    //
    // North is the top of StarRupture's unrotated in-game map. The proven
    // source-map axes are +X right and +Y down, therefore source-map-up is -Y.
    //
    // This is an explicit MiniMap convention; StarRupture does not expose a
    // separately verified geographic North vector.
    constexpr CompassDirection kCompassDirections[] = {
        { "N",  0.0, -1.0 },
        { "E",  1.0,  0.0 },
        { "S",  0.0,  1.0 },
        { "W", -1.0,  0.0 }
    };


    MiniMapMap::ScreenPoint RotateScreenOffset(
        float x,
        float y,
        double clockwiseDegrees)
    {
        const double radians =
            clockwiseDegrees *
            kDegreesToRadians;

        const double c =
            std::cos(
                radians);

        const double s =
            std::sin(
                radians);

        MiniMapMap::ScreenPoint result = {};

        result.X =
            static_cast<float>(
                c * static_cast<double>(x) -
                s * static_cast<double>(y));

        result.Y =
            static_cast<float>(
                s * static_cast<double>(x) +
                c * static_cast<double>(y));

        return result;
    }


    void DrawPlayerMarker(
        IModLoaderImGui* ui,
        PluginDrawList drawList,
        const MiniMapMap::Transform& transform)
    {
        const double markerAngleDegrees =
            transform.PlayerMarkerScreenAngleDegrees();

        const MiniMapMap::ScreenPoint tip =
            RotateScreenOffset(
                0.0f,
                -kMarkerTipDistance,
                markerAngleDegrees);

        const MiniMapMap::ScreenPoint rearRight =
            RotateScreenOffset(
                kMarkerHalfWidth,
                kMarkerRearDistance,
                markerAngleDegrees);

        const MiniMapMap::ScreenPoint rearLeft =
            RotateScreenOffset(
                -kMarkerHalfWidth,
                kMarkerRearDistance,
                markerAngleDegrees);

        const float tipX =
            transform.CenterX +
            tip.X;

        const float tipY =
            transform.CenterY +
            tip.Y;

        const float rightX =
            transform.CenterX +
            rearRight.X;

        const float rightY =
            transform.CenterY +
            rearRight.Y;

        const float leftX =
            transform.CenterX +
            rearLeft.X;

        const float leftY =
            transform.CenterY +
            rearLeft.Y;

        ui->DL_AddTriangleFilled(
            drawList,
            tipX,
            tipY,
            rightX,
            rightY,
            leftX,
            leftY,
            kMarkerFillColor);

        ui->DL_AddTriangle(
            drawList,
            tipX,
            tipY,
            rightX,
            rightY,
            leftX,
            leftY,
            kMarkerOutlineColor,
            1.5f);
    }


    void DrawCompassLabel(
        IModLoaderImGui* ui,
        PluginDrawList drawList,
        const MiniMapMap::Transform& transform,
        const CompassDirection& cardinal)
    {
        MiniMapMap::ScreenPoint direction = {};

        if (!transform.DirectionToScreen(
            cardinal.WorldX,
            cardinal.WorldY,
            direction))
        {
            return;
        }

        const double directionLengthSquared =
            static_cast<double>(
                direction.X) *
            static_cast<double>(
                direction.X) +
            static_cast<double>(
                direction.Y) *
            static_cast<double>(
                direction.Y);

        if (!std::isfinite(directionLengthSquared) ||
            directionLengthSquared <=
            kDirectionEpsilon)
        {
            return;
        }

        float textWidth = 0.0f;
        float textHeight = 0.0f;

        ui->CalcTextSize(
            cardinal.Label,
            &textWidth,
            &textHeight,
            false,
            0.0f);

        const double halfWidth =
            static_cast<double>(
                transform.Width) *
            0.5 -
            static_cast<double>(
                kCompassInsetPixels) -
            static_cast<double>(
                textWidth) *
            0.5;

        const double halfHeight =
            static_cast<double>(
                transform.Height) *
            0.5 -
            static_cast<double>(
                kCompassInsetPixels) -
            static_cast<double>(
                textHeight) *
            0.5;

        if (halfWidth <= 0.0 ||
            halfHeight <= 0.0)
        {
            return;
        }

        const double absX =
            std::abs(
                static_cast<double>(
                    direction.X));

        const double absY =
            std::abs(
                static_cast<double>(
                    direction.Y));

        const double infinity =
            std::numeric_limits<double>::infinity();

        const double tx =
            absX >
            kDirectionEpsilon
            ? halfWidth /
            absX
            : infinity;

        const double ty =
            absY >
            kDirectionEpsilon
            ? halfHeight /
            absY
            : infinity;

        const double t =
            (std::min)(
                tx,
                ty);

        if (!std::isfinite(t))
        {
            return;
        }

        const float centerX =
            transform.CenterX +
            static_cast<float>(
                t *
                static_cast<double>(
                    direction.X));

        const float centerY =
            transform.CenterY +
            static_cast<float>(
                t *
                static_cast<double>(
                    direction.Y));

        const float textX =
            centerX -
            textWidth *
            0.5f;

        const float textY =
            centerY -
            textHeight *
            0.5f;

        ui->DL_AddText(
            drawList,
            textX + 1.0f,
            textY + 1.0f,
            kCompassShadowColor,
            cardinal.Label);

        ui->DL_AddText(
            drawList,
            textX,
            textY,
            kCompassTextColor,
            cardinal.Label);
    }
}


namespace MiniMapOverlays
{
    void Render(
        IModLoaderImGui* ui,
        const MiniMapMap::Transform& transform)
    {
        if (ui == nullptr ||
            !transform.Valid)
        {
            return;
        }

        PluginDrawList drawList =
            ui->GetWindowDrawList();

        for (const CompassDirection& cardinal :
            kCompassDirections)
        {
            DrawCompassLabel(
                ui,
                drawList,
                transform,
                cardinal);
        }

        DrawPlayerMarker(
            ui,
            drawList,
            transform);
    }
}

#endif