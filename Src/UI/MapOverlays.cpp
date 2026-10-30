#include "Services/Viewport.h"
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
        MiniMap::Services::ICanvas& ui,
        MiniMap::Services::DrawLayer drawList,
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
                ui.FilledTriangle(drawList, {{{centerX+a.X, centerY+a.Y}, {centerX+b.X, centerY+b.Y}, {centerX+c.X, centerY+c.Y}}}, kMarkerFillColor);
                ui.Triangle(drawList, {{{centerX+a.X, centerY+a.Y}, {centerX+b.X, centerY+b.Y}, {centerX+c.X, centerY+c.Y}}}, kMarkerOutlineColor, 1.5f);
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

        ui.FilledTriangle(drawList, {{{tipX, tipY}, {rightX, rightY}, {leftX, leftY}}}, kMarkerFillColor);

        ui.Triangle(drawList, {{{tipX, tipY}, {rightX, rightY}, {leftX, leftY}}}, kMarkerOutlineColor, 1.5f);
    }


    void DrawPlayerCoordinates(
        MiniMap::Services::ICanvas& ui,
        MiniMap::Services::DrawLayer drawList,
        const MiniMapMap::Transform& transform, bool coordsInUEUnits)
    {
        char coordinateText[192] = {};
        if (coordsInUEUnits)
            std::snprintf(coordinateText, sizeof(coordinateText),
                "X: %.6f\nY: %.6f\nZ: %.6f",
                transform.PlayerWorldX, transform.PlayerWorldY, transform.PlayerWorldZ);
        else
            std::snprintf(coordinateText, sizeof(coordinateText),
                "X: %.0f m   Y: %.0f m",
                std::round(transform.PlayerWorldX / 100.0),
                std::round(transform.PlayerWorldY / 100.0));

        float textWidth = 0.0f;
        float textHeight = 0.0f;
        { const auto measured = ui.MeasureText(coordinateText, false, 0.0f); textWidth = measured.x; textHeight = measured.y; };

        float compassLineHeight = textHeight;
        if (coordsInUEUnits)
        {
            float unusedWidth = 0.0f;
            { const auto measured = ui.MeasureText("X", false, 0.0f); unusedWidth = measured.x; compassLineHeight = measured.y; };
        }

        const float textX = transform.CenterX - textWidth * 0.5f;
        const float textY = transform.CenterY + transform.Height * 0.5f -
            kCompassInsetPixels - compassLineHeight * 0.5f -
            kCoordinateCompassGapPixels - textHeight;

        ui.Text(drawList, {textX + 1.0f, textY + 1.0f}, kCompassShadowColor, coordinateText);
        ui.Text(drawList, {textX, textY}, kCompassTextColor, coordinateText);
    }


    void DrawCompassLabel(
        MiniMap::Services::ICanvas& ui,
        MiniMap::Services::DrawLayer drawList,
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

        { const auto measured = ui.MeasureText(cardinal.Label, false, 0.0f); textWidth = measured.x; textHeight = measured.y; };

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

        ui.Text(drawList, {textX + 1.0f, textY + 1.0f}, kCompassShadowColor, cardinal.Label);

        ui.Text(drawList, {textX, textY}, kCompassTextColor, cardinal.Label);
    }
}


namespace MiniMapOverlays
{
    void Render(
        MiniMap::Services::ICanvas& ui,
        const MiniMapMap::Transform& transform, bool showCoordinates, bool coordsInUEUnits, bool showCompass,
        const MiniMapConfig::FogOfWarSettings& fog)
    {
        if (!transform.Valid)
        {
            return;
        }

        MiniMap::Services::DrawLayer drawList =
            MiniMap::Services::DrawLayer::Window;

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
                ui.PushClip(drawList, {left, top}, {left+transform.Width, top+transform.Height}, true);
                DrawPlayerMarker(ui, drawList, character.X, character.Y,
                    transform.CharacterWorldYawDegrees + 90.0 + transform.MapRotationDegrees);
                ui.PopClip(drawList);
            }
        }
        DrawPlayerMarker(ui, drawList, transform.CenterX, transform.CenterY,
            transform.PlayerMarkerScreenAngleDegrees(), transform.DroneActive);

        if (showCoordinates) DrawPlayerCoordinates(
            ui,
            drawList,
            transform, coordsInUEUnits);
    }
}

#endif
