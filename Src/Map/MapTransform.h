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

        double PlayerControlYawDegrees = 0.0;
        double MapRotationDegrees = 0.0;

        double SinMapRotation = 0.0;
        double CosMapRotation = 1.0;

        double PixelsPerWorldUnit = 0.0;

        float CenterX = 0.0f;
        float CenterY = 0.0f;

        float Width = 0.0f;
        float Height = 0.0f;

        bool Valid = false;


        bool DirectionToScreen(
            double worldDirectionX,
            double worldDirectionY,
            ScreenPoint& outDirection) const
        {
            outDirection = {};

            if (!Valid ||
                !std::isfinite(worldDirectionX) ||
                !std::isfinite(worldDirectionY))
            {
                return false;
            }

            const double screenX =
                CosMapRotation *
                worldDirectionX -
                SinMapRotation *
                worldDirectionY;

            const double screenY =
                SinMapRotation *
                worldDirectionX +
                CosMapRotation *
                worldDirectionY;

            if (!std::isfinite(screenX) ||
                !std::isfinite(screenY))
            {
                return false;
            }

            outDirection.X =
                static_cast<float>(
                    screenX);

            outDirection.Y =
                static_cast<float>(
                    screenY);

            return true;
        }


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

            const double dx =
                worldX -
                PlayerWorldX;

            const double dy =
                worldY -
                PlayerWorldY;

            ScreenPoint direction = {};

            if (!DirectionToScreen(
                dx,
                dy,
                direction))
            {
                return false;
            }

            const double screenX =
                static_cast<double>(
                    CenterX) +
                PixelsPerWorldUnit *
                static_cast<double>(
                    direction.X);

            const double screenY =
                static_cast<double>(
                    CenterY) +
                PixelsPerWorldUnit *
                static_cast<double>(
                    direction.Y);

            if (!std::isfinite(screenX) ||
                !std::isfinite(screenY))
            {
                return false;
            }

            outPoint.X =
                static_cast<float>(
                    screenX);

            outPoint.Y =
                static_cast<float>(
                    screenY);

            return true;
        }


        double PlayerMarkerScreenAngleDegrees() const
        {
            // StarRupture's full-map player marker uses Yaw + 90 degrees.
            // Applying the map rotation produces the marker's final screen
            // angle. Heading-up therefore naturally yields zero degrees.
            return
                PlayerControlYawDegrees +
                90.0 +
                MapRotationDegrees;
        }
    };


    inline double HeadingUpMapRotationDegrees(
        double controlYawDegrees)
    {
        return
            -controlYawDegrees -
            90.0;
    }


    inline bool BuildTransform(
        const PlayerPose& pose,
        double mapRotationDegrees,
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
            !std::isfinite(mapRotationDegrees) ||
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
            3.1415926535897932384626433832795 /
            180.0;

        const double mapRotationRadians =
            mapRotationDegrees *
            kDegreesToRadians;

        outTransform.PlayerWorldX =
            pose.WorldX;

        outTransform.PlayerWorldY =
            pose.WorldY;

        outTransform.PlayerControlYawDegrees =
            pose.ControlYawDegrees;

        outTransform.MapRotationDegrees =
            mapRotationDegrees;

        outTransform.SinMapRotation =
            std::sin(
                mapRotationRadians);

        outTransform.CosMapRotation =
            std::cos(
                mapRotationRadians);

        // StarRupture world coordinates use centimeters.
        outTransform.PixelsPerWorldUnit =
            1.0 /
            (100.0 *
                metersPerPixel);

        outTransform.CenterX =
            viewportX +
            viewportWidth *
            0.5f;

        outTransform.CenterY =
            viewportY +
            viewportHeight *
            0.5f;

        outTransform.Width =
            viewportWidth;

        outTransform.Height =
            viewportHeight;

        outTransform.Valid =
            std::isfinite(outTransform.SinMapRotation) &&
            std::isfinite(outTransform.CosMapRotation) &&
            std::isfinite(outTransform.PixelsPerWorldUnit) &&
            outTransform.PixelsPerWorldUnit > 0.0;

        return outTransform.Valid;
    }
}

#endif