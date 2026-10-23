#pragma once
#include <array>
#include <cstdint>
#include <cmath>

// Owned numerical values only; no native instance or access capability.
namespace MiniMapBuildingGeometryTypes
{
    using Point=std::array<double,3>;
    struct Identity
    {
        int32_t Index=-1,Serial=0;
        bool operator==(const Identity&) const=default;
    };
    struct Transform
    {
        Point Translation={},Scale={1,1,1};
        std::array<double,4> Rotation={0,0,0,1};
    };
    inline bool Finite(const Point& p)
    {return std::isfinite(p[0])&&std::isfinite(p[1])&&std::isfinite(p[2]);}
    inline bool Valid(const Transform& t)
    {
        double norm=0;for(double q:t.Rotation){if(!std::isfinite(q))return false;norm+=q*q;}
        return Finite(t.Translation)&&Finite(t.Scale)&&std::isfinite(norm)&&std::abs(norm-1)<=1e-6;
    }
    inline bool ToWorld(const Transform& t,const Point& local,Point& out,bool vector=false)
    {
        if(!Valid(t)||!Finite(local))return false;
        Point v;for(size_t i=0;i<3;++i)v[i]=local[i]*t.Scale[i];
        const auto& q=t.Rotation;
        const Point c{q[1]*v[2]-q[2]*v[1],q[2]*v[0]-q[0]*v[2],q[0]*v[1]-q[1]*v[0]};
        const Point d{q[1]*c[2]-q[2]*c[1],q[2]*c[0]-q[0]*c[2],q[0]*c[1]-q[1]*c[0]};
        Point result;for(size_t i=0;i<3;++i)result[i]=v[i]+2*(q[3]*c[i]+d[i])+(vector?0:t.Translation[i]);
        if(!Finite(result))return false;out=result;return true;
    }
}
