#include "../Src/Experiments/FloatReadbackPixels.h"
#include <cassert>
#include <cstdio>
using namespace MiniMapFloatPixels;
int main()
{
 assert(Half(0)==0 && std::signbit(Half(0x8000)));
 assert(Half(0x3c00)==1 && Half(0xc000)==-2 && Half(0x7bff)==65504);
 assert(Half(1)==std::ldexp(1.f,-24) && Half(0x400)==std::ldexp(1.f,-14));
 assert(std::isinf(Half(0x7c00)) && std::isnan(Half(0x7e00)));
 size_t row,total,source;
 assert(!Size(0,1,1,1,row,total,source));
 assert(!Size(37,23,36,23,row,total,source));
 assert(!Size(37,23,64,22,row,total,source));
 assert(!Size(SIZE_MAX,2,SIZE_MAX,2,row,total,source));
 std::vector<uint8_t> padded(64*23*8,0xee), copied;
 for(size_t y=0;y<23;++y) for(size_t x=0;x<37;++x) for(size_t c=0;c<4;++c)
 { const uint16_t h=c==3 ? (x<13?0:0x3c00) : 0x3c00; const size_t i=(y*64+x)*8+c*2; padded[i]=uint8_t(h);padded[i+1]=uint8_t(h>>8); }
 assert(CopyRows(padded.data(),padded.size(),37,23,64,23,copied));
 assert(copied.size()==37*23*8);
 Stats stats;assert(Analyze(copied.data(),copied.size(),37,23,stats));
 assert(stats.Finite==37*23*4 && !stats.Nonfinite && stats.Foreground==13*23 && stats.Background==24*23);
 assert(stats.Coverage==13*23 && stats.RgbMin==1 && stats.RgbMax==1);
 assert(!CopyRows(padded.data(),1,37,23,64,23,copied));
 copied[0]=0; copied[1]=0x7c;assert(Analyze(copied.data(),copied.size(),37,23,stats) && stats.Nonfinite==1);
 puts("Float readback tests passed: binary16 specials, pitched rows, bounds/overflow, alpha statistics");
}
