#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingVisualGeometry.h"
#include "Map/Sprites/SpriteStatus.h"
namespace MiniMapBuildingSpriteGeometry
{
    namespace C=MiniMapBuildingCollector;
    namespace G=MiniMapBuildingVisualGeometry;
    inline bool BuildWorldQuad(const C::Record& record, const MiniMapSpriteStatus::AtlasEntry& entry,
        std::array<G::XY,4>& corners)
    {
        if (!record.TransformValid) return false;
        for (double value:entry.Region) if (!std::isfinite(value) || value<0 || value>1) return false;
        if (!(entry.Region[2]>entry.Region[0] && entry.Region[3]>entry.Region[1])) return false;
        double norm=0;
        for (double q:record.Rotation) { if (!std::isfinite(q)) return false; norm+=q*q; }
        if (!std::isfinite(norm) || !(norm>0)) return false;
        const double inverse=1/std::sqrt(norm);
        const double x=record.Rotation[0]*inverse,y=record.Rotation[1]*inverse,
            z=record.Rotation[2]*inverse,w=record.Rotation[3]*inverse;
        // Apply the copied transform, including tilt and signed scale. Only a
        // nonfinite/degenerate projected quad is unrepresentable; these are not
        // additional building-instance eligibility rules.
        std::array<double,3> center{},extent{};
        for (size_t axis=0;axis<3;++axis)
        {
            if (!std::isfinite(record.Position[axis]) || !std::isfinite(record.Scale[axis]) ||
                !std::isfinite(entry.Min[axis]) || !std::isfinite(entry.Max[axis]) ||
                entry.Max[axis]<entry.Min[axis]) return false;
            center[axis]=entry.Min[axis]*0.5+entry.Max[axis]*0.5;
            extent[axis]=entry.Max[axis]*0.5-entry.Min[axis]*0.5;
        }
        if (!(extent[0]>0 && extent[1]>0)) return false;
        // Match the float OrthoWidth actually assigned by ConfigureSceneCapture.
        const double halfWidth=double(float(2*(std::max)(extent[0],extent[1])*1.1))*0.5;
        if (!std::isfinite(halfWidth) || !(halfWidth>0)) return false;
        const auto& rect=entry.SourceRect;
        for (double value:rect) if (!std::isfinite(value)) return false;
        if (rect[0]<0 || rect[1]<0 || rect[0]>=256 || rect[1]>=256 || rect[2]<=rect[0] || rect[3]<=rect[1]) return false;
        const double left=rect[0],top=rect[1],right=rect[2],bottom=rect[3];
        const std::array<std::array<double,2>,4> source{{{left,top},{right,top},{right,bottom},{left,bottom}}};
        for (size_t i=0;i<source.size();++i)
        {
            // Capture rotation (-90,-90,0): image right=+X, image down=+Y.
            // Source pixel BOUNDARIES, not atlas UVs; keep asymmetric crop and
            // Actor-origin offset. FTransform applies signed scale before rotation.
            const std::array<double,3> v{{(center[0]+halfWidth*(2*source[i][0]/256-1))*record.Scale[0],
                (center[1]+halfWidth*(2*source[i][1]/256-1))*record.Scale[1],center[2]*record.Scale[2]}};
            for (double value:v) if (!std::isfinite(value)) return false;
            const double tx=2*(y*v[2]-z*v[1]),ty=2*(z*v[0]-x*v[2]),tz=2*(x*v[1]-y*v[0]);
            const std::array<double,3> world{{record.Position[0]+v[0]+w*tx+y*tz-z*ty,
                record.Position[1]+v[1]+w*ty+z*tx-x*tz,record.Position[2]+v[2]+w*tz+x*ty-y*tx}};
            for (double value:world) if (!std::isfinite(value)) return false;
            corners[i]={world[0],world[1]};
        }
        return true;
    }
}
#endif
