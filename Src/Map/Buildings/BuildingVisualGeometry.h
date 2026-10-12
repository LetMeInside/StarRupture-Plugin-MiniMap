#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingCollector.h"
#include <algorithm>
#include <cmath>

// Pure copied-data operations, shared by preparation, drawing and offline checks.
namespace MiniMapBuildingVisualGeometry
{
    using LocalBounds = MiniMapBuildingCollector::LocalBounds;
    using Record = MiniMapBuildingCollector::Record;
    struct XY { double X=0, Y=0; };
    struct Polygon { std::array<XY,8> Points={}; size_t Count=0; };
    constexpr size_t kMaximumCurvePoints=257;
    constexpr unsigned kMaximumCurveDepth=8;

    inline bool Valid(const LocalBounds& b)
    {
        if(!b.Valid) return false;
        for(size_t i=0;i<3;++i)
            if(!std::isfinite(b.Min[i]) || !std::isfinite(b.Max[i]) || b.Max[i]<b.Min[i] ||
                std::abs(b.Min[i])>10000000 || std::abs(b.Max[i])>10000000) return false;
        return b.Max[0]>b.Min[0] && b.Max[1]>b.Min[1];
    }
    inline double Cross(XY a,XY b,XY c)
    { return (b.X-a.X)*(c.Y-a.Y)-(b.Y-a.Y)*(c.X-a.X); }
    inline Polygon Project(const LocalBounds& b,const Record& r)
    {
        Polygon result;
        if(!Valid(b) || !r.TransformValid) return result;
        double norm=0; for(double q:r.Rotation) { if(!std::isfinite(q)) return result; norm+=q*q; }
        if(!std::isfinite(norm) || std::abs(norm-1)>0.001) return result;
        for(size_t i=0;i<3;++i) if(!std::isfinite(r.Position[i]) || !std::isfinite(r.Scale[i])) return result;
        const double inv=1/std::sqrt(norm);
        const double x=r.Rotation[0]*inv,y=r.Rotation[1]*inv,z=r.Rotation[2]*inv,w=r.Rotation[3]*inv;
        std::array<XY,8> points;
        for(unsigned mask=0;mask<8;++mask)
        {
            std::array<double,3> v;
            for(size_t i=0;i<3;++i) v[i]=((mask&(1u<<i))?b.Max[i]:b.Min[i])*r.Scale[i];
            const double tx=2*(y*v[2]-z*v[1]),ty=2*(z*v[0]-x*v[2]),tz=2*(x*v[1]-y*v[0]);
            points[mask]={r.Position[0]+v[0]+w*tx+y*tz-z*ty,r.Position[1]+v[1]+w*ty+z*tx-x*tz};
            if(!std::isfinite(points[mask].X) || !std::isfinite(points[mask].Y)) return result;
        }
        std::sort(points.begin(),points.end(),[](XY a,XY b){return a.X==b.X?a.Y<b.Y:a.X<b.X;});
        std::array<XY,16> hull; size_t count=0;
        for(auto p:points) { while(count>=2 && Cross(hull[count-2],hull[count-1],p)<=0) --count; hull[count++]=p; }
        const size_t lower=count;
        for(size_t i=points.size()-1;i-->0;)
        { auto p=points[i]; while(count>lower && Cross(hull[count-2],hull[count-1],p)<=0) --count; hull[count++]=p; }
        if(count>1) --count;
        if(count<3 || count>result.Points.size()) return result;
        result.Count=count; std::copy_n(hull.begin(),count,result.Points.begin()); return result;
    }
    inline MiniMapBuildingCollector::Bounds Bounds(const Polygon& p)
    {
        MiniMapBuildingCollector::Bounds b;
        for(size_t i=0;i<p.Count;++i)
        {
            auto v=p.Points[i];
            if(!b.Valid) b={v.X,v.Y,v.X,v.Y,true};
            else { b.MinX=(std::min)(b.MinX,v.X); b.MinY=(std::min)(b.MinY,v.Y);
                b.MaxX=(std::max)(b.MaxX,v.X); b.MaxY=(std::max)(b.MaxY,v.Y); }
        }
        return b;
    }
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
