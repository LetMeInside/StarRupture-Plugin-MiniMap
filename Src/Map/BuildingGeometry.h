#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingCollector.h"
#include "SDK/AuActorPlacement_structs.hpp"
#include <algorithm>
#include <cmath>

namespace MiniMapBuildingGeometry
{
    using Bounds = MiniMapBuildingCollector::Bounds;
    inline bool Intersects(const Bounds& a, const Bounds& b)
    {
        return a.Valid && b.Valid && a.MaxX >= b.MinX && a.MinX <= b.MaxX &&
            a.MaxY >= b.MinY && a.MinY <= b.MaxY;
    }
    inline bool Add(Bounds& b, double x, double y)
    {
        if (!std::isfinite(x) || !std::isfinite(y)) return false;
        if (!b.Valid) b = { x, y, x, y, true };
        else { b.MinX = (std::min)(b.MinX, x); b.MinY = (std::min)(b.MinY, y);
            b.MaxX = (std::max)(b.MaxX, x); b.MaxY = (std::max)(b.MaxY, y); }
        return true;
    }
    // Same verified CL-127004 world-space source and Hermite conversion as
    // Stage A.3, but bounds every segment; no origin/length admission heuristic.
    inline Bounds SplineBounds(const SDK::FAuSplineConnectionFragment& s)
    {
        Bounds b;
        const auto& c = s.SplineCurves.Position;
        if (!s.bSplineCurvesInitialized)
        {
            const auto& p = s.Data.SplineData;
            if (p.Num() != 2 || p.Max() < 2 || !p.GetDataPtr()) return {};
            if (!Add(b, p[0].Position.X, p[0].Position.Y) ||
                !Add(b, p[1].Position.X, p[1].Position.Y) ||
                !Add(b, p[0].Position.X + p[0].Tangent.X / 3, p[0].Position.Y + p[0].Tangent.Y / 3) ||
                !Add(b, p[1].Position.X - p[1].Tangent.X / 3, p[1].Position.Y - p[1].Tangent.Y / 3)) return {};
            return b;
        }
        const auto& p = c.Points;
        if (c.bIsLooped || p.Num() < 2 || p.Num() > 256 || p.Max() < p.Num() || !p.GetDataPtr()) return {};
        for (int i = 0; i + 1 < p.Num(); ++i)
        {
            const auto& a = p[i]; const auto& z = p[i + 1];
            const double dt = double(z.InVal) - a.InVal;
            if (!std::isfinite(dt) || dt <= 0 || !Add(b, a.OutVal.X, a.OutVal.Y) ||
                !Add(b, z.OutVal.X, z.OutVal.Y)) return {};
            switch (a.InterpMode)
            {
            case SDK::EInterpCurveMode::CIM_Constant:
            case SDK::EInterpCurveMode::CIM_Linear: break;
            case SDK::EInterpCurveMode::CIM_CurveAuto:
            case SDK::EInterpCurveMode::CIM_CurveUser:
            case SDK::EInterpCurveMode::CIM_CurveBreak:
            case SDK::EInterpCurveMode::CIM_CurveAutoClamped:
                if (!Add(b, a.OutVal.X + a.LeaveTangent.X * dt / 3, a.OutVal.Y + a.LeaveTangent.Y * dt / 3) ||
                    !Add(b, z.OutVal.X - z.ArriveTangent.X * dt / 3, z.OutVal.Y - z.ArriveTangent.Y * dt / 3)) return {};
                break;
            default: return {};
            }
        }
        return b;
    }
}
#endif
