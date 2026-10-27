#pragma once
#include "DiagnosticExports.h"
#include "Native/FloatReadbackPixels.h"
#include "PluginPaths.h"
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <filesystem>
#include <string>
#include <utility>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace MiniMapCapturePng
{
    inline uint8_t Byte(float v) { return uint8_t(std::lround(std::clamp(v,0.f,1.f)*255.f)); }
    // FinalColorLDR in a non-sRGB PF_FloatRGBA target with TargetGamma=2.2:
    // Unreal has already applied its tone curve and output transfer function.
    // These are RGBA half-floats in channel order, not BGRA8 or scene-linear RGB.
    inline bool FinalColorRgb(const std::vector<uint8_t>& pixels,size_t w,size_t h,std::vector<uint8_t>& out)
    {
        size_t row,total,source;
        if(w>4096 || h>4096 || !MiniMapFloatPixels::Size(w,h,w,h,row,total,source) || pixels.size()!=total) return false;
        std::vector<uint8_t> rgb(w*h*3);
        for(size_t i=0;i<w*h;++i) for(size_t k=0;k<3;++k)
        {
            const size_t p=i*8+k*2;
            const float c=MiniMapFloatPixels::Half(uint16_t(pixels[p])|(uint16_t(pixels[p+1])<<8));
            if(!std::isfinite(c)) return false;
            rgb[i*3+k]=Byte(c); // Clamp/quantize only: no Reinhard, gamma or unassociation.
        }
        out=std::move(rgb);
        return true;
    }
    inline bool HdrCoverage(const std::vector<uint8_t>& hdr,size_t w,size_t h,std::vector<uint8_t>& out)
    {
        size_t row,total,source;
        if(w>4096 || h>4096 || !MiniMapFloatPixels::Size(w,h,w,h,row,total,source) || hdr.size()!=total) return false;
        std::vector<uint8_t> coverage(w*h);
        for(size_t i=0;i<w*h;++i)
        {
            const size_t p=i*8+6;
            const float inverseOpacity=MiniMapFloatPixels::Half(uint16_t(hdr[p])|(uint16_t(hdr[p+1])<<8));
            if(!std::isfinite(inverseOpacity)) return false;
            coverage[i]=Byte(1.f-std::clamp(inverseOpacity,0.f,1.f));
        }
        out=std::move(coverage); // Independent of the native Job's pixel lifetime.
        return true;
    }
    inline bool CombineFinalColorCoverage(const std::vector<uint8_t>& rgb,const std::vector<uint8_t>& coverage,
        size_t w,size_t h,std::vector<uint8_t>& out)
    {
        if(!w || !h || w>4096 || h>4096 || coverage.size()!=w*h || rgb.size()!=w*h*3) return false;
        std::vector<uint8_t> rgba(w*h*4);
        for(size_t i=0;i<w*h;++i)
        {
            const auto alpha=coverage[i];
            rgba[i*4+3]=alpha;
            // Accepted experiment: fractional-alpha RGB retains the final capture's
            // background contribution. No inverse tone map or divide by coverage.
            if(alpha) for(size_t k=0;k<3;++k) rgba[i*4+k]=rgb[i*3+k];
        }
        out=std::move(rgba);
        return true;
    }
    // Sprite processing below consumes final display RGBA8.
    struct ProcessedImage
    {
        std::vector<uint8_t> Rgba;
        size_t X0{},Y0{},X1{},Y1{},Width{},Height{};
        double Ratio=1,SourceStep=1;
        bool Ok=false,Empty=false,Insufficient=false,Passthrough=false;
        const char* Reason="invalid_processing_input";
    };
    inline ProcessedImage Process(const std::vector<uint8_t>& rgba,size_t width,size_t height,
        double orthoExtent,double pixelsPerWorldUnit) noexcept
    {
        ProcessedImage out;
        try
        {
            // Bounded square capture canvas; not an atlas cell or final sprite size.
            if(!width||width>256||height!=width||rgba.size()!=width*height*4||
                !std::isfinite(orthoExtent)||orthoExtent<=0||
                !std::isfinite(pixelsPerWorldUnit)||pixelsPerWorldUnit<=0)return out;
            out.X0=width;out.Y0=height;
            for(size_t y=0;y<height;++y)for(size_t x=0;x<width;++x)
                if(rgba[(y*width+x)*4+3])
                {out.X0=(std::min)(out.X0,x);out.Y0=(std::min)(out.Y0,y);
                 out.X1=(std::max)(out.X1,x+1);out.Y1=(std::max)(out.Y1,y+1);}
            if(!out.X1){out.Empty=true;out.Reason="empty_alpha_capture";return out;}
            // Move the atlas's former one-source-pixel transparent guard here,
            // before filtering. The atlas will only copy/pack processed pixels.
            if(out.X0) --out.X0;
            if(out.Y0) --out.Y0;
            if(out.X1<width) ++out.X1;
            if(out.Y1<height) ++out.Y1;
            const double requestedRatio=(orthoExtent/double(width))*pixelsPerWorldUnit;
            if(!std::isfinite(requestedRatio)||requestedRatio<=0)return out;
            out.Insufficient=requestedRatio>1;out.Ratio=(std::min)(1.0,requestedRatio);
            out.SourceStep=1/out.Ratio;
            const double area=out.SourceStep*out.SourceStep;
            if(!std::isfinite(area)||area<=0){out.Reason="invalid_sampling_footprint";return out;}
            out.Width=size_t(std::ceil(double(out.X1-out.X0)*out.Ratio));
            out.Height=size_t(std::ceil(double(out.Y1-out.Y0)*out.Ratio));
            if(!out.Width||!out.Height||out.Width>width||out.Height>height)return out;
            if(out.Ratio==1&&out.X0==0&&out.Y0==0&&out.X1==width&&out.Y1==height)
            {out.Ok=true;out.Passthrough=true;out.Reason="original_sampling_retained";return out;}
            out.Rgba.resize(out.Width*out.Height*4);
            if(out.Ratio==1)
            {
                for(size_t y=0;y<out.Height;++y)
                    std::memcpy(out.Rgba.data()+y*out.Width*4,rgba.data()+((out.Y0+y)*width+out.X0)*4,out.Width*4);
            }
            else
            {
                std::array<double,256> linear{};
                for(size_t i=0;i<linear.size();++i)
                {const double v=double(i)/255;linear[i]=v<=0.04045?v/12.92:std::pow((v+0.055)/1.055,2.4);}
                auto encode=[](double v)
                {v=std::clamp(v,0.0,1.0);return uint8_t(std::lround(255*(v<=0.0031308?12.92*v:1.055*std::pow(v,1/2.4)-0.055)));};
                for(size_t y=0;y<out.Height;++y)for(size_t x=0;x<out.Width;++x)
                {
                    const double left=out.X0+x*out.SourceStep,top=out.Y0+y*out.SourceStep;
                    const double right=(std::min)(double(out.X1),left+out.SourceStep);
                    const double bottom=(std::min)(double(out.Y1),top+out.SourceStep);
                    double alpha=0;std::array<double,3> color{};
                    if(left<right&&top<bottom)
                        for(size_t sy=size_t(std::floor(top));sy<size_t(std::ceil(bottom));++sy)
                            for(size_t sx=size_t(std::floor(left));sx<size_t(std::ceil(right));++sx)
                            {
                                const double weight=((std::min)(right,double(sx+1))-(std::max)(left,double(sx)))*
                                    ((std::min)(bottom,double(sy+1))-(std::max)(top,double(sy)));
                                const auto* pixel=rgba.data()+(sy*width+sx)*4;
                                const double covered=weight*double(pixel[3])/255;alpha+=covered;
                                for(size_t k=0;k<3;++k)color[k]+=covered*linear[pixel[k]];
                            }
                    auto* pixel=out.Rgba.data()+(y*out.Width+x)*4;
                    // The denominator includes transparent rounding margins outside the crop.
                    pixel[3]=uint8_t(std::lround(255*std::clamp(alpha/area,0.0,1.0)));
                    if(pixel[3]&&alpha>0)for(size_t k=0;k<3;++k)pixel[k]=encode(color[k]/alpha);
                }
            }
            bool coverage=false;for(size_t i=3;i<out.Rgba.size();i+=4)coverage|=out.Rgba[i]!=0;
            if(!coverage){out.Reason="empty_processed_alpha";return out;}
            out.Ok=true;out.Reason="cropped_spatial_image";return out;
        }
        catch(...){out.Ok=false;out.Reason="processing_exception";return out;}
    }
    struct WriteResult { bool Ok=false; uintmax_t Bytes=0; HRESULT Error=S_OK; const char* Stage=""; };
    // Exactly one WIC encoder path. A null path extracts into owned memory;
    // a file path preserves synchronous diagnostic export without a durability flush.
    inline WriteResult EncodeOrWrite(const std::filesystem::path* path,UINT w,UINT h,
        const std::vector<uint8_t>& pixels,bool gray,std::vector<uint8_t>* memory)
    {
        WriteResult r;
        const size_t channels=gray?1:4;
        if(!w||!h||w>4096||h>4096||pixels.size()!=size_t(w)*h*channels)
            return {false,0,E_INVALIDARG,"dimensions/byte count"};
        const HRESULT init=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        if(FAILED(init)&&init!=RPC_E_CHANGED_MODE)return {false,0,init,"COM initialization"};
        struct ComScope {bool Own;~ComScope(){if(Own)CoUninitialize();}} scope{SUCCEEDED(init)};
        using Microsoft::WRL::ComPtr;
        ComPtr<IWICImagingFactory> factory;ComPtr<IStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;ComPtr<IWICBitmapFrameEncode> frame;
        auto step=[&](HRESULT hr,const char* stage){r.Error=hr;r.Stage=stage;return SUCCEEDED(hr);};
        if(!step(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"WIC factory"))return r;
        // The HGLOBAL stream owns its allocation (delete-on-release). Nothing
        // borrowed from it escapes: memory output is copied before COM teardown.
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        struct FileScope
        {
            HANDLE Value=INVALID_HANDLE_VALUE;
            ~FileScope(){if(Value!=INVALID_HANDLE_VALUE)CloseHandle(Value);}
            bool Close(){const auto value=Value;Value=INVALID_HANDLE_VALUE;return CloseHandle(value)!=FALSE;}
        } file;
        if(path)
        {
            std::filesystem::path outputPath;
            if(!MiniMapPluginPaths::Extended(*path,outputPath))return {false,0,E_INVALIDARG,"absolute output path"};
            file.Value=CreateFileW(outputPath.c_str(),GENERIC_WRITE|FILE_READ_ATTRIBUTES,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(file.Value==INVALID_HANDLE_VALUE)return {false,0,HRESULT_FROM_WIN32(GetLastError()),"open output"};
        }
#endif
        if(!step(CreateStreamOnHGlobal(nullptr,TRUE,&stream),"PNG memory stream")||
            !step(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder),"PNG encoder")||
            !step(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"encoder initialization")||
            !step(encoder->CreateNewFrame(&frame,nullptr),"PNG frame")||
            !step(frame->Initialize(nullptr),"frame initialization")||
            !step(frame->SetSize(w,h),"frame size"))return r;
        // Windows PNG encoder accepts BGRA; keep the pipeline RGBA and swap
        // only its bounded encoder input buffer. PNG itself retains RGBA alpha.
        std::vector<uint8_t> bgra;
        if(!gray){bgra=pixels;for(size_t i=0;i<bgra.size();i+=4)std::swap(bgra[i],bgra[i+2]);}
        const auto& encoded=gray?pixels:bgra;
        WICPixelFormatGUID format=gray?GUID_WICPixelFormat8bppGray:GUID_WICPixelFormat32bppBGRA;
        const auto requested=format;
        if(!step(frame->SetPixelFormat(&format),"pixel format"))return r;
        if(!IsEqualGUID(format,requested))return {false,0,E_FAIL,"unexpected WIC format conversion"};
        if(!step(frame->WritePixels(h,w*UINT(channels),UINT(encoded.size()),const_cast<BYTE*>(encoded.data())),"pixels")||
            !step(frame->Commit(),"frame commit")||!step(encoder->Commit(),"PNG commit"))return r;
        frame.Reset();encoder.Reset();
        STATSTG encodedInfo{};
        if(!step(stream->Stat(&encodedInfo,STATFLAG_NONAME),"encoded PNG size"))return r;
        LARGE_INTEGER beginning{};
        if(!step(stream->Seek(beginning,STREAM_SEEK_SET,nullptr),"encoded PNG rewind"))return r;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        if(!path)
#endif
        {
            // Match the cache payload bound before allocating; output is unchanged
            // on failure. Read copies bytes, independent of stream/HGLOBAL lifetime.
            if(!memory || !encodedInfo.cbSize.QuadPart || encodedInfo.cbSize.QuadPart>128u*1024u*1024u)
                return {false,0,E_INVALIDARG,"encoded PNG length"};
            std::vector<uint8_t> bytes(static_cast<size_t>(encodedInfo.cbSize.QuadPart));
            ULONG read=0;
            if(!step(stream->Read(bytes.data(),ULONG(bytes.size()),&read),"read encoded PNG"))return r;
            if(read!=bytes.size())return {false,0,HRESULT_FROM_WIN32(ERROR_HANDLE_EOF),"read encoded PNG length"};
            memory->swap(bytes);
        }
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        else
        {
            std::array<BYTE,16384> buffer;
            auto remaining=encodedInfo.cbSize.QuadPart;
            while(remaining)
            {
                const auto count=ULONG((std::min)(remaining,static_cast<ULONGLONG>(buffer.size())));
                ULONG read=0;
                if(!step(stream->Read(buffer.data(),count,&read),"read encoded PNG"))return r;
                if(read!=count)return {false,0,HRESULT_FROM_WIN32(ERROR_HANDLE_EOF),"read encoded PNG length"};
                DWORD written=0;
                if(!WriteFile(file.Value,buffer.data(),read,&written,nullptr))
                    return {false,0,HRESULT_FROM_WIN32(GetLastError()),"write output"};
                if(written!=read)return {false,0,HRESULT_FROM_WIN32(ERROR_WRITE_FAULT),"write output length"};
                remaining-=read;
            }
            // Diagnostics require complete synchronous writes, not power-loss
            // durability. Size-check and close before the caller publishes/logs it.
            LARGE_INTEGER length{};
            if(!GetFileSizeEx(file.Value,&length))return {false,0,HRESULT_FROM_WIN32(GetLastError()),"file size"};
            if(length.QuadPart<0 || ULONGLONG(length.QuadPart)!=encodedInfo.cbSize.QuadPart)
                return {false,0,HRESULT_FROM_WIN32(ERROR_INVALID_DATA),"file size mismatch"};
            if(!file.Close())return {false,0,HRESULT_FROM_WIN32(GetLastError()),"close output"};
        }
#endif
        r.Bytes=uintmax_t(encodedInfo.cbSize.QuadPart);
        r.Ok=true;return r;
    }
    inline WriteResult Encode(UINT w,UINT h,const std::vector<uint8_t>& pixels,bool gray,std::vector<uint8_t>& bytes)
    {return EncodeOrWrite(nullptr,w,h,pixels,gray,&bytes);}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
    inline WriteResult Write(const std::filesystem::path& path,UINT w,UINT h,const std::vector<uint8_t>& pixels,bool gray)
    {return EncodeOrWrite(&path,w,h,pixels,gray,nullptr);}
#endif
}
