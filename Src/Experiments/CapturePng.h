#pragma once
#include "FloatReadbackPixels.h"
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <filesystem>
#include <string>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace MiniMapCapturePng
{
    struct Image { std::vector<uint8_t> Rgba, Mask; size_t Sanitized = 0; };
    inline uint8_t Byte(float v) { return uint8_t(std::lround(std::clamp(v,0.f,1.f)*255.f)); }
    inline uint8_t Display(float linear)
    {
        linear=(std::max)(0.f,linear);
        const float mapped=linear/(1.f+linear); // Reinhard; fixed unit exposure.
        return Byte(mapped<=0.0031308f ? 12.92f*mapped : 1.055f*std::pow(mapped,1.f/2.4f)-0.055f);
    }
    inline bool Convert(const uint8_t* data,size_t length,size_t w,size_t h,size_t pitch,size_t bufferHeight,Image& out)
    {
        size_t row,total,source;
        if(w>4096 || h>4096 || !MiniMapFloatPixels::Size(w,h,pitch,bufferHeight,row,total,source) ||
            !data || length<source || total>128*1024*1024) return false;
        out={};out.Rgba.resize(w*h*4);out.Mask.resize(w*h);
        for(size_t y=0;y<h;++y) for(size_t x=0;x<w;++x)
        {
            float c[4];
            for(size_t k=0;k<4;++k)
            {
                const size_t i=(y*pitch+x)*8+k*2;
                c[k]=MiniMapFloatPixels::Half(uint16_t(data[i])|(uint16_t(data[i+1])<<8));
                if(!std::isfinite(c[k])) {++out.Sanitized;c[k]=k==3?1.f:0.f;}
            }
            const size_t i=y*w+x;const uint8_t alpha=Byte(1.f-std::clamp(c[3],0.f,1.f));
            out.Mask[i]=alpha;out.Rgba[i*4+3]=alpha;
            // RGB association is unverified for partial coverage. Do not divide
            // by alpha; binary-alpha runtime evidence requires no unpremultiply.
            for(size_t k=0;k<3;++k)out.Rgba[i*4+k]=alpha?Display(c[k]):0;
        }
        return true;
    }
    struct WriteResult { bool Ok=false; uintmax_t Bytes=0; HRESULT Error=S_OK; const char* Stage=""; };
    inline WriteResult Write(const std::filesystem::path& path,UINT w,UINT h,const std::vector<uint8_t>& pixels,bool gray)
    {
        WriteResult r;
        const size_t channels=gray?1:4;
        if(!w||!h||w>4096||h>4096||pixels.size()!=size_t(w)*h*channels)
            return {false,0,E_INVALIDARG,"dimensions/byte count"};
        const HRESULT init=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        if(FAILED(init)&&init!=RPC_E_CHANGED_MODE)return {false,0,init,"COM initialization"};
        struct ComScope {bool Own;~ComScope(){if(Own)CoUninitialize();}} scope{SUCCEEDED(init)};
        using Microsoft::WRL::ComPtr;
        ComPtr<IWICImagingFactory> factory;ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;ComPtr<IWICBitmapFrameEncode> frame;
        auto step=[&](HRESULT hr,const char* stage){r.Error=hr;r.Stage=stage;return SUCCEEDED(hr);};
        if(!step(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"WIC factory"))return r;
        if(!step(factory->CreateStream(&stream),"WIC stream")||
            !step(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE),"open output")||
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
        frame.Reset();encoder.Reset();stream.Reset();
        std::error_code ec;r.Bytes=std::filesystem::file_size(path,ec);
        if(ec)return {false,0,HRESULT_FROM_WIN32(ec.value()),"file size"};
        r.Ok=true;return r;
    }
}
