#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace MiniMapFloatPixels
{
    inline float Half(uint16_t h)
    {
        const bool negative = (h & 0x8000) != 0;
        const unsigned exponent = (h >> 10) & 31, fraction = h & 1023;
        float value;
        if (exponent == 31) value = fraction ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
        else if (!exponent) value = std::ldexp(float(fraction), -24);
        else value = std::ldexp(float(1024 + fraction), int(exponent) - 25);
        return negative ? -value : value;
    }
    inline bool Size(size_t width, size_t height, size_t stridePixels, size_t bufferHeight, size_t& row, size_t& total, size_t& source)
    {
        constexpr size_t max = (std::numeric_limits<size_t>::max)();
        if (!width || !height || stridePixels < width || bufferHeight < height || stridePixels > max / 8 || width > max / 8) return false;
        row = width * 8;
        if (height > max / row || height > max / (stridePixels * 8)) return false;
        total = height * row;
        source = (height - 1) * stridePixels * 8 + row;
        return true;
    }
    struct Sample { size_t X = 0, Y = 0; std::array<float,4> Rgba{}; };
    struct Stats
    {
        uint64_t Finite = 0, Nonfinite = 0, Foreground = 0, Background = 0, Partial = 0;
        float RgbMin = std::numeric_limits<float>::infinity(), RgbMax = -std::numeric_limits<float>::infinity();
        float AlphaMin = std::numeric_limits<float>::infinity(), AlphaMax = -std::numeric_limits<float>::infinity();
        double Coverage = 0;
        std::array<Sample,7> Samples{};
        size_t SampleCount = 0;
    };
    inline bool Analyze(const uint8_t* data, size_t length, size_t width, size_t height, Stats& stats)
    {
        size_t row, total, source;
        if (!Size(width,height,width,height,row,total,source) || !data || length != total) return false;
        stats = {};
        auto read = [&](size_t x,size_t y) {
            Sample s; s.X=x; s.Y=y;
            for (size_t c=0;c<4;++c) { const auto i=(y*width+x)*8+c*2; s.Rgba[c]=Half(uint16_t(data[i]) | (uint16_t(data[i+1])<<8)); }
            return s;
        };
        const std::array<std::array<size_t,2>,5> fixed{{{0,0},{width-1,0},{0,height-1},{width-1,height-1},{width/2,height/2}}};
        for (const auto& p:fixed) stats.Samples[stats.SampleCount++]=read(p[0],p[1]);
        bool edge=false, foreground=false;
        for (size_t y=0;y<height;++y) for(size_t x=0;x<width;++x)
        {
            const auto s=read(x,y);
            for(size_t c=0;c<4;++c)
            {
                const float f=s.Rgba[c];
                if(!std::isfinite(f)) { ++stats.Nonfinite; continue; }
                ++stats.Finite;
                if(c<3) { stats.RgbMin=(std::min)(stats.RgbMin,f); stats.RgbMax=(std::max)(stats.RgbMax,f); }
                else { stats.AlphaMin=(std::min)(stats.AlphaMin,f); stats.AlphaMax=(std::max)(stats.AlphaMax,f); }
            }
            if(!std::isfinite(s.Rgba[3])) continue;
            const float coverage=1-std::clamp(s.Rgba[3],0.f,1.f);
            stats.Coverage+=coverage;
            if(coverage>=0.99f) { ++stats.Foreground; if(!foreground) {stats.Samples[stats.SampleCount++]=s; foreground=true;} }
            else if(coverage<=0.01f) ++stats.Background;
            else { ++stats.Partial; if(!edge) {stats.Samples[stats.SampleCount++]=s; edge=true;} }
        }
        return true;
    }
    inline bool CopyRows(const uint8_t* mapped, size_t available, size_t width, size_t height, size_t pitch, size_t bufferHeight, std::vector<uint8_t>& output)
    {
        size_t row,total,source;
        if(!mapped || !Size(width,height,pitch,bufferHeight,row,total,source) || available<source) return false;
        output.resize(total);
        for(size_t y=0;y<height;++y) std::memcpy(output.data()+y*row,mapped+y*pitch*8,row);
        return true;
    }
}
