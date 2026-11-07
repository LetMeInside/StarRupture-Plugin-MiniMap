#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "Adapters/AlienX/GameFields.h"
#include "BuildingCollector.h"
#include "SDK/AuActorPlacement_structs.hpp"
#include <algorithm>
#include <cmath>

namespace MiniMapBuildingGeometry
{
    using Bounds = MiniMapBuildingCollector::Bounds;
    using Point = MiniMapBuildingCollector::Point;
    using LocalBounds = MiniMapBuildingCollector::LocalBounds;
    using GeometryIssue = MiniMapBuildingCollector::GeometryIssue;
    using CurveSegment = MiniMapBuildingCollector::CurveSegment;
    using SplineGeometry = MiniMapBuildingCollector::SplineGeometry;
    constexpr double kMaximumGeometrySpan = 10000000.0; // 100km sanity guard, NOT gameplay limit.
    inline bool Finite(const Point& p)
    { return std::all_of(p.begin(), p.end(), [](double v) { return std::isfinite(v); }); }
    inline Point Copy(const SDK::FVector& p) { return {p.X, p.Y, p.Z}; }
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
    inline Bounds Union(Bounds a,const Bounds& b)
    {
        if(b.Valid) { Add(a,b.MinX,b.MinY); Add(a,b.MaxX,b.MaxY); }
        return a;
    }
    inline Bounds Inflate(Bounds b,double amount)
    {
        if(b.Valid && std::isfinite(amount) && amount>=0)
        { b.MinX-=amount; b.MinY-=amount; b.MaxX+=amount; b.MaxY+=amount; }
        else b={};
        return b;
    }
    inline Point Midpoint(const Point& a,const Point& b)
    { return {a[0]*0.5+b[0]*0.5,a[1]*0.5+b[1]*0.5,a[2]*0.5+b[2]*0.5}; }
    inline void CurveHull(Bounds& b,const std::array<Point,4>& p,unsigned depth)
    {
        if(!depth) { for(const auto& v:p) Add(b,v[0],v[1]); return; }
        const auto a=Midpoint(p[0],p[1]),c=Midpoint(p[1],p[2]),d=Midpoint(p[2],p[3]);
        const auto e=Midpoint(a,c),f=Midpoint(c,d),m=Midpoint(e,f);
        CurveHull(b,{p[0],a,e,m},depth-1); CurveHull(b,{m,f,d,p[3]},depth-1);
    }
    // The authored points of a spline data block, and point `index` of them as a
    // position and a tangent. Both go through the map: null and false when a
    // member did not resolve on the running build, or the index is outside.
    inline const MiniMapFields::RawTArray* AuthoredPoints(const SDK::FAuAPSplineData& data)
    { return MiniMapFields::SplineDataPoints.In(&data); }
    inline bool AuthoredPoint(const MiniMapFields::RawTArray& points,int32_t index,Point& position,Point& tangent)
    {
        const auto* element=PluginMap::ElementAt(points.Data,points.Num,index,MiniMapFields::SplinePointSize);
        const auto* p=MiniMapFields::SplinePointPosition.In(element);
        const auto* t=MiniMapFields::SplinePointTangent.In(element);
        if(!p || !t)return false;
        position=Copy(*p);tangent=Copy(*t);return true;
    }
    // True when the fragment carries a spline of its own: the curves are built, or
    // more than one point is authored.
    inline bool HasOwnSpline(const SDK::FAuSplineConnectionFragment& s)
    {
        const auto* built=MiniMapFields::SplineFragmentBuilt.In(&s);
        const auto* data=MiniMapFields::SplineFragmentData.In(&s);
        const auto* points=data?AuthoredPoints(*data):nullptr;
        return (built && *built) || (points && points->Num>1);
    }
    inline void CopySpline(const SDK::FAuSplineConnectionFragment& s,SplineGeometry& out)
    {
        out.Segments.clear(); out.CenterlineBounds={}; out.Issue=GeometryIssue::None;
        auto fail=[&](GeometryIssue issue) { out.Issue=issue; out.Segments.clear(); out.CenterlineBounds={}; };
        auto append=[&](CurveSegment segment)
        {
            LocalBounds range; range.Valid=true; range.Min=range.Max=segment.Controls[0];
            for(const auto& p:segment.Controls)
            {
                if(!Finite(p)) { fail(GeometryIssue::NonFinite); return false; }
                for(size_t axis=0;axis<3;++axis)
                { range.Min[axis]=(std::min)(range.Min[axis],p[axis]); range.Max[axis]=(std::max)(range.Max[axis],p[axis]); }
            }
            for(size_t axis=0;axis<3;++axis)
                if(range.Max[axis]-range.Min[axis]>kMaximumGeometrySpan) { fail(GeometryIssue::Oversized); return false; }
            // Four subdivided control hulls remain conservative, while avoiding
            // the very loose original tangent hull. No dense polyline allocation.
            CurveHull(out.CenterlineBounds,segment.Controls,2);
            out.Segments.push_back(segment); return true;
        };
        const auto* built=MiniMapFields::SplineFragmentBuilt.In(&s);
        if(!built) { fail(GeometryIssue::InvalidArray); return; }
        if(!*built)
        {
            const auto* data=MiniMapFields::SplineFragmentData.In(&s);
            const auto* p=data?AuthoredPoints(*data):nullptr;
            if(!p) { fail(GeometryIssue::InvalidArray); return; }
            if(p->Num!=2) { fail(GeometryIssue::Uninitialized); return; }
            if(p->Max<p->Num || !p->Data) { fail(GeometryIssue::InvalidArray); return; }
            Point p0,t0,p1,t1;
            if(!AuthoredPoint(*p,0,p0,t0) || !AuthoredPoint(*p,1,p1,t1)) { fail(GeometryIssue::InvalidArray); return; }
            CurveSegment segment; segment.Mode=uint8_t(SDK::EInterpCurveMode::CIM_CurveUser);
            segment.Controls[0]=p0; segment.Controls[3]=p1;
            const auto a=t0,b=t1;
            for(size_t axis=0;axis<3;++axis)
            { segment.Controls[1][axis]=segment.Controls[0][axis]+a[axis]/3; segment.Controls[2][axis]=segment.Controls[3][axis]-b[axis]/3; }
            if(!append(segment)) return;
        }
        else
        {
            const auto* curves=MiniMapFields::SplineFragmentCurves.In(&s);
            if(!curves) { fail(GeometryIssue::InvalidArray); return; }
            const auto& c=curves->Position; const auto& p=c.Points;
            if(c.bIsLooped) { fail(GeometryIssue::Looped); return; }
            if(p.Num()<2 || p.Max()<p.Num() || !p.GetDataPtr()) { fail(GeometryIssue::InvalidArray); return; }
            for(int i=0;i+1<p.Num();++i)
            {
                const auto& a=p[i]; const auto& b=p[i+1];
                const double dt=double(b.InVal)-a.InVal;
                if(!std::isfinite(dt) || dt<=0) { fail(GeometryIssue::InvalidCurve); return; }
                CurveSegment segment; segment.Mode=uint8_t(a.InterpMode);
                segment.StartKey=a.InVal; segment.EndKey=b.InVal;
                segment.Controls[0]=Copy(a.OutVal); segment.Controls[3]=Copy(b.OutVal);
                switch(a.InterpMode)
                {
                case SDK::EInterpCurveMode::CIM_Constant:
                    segment.Controls[1]=segment.Controls[0]; segment.Controls[2]=segment.Controls[3]; break;
                case SDK::EInterpCurveMode::CIM_Linear:
                    for(size_t axis=0;axis<3;++axis)
                    { const double delta=segment.Controls[3][axis]-segment.Controls[0][axis];
                      segment.Controls[1][axis]=segment.Controls[0][axis]+delta/3;
                      segment.Controls[2][axis]=segment.Controls[0][axis]+delta*2/3; }
                    break;
                case SDK::EInterpCurveMode::CIM_CurveAuto:
                case SDK::EInterpCurveMode::CIM_CurveUser:
                case SDK::EInterpCurveMode::CIM_CurveBreak:
                case SDK::EInterpCurveMode::CIM_CurveAutoClamped:
                    for(size_t axis=0;axis<3;++axis)
                    { segment.Controls[1][axis]=segment.Controls[0][axis]+Copy(a.LeaveTangent)[axis]*dt/3;
                      segment.Controls[2][axis]=segment.Controls[3][axis]-Copy(b.ArriveTangent)[axis]*dt/3; }
                    break;
                default: fail(GeometryIssue::UnsupportedMode); return;
                }
                if(!append(segment)) return;
            }
        }
        if(out.CenterlineBounds.MaxX-out.CenterlineBounds.MinX>kMaximumGeometrySpan ||
            out.CenterlineBounds.MaxY-out.CenterlineBounds.MinY>kMaximumGeometrySpan)
        { fail(GeometryIssue::Oversized); return; }
        out.CenterlineBounds=Inflate(out.CenterlineBounds,0.01);
    }
}
#endif
