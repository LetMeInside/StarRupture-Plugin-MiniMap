#include "../Src/Experiments/CapturePng.h"
#include <cassert>
#include <cstdio>
int main(int argc,char** argv)
{
    using namespace MiniMapCapturePng;
    constexpr size_t w=3,h=2,pitch=5;
    std::vector<uint8_t> raw(pitch*h*8,0xff);
    auto pixel=[&](size_t x,size_t y,uint16_t r,uint16_t g,uint16_t b,uint16_t a)
    {
        const uint16_t v[]{r,g,b,a};for(size_t k=0;k<4;++k)
        {raw[(y*pitch+x)*8+k*2]=uint8_t(v[k]);raw[(y*pitch+x)*8+k*2+1]=uint8_t(v[k]>>8);}
    };
    pixel(0,0,0x3c00,0,0,0); // linear red=1 -> mapped .5 -> sRGB 188
    pixel(1,0,0,0x4400,0,0x3800); // HDR green=4, inverse opacity .5
    pixel(2,0,0,0,0x3c00,0x4000); // out-of-range inverse alpha -> transparent
    pixel(0,1,0x7e00,0x7c00,0xbc00,0); // nonfinite/negative -> black
    pixel(1,1,0,0,0,0x7e00); // invalid alpha -> transparent
    pixel(2,1,0x7bff,0x3c00,0,0xbc00); // huge finite, negative inverse alpha
    Image image;
    assert(Convert(raw.data(),raw.size(),w,h,pitch,h,image));
    assert(image.Rgba.size()==24&&image.Mask.size()==6&&image.Sanitized==3);
    assert(image.Rgba[0]==188&&image.Rgba[3]==255);
    assert(image.Rgba[5]==231&&image.Rgba[7]==128);
    assert(image.Rgba[10]==0&&image.Rgba[11]==0);
    assert(image.Rgba[12]==0&&image.Rgba[13]==0&&image.Rgba[14]==0);
    assert(image.Rgba[19]==0&&image.Rgba[20]==255&&image.Rgba[23]==255);
    assert(!Convert(raw.data(),raw.size(),w,h,2,h,image));
    assert(!Convert(raw.data(),8,w,h,pitch,h,image));
    assert(!Convert(raw.data(),raw.size(),4097,h,pitch,h,image));
    assert(!Convert(nullptr,raw.size(),w,h,pitch,h,image));
    assert(argc==2);const auto dir=std::filesystem::path(argv[1]);
    std::filesystem::create_directories(dir);
    const auto encoded=Write(dir/L"Conversion.png",w,h,image.Rgba,false);
    if(!encoded.Ok)fprintf(stderr,"WIC failure: %s 0x%08lx\n",encoded.Stage,(unsigned long)encoded.Error);
    assert(encoded.Ok);
    assert(!Write(dir/L"Conversion.png"/L"child.png",w,h,image.Rgba,false).Ok);
    assert(Write(dir/L"Alpha.png",w,h,image.Mask,true).Ok);
    std::vector<uint8_t> full(256*256*8,0);
    assert(Convert(full.data(),full.size(),256,256,256,256,image));
    assert(Write(dir/L"Dimensions.png",256,256,image.Rgba,false).Ok);
    assert(!Write(dir/L"Invalid.png",256,256,image.Mask,false).Ok);
    puts("Capture PNG tests passed: inversion, HDR/sRGB, pitched rows, nonfinite values, bounds, WIC export, dimensions");
}
