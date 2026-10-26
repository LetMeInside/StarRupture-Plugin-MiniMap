#if defined(MODLOADER_CLIENT_BUILD)

#include "MapOverlays.h"
#include "Config/Config.h"

#include "Map/MapTransform.h"
#include "Map/FogOfWar/FogOfWar.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
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

    constexpr float kCoordinateCompassGapPixels =
        4.0f;

    constexpr uint32_t kMarkerFillColor =
        0xFFD4E500u;

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
        float centerX,
        float centerY,
        double markerAngleDegrees,
        bool wings = false)
    {
        if (wings)
        {
            for (const float side : {-1.0f, 1.0f})
            {
                const auto a = RotateScreenOffset(side * 4.0f, 0.0f, markerAngleDegrees);
                const auto b = RotateScreenOffset(side * 14.0f, 3.0f, markerAngleDegrees);
                const auto c = RotateScreenOffset(side * 7.0f, 7.0f, markerAngleDegrees);
                ui->DL_AddTriangleFilled(drawList, centerX+a.X, centerY+a.Y,
                    centerX+b.X, centerY+b.Y, centerX+c.X, centerY+c.Y, kMarkerFillColor);
                ui->DL_AddTriangle(drawList, centerX+a.X, centerY+a.Y,
                    centerX+b.X, centerY+b.Y, centerX+c.X, centerY+c.Y, kMarkerOutlineColor, 1.5f);
            }
        }

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
            centerX +
            tip.X;

        const float tipY =
            centerY +
            tip.Y;

        const float rightX =
            centerX +
            rearRight.X;

        const float rightY =
            centerY +
            rearRight.Y;

        const float leftX =
            centerX +
            rearLeft.X;

        const float leftY =
            centerY +
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


    void DrawPlayerCoordinates(
        IModLoaderImGui* ui,
        PluginDrawList drawList,
        const MiniMapMap::Transform& transform)
    {
        char coordinateText[96] = {};
        std::snprintf(coordinateText, sizeof(coordinateText),
            "X: %.0f m   Y: %.0f m",
            std::round(transform.PlayerWorldX / 100.0),
            std::round(transform.PlayerWorldY / 100.0));

        float textWidth = 0.0f;
        float textHeight = 0.0f;
        ui->CalcTextSize(coordinateText, &textWidth, &textHeight, false, 0.0f);

        const float textX = transform.CenterX - textWidth * 0.5f;
        const float textY = transform.CenterY + transform.Height * 0.5f -
            kCompassInsetPixels - textHeight * 0.5f -
            kCoordinateCompassGapPixels - textHeight;

        ui->DL_AddText(drawList, textX + 1.0f, textY + 1.0f,
            kCompassShadowColor, coordinateText);
        ui->DL_AddText(drawList, textX, textY,
            kCompassTextColor, coordinateText);
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
        const MiniMapMap::Transform& transform, bool showCoordinates, bool showCompass,
        const MiniMapConfig::FogOfWarSettings& fog)
    {
        if (ui == nullptr ||
            !transform.Valid)
        {
            return;
        }

        PluginDrawList drawList =
            ui->GetWindowDrawList();

        if (showCompass) for (const CompassDirection& cardinal :
            kCompassDirections)
        {
            DrawCompassLabel(
                ui,
                drawList,
                transform,
                cardinal);
        }

        if (transform.DroneActive)
        {
            MiniMapMap::ScreenPoint character{};
            if (transform.WorldToScreen(transform.PlayerWorldX, transform.PlayerWorldY, character) &&
                MiniMapFogOfWar::GetRenderedVisibilityAtWorldPosition(
                    transform.PlayerWorldX, transform.PlayerWorldY, transform, fog) > 0.0f)
            {
                const float left = transform.CenterX - transform.Width * 0.5f;
                const float top = transform.CenterY - transform.Height * 0.5f;
                ui->DL_PushClipRect(drawList, left, top, left+transform.Width, top+transform.Height, true);
                DrawPlayerMarker(ui, drawList, character.X, character.Y,
                    transform.CharacterWorldYawDegrees + 90.0 + transform.MapRotationDegrees);
                ui->DL_PopClipRect(drawList);
            }
        }
        DrawPlayerMarker(ui, drawList, transform.CenterX, transform.CenterY,
            transform.PlayerMarkerScreenAngleDegrees(), transform.DroneActive);

        if (showCoordinates) DrawPlayerCoordinates(
            ui,
            drawList,
            transform);
    }
}

#endif