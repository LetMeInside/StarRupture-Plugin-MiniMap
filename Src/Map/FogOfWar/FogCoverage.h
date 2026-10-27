#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace MiniMapFogCoverage
{
    template<class Read> bool Revealed(double minX,double minY,double maxX,double maxY,
        double pivotX,double pivotY,double scale,int logical,size_t limit,Read read)
    {
        if(!std::isfinite(minX)||!std::isfinite(minY)||!std::isfinite(maxX)||!std::isfinite(maxY)||
            !std::isfinite(scale)||scale<=0||minX>maxX||minY>maxY)return false;
        const double a=std::floor((minX-pivotX)*scale),b=std::floor((minY-pivotY)*scale);
        const double c=std::floor((maxX-pivotX)*scale),d=std::floor((maxY-pivotY)*scale);
        if(!std::isfinite(a)||!std::isfinite(b)||!std::isfinite(c)||!std::isfinite(d)||
            a<0||b<0||c>=logical||d>=logical||(c-a+1)*(d-b+1)>double(limit))return false;
        for(int y=int(b);y<=int(d);++y)for(int x=int(a);x<=int(c);++x)if(!read(x,y))return false;
        return true;
    }
    // A sufficient opacity proof for the overlay's coarse cells. Unknown samples,
    // native interpolation edges and local awareness must all remain drawable.
    template<class Read> bool OpaqueFogCell(int x,int y,bool localFeather,Read read)
    {
        if(localFeather)return false;
        for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx)
        {uint8_t value=0;if(!read(x+dx,y+dy,value)||value!=0)return false;}
        return true;
    }
    template<class Opaque> bool FullyObscured(double minX,double minY,double maxX,double maxY,
        double pivotX,double pivotY,double scale,int logical,size_t limit,Opaque opaque)
    {
        // Reuse the bounded inclusive cell walk; false means opacity was not proven.
        return Revealed(minX,minY,maxX,maxY,pivotX,pivotY,scale,logical,limit,opaque);
    }
}
