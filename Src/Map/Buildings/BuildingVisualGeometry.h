#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingCollector.h"
#include <algorithm>
#include <cmath>

// Pure copied-data operations, shared by preparation, drawing and offline checks.
namespace MiniMapBuildingVisualGeometry
{
    struct XY { double X=0, Y=0; };
    constexpr size_t kMaximumCurvePoints=257;
    constexpr unsigned kMaximumCurveDepth=8;

    inline bool Intersects(const MiniMapBuildingCollector::Bounds& a,const MiniMapBuildingCollector::Bounds& b)
    { return a.Valid && b.Valid && a.MaxX>=b.MinX && a.MinX<=b.MaxX && a.MaxY>=b.MinY && a.MinY<=b.MaxY; }
    inline MiniMapBuildingCollector::Bounds ScreenBounds(const MiniMapBuildingCollector::Bounds& b,const MiniMapMap::Transform& t)
    {
        MiniMapBuildingCollector::Bounds out;
        if(!b.Valid) return out;
        for(unsigned i=0;i<4;++i)
        {
            MiniMapMap::ScreenPoint p;
            if(!t.WorldToScreen((i&1)?b.MaxX:b.MinX,(i&2)?b.MaxY:b.MinY,p)) return {};
            if(!out.Valid) out={p.X,p.Y,p.X,p.Y,true};
            else { out.MinX=(std::min)(out.MinX,double(p.X)); out.MinY=(std::min)(out.MinY,double(p.Y));
                out.MaxX=(std::max)(out.MaxX,double(p.X)); out.MaxY=(std::max)(out.MaxY,double(p.Y)); }
        }
        return out;
    }
    inline XY Mid(XY a,XY b) { return {(a.X+b.X)*0.5,(a.Y+b.Y)*0.5}; }
    inline double DistanceSquared(XY p,XY a,XY b)
    {
        const double dx=b.X-a.X,dy=b.Y-a.Y,length=dx*dx+dy*dy;
        const double t=length>0?std::clamp(((p.X-a.X)*dx+(p.Y-a.Y)*dy)/length,0.0,1.0):0;
        const double x=p.X-(a.X+t*dx),y=p.Y-(a.Y+t*dy); return x*x+y*y;
    }
    // Error is measured against the finite screen-space chord, including backtracking.
    inline void Subdivide(const std::array<XY,4>& p,std::array<XY,kMaximumCurvePoints>& out,
        size_t& count,size_t& limited,unsigned depth=0)
    {
        const bool flat=(std::max)(DistanceSquared(p[1],p[0],p[3]),DistanceSquared(p[2],p[0],p[3]))<=0.75*0.75;
        if(flat || depth==kMaximumCurveDepth)
        { limited+=!flat; if(count<out.size()) out[count++]=p[3]; return; }
        const auto a=Mid(p[0],p[1]),b=Mid(p[1],p[2]),c=Mid(p[2],p[3]),d=Mid(a,b),e=Mid(b,c),m=Mid(d,e);
        Subdivide({p[0],a,d,m},out,count,limited,depth+1);
        Subdivide({m,e,c,p[3]},out,count,limited,depth+1);
    }
}
#endif
