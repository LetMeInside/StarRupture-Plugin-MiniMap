#if defined(MODLOADER_CLIENT_BUILD)

#include "BuildingCache.h"

#include "../Sprites/CapturePng.h"
#include "CacheIdentity.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <string_view>

#pragma comment(lib, "windowscodecs.lib")

namespace
{
    constexpr char kMagic[8] = {'M','M','B','S','P','R','T','1'};
    constexpr uint32_t kVersion = 1;
    constexpr uint32_t kMaxDimension = 4096;
    constexpr size_t kMaxRgbaBytes = 64u * 1024u * 1024u;
    constexpr size_t kMaxPngBytes = 128u * 1024u * 1024u;
    constexpr size_t kMaxFileNameBytes = 240;
    int g_moduleAnchor = 0;
#pragma pack(push, 1)
    // 24-byte format core followed by the 48-byte spatial extension.
    struct SpriteHeader
    {
        char Magic[8];
        uint32_t Version;
        uint32_t Width;
        uint32_t Height;
        uint32_t PngBytes;
        double Min[3];
        double Max[3];
    };
#pragma pack(pop)
    static_assert(sizeof(SpriteHeader) == 72);

    bool ValidUtf8(const std::string& s)
    {
        if (s.empty()) return false;
        int needed = 0;
        uint32_t code = 0, minimum = 0;
        for (unsigned char c : s)
        {
            if (!needed)
            {
                if (c <= 0x7f) continue;
                if (c >= 0xc2 && c <= 0xdf) { needed = 1; code = c & 0x1f; minimum = 0x80; }
                else if (c >= 0xe0 && c <= 0xef) { needed = 2; code = c & 0x0f; minimum = 0x800; }
                else if (c >= 0xf0 && c <= 0xf4) { needed = 3; code = c & 0x07; minimum = 0x10000; }
                else return false;
            }
            else
            {
                if ((c & 0xc0) != 0x80) return false;
                code = (code << 6) | (c & 0x3f);
                if (--needed == 0 && (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))) return false;
            }
        }
        return needed == 0;
    }

    bool ValidBlueprintPath(const std::string& path)
    {
        if (!ValidUtf8(path) || path.size() > 4096 || path.front() != '/' || path.find('\\') != std::string::npos) return false;
        if (path.find("//") != std::string::npos) return false;
        for (unsigned char c : path) if (c < 0x20 || c == 0x7f) return false;
        const size_t slash = path.find_last_of('/');
        const size_t dot = path.find_last_of('.');
        if (dot == std::string::npos || dot <= slash || dot + 1 >= path.size()) return false;
        if (path.find('/', dot) != std::string::npos || path.find('.', dot + 1) != std::string::npos) return false;
        if (path.substr(dot + 1).size() < 3 || path.compare(path.size() - 2, 2, "_C") != 0) return false;
        for (size_t start = 1; start < dot;)
        {
            const size_t end = path.find('/', start);
            const size_t stop = end == std::string::npos || end > dot ? dot : end;
            const auto component = std::string_view(path).substr(start, stop - start);
            if (component.empty() || component == "." || component == "..") return false;
            start = stop + 1;
        }
        return true;
    }

    bool GetPluginDirectory(std::filesystem::path& out)
    {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&g_moduleAnchor), &module)) return false;
        std::wstring modulePath(32768, L'\0');
        const DWORD length = GetModuleFileNameW(module, modulePath.data(), static_cast<DWORD>(modulePath.size()));
        if (!length || length >= modulePath.size()) return false;
        modulePath.resize(length);
        out = std::filesystem::path(modulePath).parent_path();
        return true;
    }

    bool Paths(const std::string& blueprint, std::filesystem::path& sprite, std::filesystem::path* png)
    {
        std::string name;
        if (!MiniMapBuildingCache::BlueprintFileName(blueprint, name) || !MiniMapCacheIdentity::IsReady()) return false;
        std::filesystem::path root;
        if (!MiniMapBuildingCache::GetCacheRoot(root)) return false;
        sprite = root / std::filesystem::path(std::wstring(name.begin(), name.end()));
        if (png)
        {
            name.replace(name.size() - 7, 7, ".png");
            *png = root / std::filesystem::path(std::wstring(name.begin(), name.end()));
        }
        return true;
    }

    bool ValidBounds(const MiniMapBuildingCache::SpatialBounds& bounds)
    {
        for (size_t i = 0; i != 3; ++i)
            if (!std::isfinite(bounds.Min[i]) || !std::isfinite(bounds.Max[i]) || bounds.Max[i] <= bounds.Min[i]) return false;
        return true;
    }

    bool Decode(const std::vector<uint8_t>& png, uint32_t expectedWidth, uint32_t expectedHeight,
        std::vector<uint8_t>& rgba)
    {
        if (png.empty() || png.size() > kMaxPngBytes || png.size() > (std::numeric_limits<UINT>::max)()) return false;
        const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(init) && init != RPC_E_CHANGED_MODE) return false;
        struct ComScope { bool Own; ~ComScope() { if (Own) CoUninitialize(); } } scope{SUCCEEDED(init)};
        using Microsoft::WRL::ComPtr;
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
            FAILED(factory->CreateStream(&stream)) ||
            FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(png.data()), static_cast<DWORD>(png.size()))) ||
            FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) ||
            FAILED(decoder->GetFrame(0, &frame))) return false;
        UINT width = 0, height = 0;
        if (FAILED(frame->GetSize(&width, &height)) || width != expectedWidth || height != expectedHeight) return false;
        if (FAILED(factory->CreateFormatConverter(&converter)) ||
            FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
        const size_t bytes = size_t(width) * height * 4;
        if (bytes > kMaxRgbaBytes) return false;
        rgba.resize(bytes);
        return SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(bytes), rgba.data()));
    }

    bool Load(const std::filesystem::path& path, MiniMapBuildingCache::Sprite& out)
    {
        out = {};
        std::error_code ec;
        const uintmax_t size = std::filesystem::file_size(path, ec);
        if (ec || size < sizeof(SpriteHeader) || size > sizeof(SpriteHeader) + kMaxPngBytes) return false;
        std::ifstream stream(path, std::ios::binary);
        SpriteHeader header{};
        stream.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!stream || std::memcmp(header.Magic, kMagic, sizeof(kMagic)) || header.Version != kVersion ||
            !header.Width || !header.Height || header.Width > kMaxDimension || header.Height > kMaxDimension ||
            uint64_t(header.Width) * header.Height * 4 > kMaxRgbaBytes || !header.PngBytes ||
            header.PngBytes > kMaxPngBytes || size != sizeof(SpriteHeader) + uint64_t(header.PngBytes)) return false;
        MiniMapBuildingCache::SpatialBounds bounds;
        std::copy_n(header.Min, 3, bounds.Min.begin());
        std::copy_n(header.Max, 3, bounds.Max.begin());
        if (!ValidBounds(bounds)) return false;
        std::vector<uint8_t> png(header.PngBytes);
        stream.read(reinterpret_cast<char*>(png.data()), static_cast<std::streamsize>(png.size()));
        if (!stream || !Decode(png, header.Width, header.Height, out.Rgba)) { out = {}; return false; }
        out.Width = header.Width; out.Height = header.Height; out.Bounds = bounds;
        return true;
    }
}

namespace MiniMapBuildingCache
{
    bool BlueprintFileName(const std::string& canonicalBlueprintPath, std::string& fileName)
    {
        fileName.clear();
        if (!ValidBlueprintPath(canonicalBlueprintPath)) return false;
        constexpr char hex[] = "0123456789ABCDEF";
        for (unsigned char byte : canonicalBlueprintPath)
        {
            const bool safe = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                (byte >= '0' && byte <= '9') || byte == '.' || byte == '-' || byte == '_';
            if (safe) fileName.push_back(char(byte));
            else { fileName.push_back('%'); fileName.push_back(hex[byte >> 4]); fileName.push_back(hex[byte & 15]); }
        }
        fileName += ".sprite";
        if (fileName.size() > kMaxFileNameBytes) { fileName.clear(); return false; }
        return true;
    }

    bool GetCacheRoot(std::filesystem::path& directory)
    {
        if (!MiniMapCacheIdentity::IsReady()) return false;
        std::filesystem::path root;
        if (!GetPluginDirectory(root)) return false;
        directory = root / L"MiniMap" / L"Cache" / L"Buildings";
        return true;
    }

    bool Clear()
    {
        std::filesystem::path root;
        if (!GetPluginDirectory(root)) return false;
        root /= L"MiniMap"; root /= L"Cache"; root /= L"Buildings";
        std::error_code error;
        std::filesystem::remove_all(root, error);
        return !error;
    }

    bool Read(const std::string& canonicalBlueprintPath, Sprite& out)
    {
        std::filesystem::path path;
        if (!Paths(canonicalBlueprintPath, path, nullptr)) { out = {}; return false; }
        return Load(path, out);
    }

    bool Write(const std::string& canonicalBlueprintPath, const uint8_t* rgba,
        uint32_t width, uint32_t height, const SpatialBounds& bounds)
    {
        if (!rgba || !width || !height || width > kMaxDimension || height > kMaxDimension ||
            uint64_t(width) * height * 4 > kMaxRgbaBytes || !ValidBounds(bounds)) return false;
        std::filesystem::path path, diagnostic;
        if (!Paths(canonicalBlueprintPath, path, &diagnostic)) return false;
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) return false;
        const auto pngTemporary = path.wstring() + L".png.tmp";
        std::vector<uint8_t> pixels(rgba, rgba + size_t(width) * height * 4);
        const auto pngResult = MiniMapCapturePng::Write(pngTemporary, width, height, pixels, false);
        if (!pngResult.Ok || pngResult.Bytes == 0 || pngResult.Bytes > kMaxPngBytes)
        {
            std::filesystem::remove(pngTemporary, ec); return false;
        }
        std::vector<uint8_t> png(static_cast<size_t>(pngResult.Bytes));
        {
            std::ifstream input(pngTemporary, std::ios::binary);
            input.read(reinterpret_cast<char*>(png.data()), static_cast<std::streamsize>(png.size()));
            if (!input || input.peek() != std::char_traits<char>::eof())
            { input.close(); std::filesystem::remove(pngTemporary, ec); return false; }
        }
        std::filesystem::remove(pngTemporary, ec);
        if (ec) return false;
        const auto temporary = path.wstring() + L".tmp";
        SpriteHeader header{};
        std::memcpy(header.Magic, kMagic, sizeof(kMagic)); header.Version = kVersion;
        header.Width = width; header.Height = height; header.PngBytes = static_cast<uint32_t>(png.size());
        std::copy(bounds.Min.begin(), bounds.Min.end(), header.Min);
        std::copy(bounds.Max.begin(), bounds.Max.end(), header.Max);
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) return false;
            output.write(reinterpret_cast<const char*>(&header), sizeof(header));
            output.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
            output.close();
            if (!output) { std::filesystem::remove(temporary, ec); return false; }
        }
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        { std::filesystem::remove(temporary, ec); return false; }
        return true;
    }

    bool Exists(const std::string& canonicalBlueprintPath)
    {
        std::filesystem::path path;
        if (!Paths(canonicalBlueprintPath, path, nullptr)) return false;
        Sprite sprite;
        return Load(path, sprite);
    }

    bool WriteDiagnosticPng(const std::string& canonicalBlueprintPath,
        const uint8_t* rgba, uint32_t width, uint32_t height)
    {
        if (!rgba || !width || !height || width > kMaxDimension || height > kMaxDimension ||
            uint64_t(width) * height * 4 > kMaxRgbaBytes) return false;
        std::filesystem::path sprite, png;
        if (!Paths(canonicalBlueprintPath, sprite, &png)) return false;
        std::error_code ec;
        std::filesystem::create_directories(png.parent_path(), ec);
        if (ec) return false;
        std::vector<uint8_t> pixels(rgba, rgba + size_t(width) * height * 4);
        return MiniMapCapturePng::Write(png, width, height, pixels, false).Ok;
    }
}

#endif
