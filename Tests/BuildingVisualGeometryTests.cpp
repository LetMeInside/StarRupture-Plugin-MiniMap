// Standalone check; compile with /std:c++20 /DMODLOADER_CLIENT_BUILD and the
// same Src, SDK include and generated Client SDK include paths as MiniMap.
#include "Map/BuildingVisualGeometry.h"
#include <cstdlib>
#include <iostream>
#include <limits>

namespace G=MiniMapBuildingVisualGeometry;
namespace C=MiniMapBuildingCollector;
void Check(bool value,const char* message)
{ if(!value) { std::cerr<<message<<'\n'; std::exit(1); } }
bool Near(double a,double b) { return std::abs(a-b)<0.001; }
int main()
{
    C::Record r; r.TransformValid=true; r.Scale={1,1,1}; r.Rotation={0,0,0,1};
    C::LocalBounds box{{-200,-100,0},{200,100,50},true};
    auto shape=G::Project(box,r); auto bounds=G::Bounds(shape);
    Check(shape.Count==4 && Near(bounds.MaxX-bounds.MinX,400) && Near(bounds.MaxY-bounds.MinY,200),"local aspect");
    double area=0; for(size_t i=0;i<shape.Count;++i)
    { auto a=shape.Points[i],b=shape.Points[(i+1)%shape.Count]; area+=a.X*b.Y-a.Y*b.X; }
    Check(area>0,"clockwise winding for ImGui screen Y-down");
    r.Position={1000,2000,0}; r.Scale={-2,3,1}; r.Rotation={0,0,std::sqrt(0.5),std::sqrt(0.5)};
    bounds=G::Bounds(G::Project({{100,200,0},{300,400,50},true},r));
    Check(Near(bounds.MinX,-200) && Near(bounds.MaxX,400) && Near(bounds.MinY,1400) && Near(bounds.MaxY,1800),"signed scale then yaw then translation, with offset center");
    r.Position={0,0,0}; r.Scale={1,1,1}; r.Rotation={std::sqrt(0.5),0,0,std::sqrt(0.5)};
    bounds=G::Bounds(G::Project({{0,0,0},{100,200,100},true},r));
    Check(Near(bounds.MinY,-100) && Near(bounds.MaxY,0),"tilted box projects all eight corners");
    r.Rotation={0,0,0,std::numeric_limits<double>::quiet_NaN()};
    Check(G::Project(box,r).Count==0,"reject invalid quaternion");

    MiniMapMap::PlayerPose pose; MiniMapMap::Transform t;
    Check(MiniMapMap::BuildTransform(pose,0,1,0,0,100,50,t),"map transform");
    MiniMapMap::ScreenPoint p; t.WorldToScreen(100,0,p);
    Check(Near(p.X,51) && Near(p.Y,25),"UU-to-meter scale and +X orientation");
    MiniMapMap::BuildTransform(pose,90,1,0,0,100,50,t); t.WorldToScreen(100,0,p);
    Check(Near(p.X,50) && Near(p.Y,26),"map rotation");
    auto screen=G::ScreenBounds({-200,-100,200,100,true},t);
    Check(Near(screen.MaxX-screen.MinX,2) && Near(screen.MaxY-screen.MinY,4),"map-rotated aspect");
    const C::Bounds viewport{0,0,100,50,true};
    Check(!G::Intersects(G::ScreenBounds({50000,50000,51000,51000,true},t),viewport),"offscreen rejection");
    MiniMapMap::BuildTransform(pose,0,1,0,0,100,50,t);
    t.WorldToScreen(6000,0,p);
    Check(p.X>viewport.MaxX && G::Intersects(G::ScreenBounds({4000,-1000,8000,1000,true},t),viewport),"origin-outside ordinary proxy still overlaps viewport");
    // Both endpoints (and a hypothetical entity origin at the first endpoint)
    // are outside. The curve's control hull still crosses the viewport.
    Check(G::Intersects(G::ScreenBounds({-10000,0,10000,0,true},t),viewport),"origin-outside spline admission");

    const std::array<G::XY,4> curve={G::XY{0,0},{50,100},{100,-100},{150,0}};
    std::array<G::XY,G::kMaximumCurvePoints> points; points[0]=curve[0]; size_t count=1,limited=0;
    G::Subdivide(curve,points,count,limited);
    Check(count>2 && count<=points.size() && limited==0,"adaptive bounded tessellation");
    for(int i=0;i<=1000;++i)
    {
        double u=double(i)/1000,v=1-u;
        G::XY q{v*v*v*curve[0].X+3*v*v*u*curve[1].X+3*v*u*u*curve[2].X+u*u*u*curve[3].X,
            v*v*v*curve[0].Y+3*v*v*u*curve[1].Y+3*v*u*u*curve[2].Y+u*u*u*curve[3].Y};
        double distance=1e100;
        for(size_t j=1;j<count;++j) distance=(std::min)(distance,G::DistanceSquared(q,points[j-1],points[j]));
        Check(distance<=0.76*0.76,"curve approximation exceeds pixel tolerance");
    }
    count=1; points[0]={0,0}; limited=0;
    G::Subdivide({G::XY{0,0},{1000,0},{-1000,0},{1,0}},points,count,limited);
    Check(count>2,"collinear backtracking must not collapse to endpoint chord");
    count=1; points[0]={0,0}; limited=0;
    G::Subdivide({G::XY{0,0},{0,1e12},{1e12,-1e12},{1e12,0}},points,count,limited);
    Check(count<=G::kMaximumCurvePoints && limited>0,"hard subdivision limit");
    std::cout<<"Building visual geometry checks passed\n";
}
