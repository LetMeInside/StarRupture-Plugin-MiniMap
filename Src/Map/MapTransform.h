#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include "Map.h"

#include <cmath>

namespace MiniMapMap
{
    struct ScreenPoint
    {
        float X = 0.0f;
        float Y = 0.0f;
    };


    struct Transform
    {
        double PlayerWorldX = 0.0;
        double PlayerWorldY = 0.0;
        double SinYaw = 0.0;
        double CosYaw = 1.0;
        double PixelsPerWorldUnit = 0.0;
        float CenterX = 0.0f;
        float CenterY = 0.0f;
        float Width = 0.0f;
        float Height = 0.0f;
        bool Valid = false;

        bool WorldToScreen(
            double worldX,
            double worldY,
            ScreenPoint& outPoint) const
        {
            outPoint = {};

            if (!Valid ||
                !std::isfinite(worldX) ||
                !std::isfinite(worldY))
            {
                return false;
            }

            const double dx = worldX - PlayerWorldX;
            const double dy = worldY - PlayerWorldY;

            const double screenX =
                static_cast<double>(CenterX) +
                PixelsPerWorldUnit *
                (-SinYaw * dx + CosYaw * dy);

            const double screenY =
                static_cast<double>(CenterY) +
                PixelsPerWorldUnit *
                (-CosYaw * dx - SinYaw * dy);

            if (!std::isfinite(screenX) ||
                !std::isfinite(screenY))
            {
                return false;
            }

            outPoint.X = static_cast<float>(screenX);
            outPoint.Y = static_cast<float>(screenY);
            return true;
        }
    };


    inline bool BuildTransform(
        const PlayerPose& pose,
        double metersPerPixel,
        float viewportX,
        float viewportY,
        float viewportWidth,
        float viewportHeight,
        Transform& outTransform)
    {
        outTransform = {};

        if (!std::isfinite(pose.WorldX) ||
            !std::isfinite(pose.WorldY) ||
            !std::isfinite(pose.ControlYawDegrees) ||
            !std::isfinite(metersPerPixel) ||
            metersPerPixel <= 0.0 ||
            !std::isfinite(viewportX) ||
            !std::isfinite(viewportY) ||
            !std::isfinite(viewportWidth) ||
            !std::isfinite(viewportHeight) ||
            viewportWidth <= 0.0f ||
            viewportHeight <= 0.0f)
        {
            return false;
        }

        constexpr double kDegreesToRadians =
            3.1415926535897932384626433832795 / 180.0;

        const double yawRadians =
            pose.ControlYawDegrees * kDegreesToRadians;

        outTransform.PlayerWorldX = pose.WorldX;
        outTransform.PlayerWorldY = pose.WorldY;
        outTransform.SinYaw = std::sin(yawRadians);
        outTransform.CosYaw = std::cos(yawRadians);
        outTransform.PixelsPerWorldUnit =
            1.0 / (100.0 * metersPerPixel);
        outTransform.CenterX = viewportX + viewportWidth * 0.5f;
        outTransform.CenterY = viewportY + viewportHeight * 0.5f;
        outTransform.Width = viewportWidth;
        outTransform.Height = viewportHeight;
        outTransform.Valid =
            std::isfinite(outTransform.SinYaw) &&
            std::isfinite(outTransform.CosYaw) &&
            std::isfinite(outTransform.PixelsPerWorldUnit) &&
            outTransform.PixelsPerWorldUnit > 0.0;

        return outTransform.Valid;
    }
}

#endif