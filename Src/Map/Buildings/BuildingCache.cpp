#include "CaptureTiming.h"
#if defined(MODLOADER_CLIENT_BUILD)

#include "BuildingCache.h"

#include "CapturePng.h"
#include "CacheIdentity.h"
#include "PluginPaths.h"
#include "plugin_helpers.h"

#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>
#include <string_view>
#include <utility>

#pragma comment(lib, "windowscodecs.lib")

namespace
{
    // Write has one attempt per retired request. No diagnostic allocations or retries.
    bool WriteRejected(const std::string& blueprint, const char* reason, uint32_t code = 0, bool codeAvailable = false) noexcept
    {
        try
        {
            if (auto* self = GetSelf(); self && self->logger && self->logger->Warn)
                self->logger->Warn(self, "MiniMap: sprite persistence rejected: canonical=%s reason=%s codeAvailable=%u code=0x%08X; one-time write failed",
                    blueprint.c_str(), reason, unsigned(codeAvailable), code);
        }
        catch (...) {}
        return false;
    }
#if MINIMAP_BUILDING_CAPTURE_PNGS
    bool DiagnosticPngRejected(const std::string& blueprint, const char* reason, uint32_t code = 0, bool codeAvailable = false) noexcept
    {
        try
        {
            if (auto* self = GetSelf(); self && self->logger && self->logger->Warn)
                self->logger->Warn(self, "MiniMap: sprite PNG export failed: canonical=%s reason=%s codeAvailable=%u code=0x%08X; successful .sprite persistence unchanged",
                    blueprint.c_str(), reason, unsigned(codeAvailable), code);
        }
        catch (...) {}
        return false;
    }
#endif
    constexpr char kMagic[8] = {'M','M','B','S','P','R','T','1'};
    constexpr uint32_t kVersion = 1;
    constexpr uint32_t kMaxDimension = 4096;
    constexpr size_t kMaxRgbaBytes = 64u * 1024u * 1024u;
    constexpr size_t kMaxPngBytes = 128u * 1024u * 1024u;
    constexpr size_t kMaxFileNameBytes = 240;
#pragma pack(push, 1)
    // Existing format core/bounds followed by the processed-image sampling map.
    struct SpriteHeader
    {
        char Magic[8];
        uint32_t Version;
        uint32_t Width;
        uint32_t Height;
        uint32_t PngBytes;
        double Min[3];
        double Max[3];
        uint16_t SourceX, SourceY;
        double SourceStep;
    };
#pragma pack(pop)
    static_assert(sizeof(SpriteHeader) == 84);

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
        return MiniMapPluginPaths::GetPluginDirectory(out);
    }

    bool Paths(const std::string& blueprint, std::filesystem::path& sprite, std::filesystem::path* png, const char** failure = nullptr)
    {
        std::string name;
        if (failure) *failure = "canonical cache filename resolution rejected";
        if (!MiniMapBuildingCache::BlueprintFileName(blueprint, name)) return false;
        if (failure) *failure = "cache namespace identity not ready";
        if (!MiniMapCacheIdentity::IsReady()) return false;
        std::filesystem::path root;
        if (failure) *failure = "cache root/module path resolution failed";
        if (!MiniMapBuildingCache::GetCacheRoot(root)) return false;
        sprite = root / std::filesystem::path(std::wstring(name.begin(), name.end()));
        if (png)
        {
            name.replace(name.size() - 7, 7, ".png");
            *png = root / std::filesystem::path(std::wstring(name.begin(), name.end()));
        }
        return true;
    }

    MiniMapBuildingCache::LoadResult Decode(const std::vector<uint8_t>& png, uint32_t expectedWidth, uint32_t expectedHeight,
        std::vector<uint8_t>& rgba)
    {
        using S = MiniMapBuildingCache::LoadStatus;
        if (png.empty() || png.size() > kMaxPngBytes || png.size() > (std::numeric_limits<UINT>::max)())
            return {S::Invalid, "PNG length"};
        const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(init) && init != RPC_E_CHANGED_MODE) return {S::Unavailable, "COM initialization", uint32_t(init)};
        struct ComScope { bool Own; ~ComScope() { if (Own) CoUninitialize(); } } scope{SUCCEEDED(init)};
        using Microsoft::WRL::ComPtr;
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (FAILED(hr)) return {S::Unavailable, "WIC factory", uint32_t(hr)};
        if (FAILED(hr = factory->CreateStream(&stream)) ||
            FAILED(hr = stream->InitializeFromMemory(const_cast<BYTE*>(png.data()), static_cast<DWORD>(png.size()))))
            return {S::Unavailable, "WIC memory stream", uint32_t(hr)};
        if (FAILED(hr = factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) ||
            FAILED(hr = decoder->GetFrame(0, &frame))) return {S::Invalid, "PNG decoder/frame", uint32_t(hr)};
        UINT width = 0, height = 0;
        if (FAILED(hr = frame->GetSize(&width, &height)) || width != expectedWidth || height != expectedHeight)
            return {S::Invalid, "PNG dimensions", uint32_t(hr)};
        if (FAILED(hr = factory->CreateFormatConverter(&converter))) return {S::Unavailable, "WIC converter", uint32_t(hr)};
        if (FAILED(hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                nullptr, 0, WICBitmapPaletteTypeCustom))) return {S::Invalid, "PNG pixel format", uint32_t(hr)};
        const size_t bytes = size_t(width) * height * 4;
        if (bytes > kMaxRgbaBytes) return {S::Invalid, "RGBA length"};
        rgba.resize(bytes);
        hr = converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(bytes), rgba.data());
        return SUCCEEDED(hr) ? MiniMapBuildingCache::LoadResult{S::Loaded, "decoded"}
            : MiniMapBuildingCache::LoadResult{S::Invalid, "PNG pixels", uint32_t(hr)};
    }

    MiniMapBuildingCache::LoadResult Load(const std::filesystem::path& path, MiniMapBuildingCache::Sprite& out)
    {
        using S = MiniMapBuildingCache::LoadStatus;
        // Classify the actual open result, including races with deletion/replacement.
        const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            const DWORD error = GetLastError();
            return {(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) ? S::Missing : S::IOError,
                "open sprite", error};
        }
        struct FileScope { HANDLE Handle; ~FileScope() { CloseHandle(Handle); } } file{handle};
        LARGE_INTEGER length{};
        if (!GetFileSizeEx(handle, &length)) return {S::IOError, "sprite size", GetLastError()};
        if (length.QuadPart < LONGLONG(sizeof(SpriteHeader)) ||
            uint64_t(length.QuadPart) > sizeof(SpriteHeader) + uint64_t(kMaxPngBytes))
            return {S::Invalid, "sprite length"};
        SpriteHeader header{};
        DWORD read = 0;
        if (!ReadFile(handle, &header, DWORD(sizeof(header)), &read, nullptr)) return {S::IOError, "sprite header read", GetLastError()};
        if (read != sizeof(header) || std::memcmp(header.Magic, kMagic, sizeof(kMagic)) || header.Version != kVersion ||
            !header.Width || !header.Height || header.Width > kMaxDimension || header.Height > kMaxDimension ||
            uint64_t(header.Width) * header.Height * 4 > kMaxRgbaBytes || !header.PngBytes ||
            header.PngBytes > kMaxPngBytes || uint64_t(length.QuadPart) != sizeof(SpriteHeader) + uint64_t(header.PngBytes))
            return {S::Invalid, "sprite header/format"};
        MiniMapBuildingCache::Sprite candidate;
        candidate.Width = header.Width; candidate.Height = header.Height;
        std::copy_n(header.Min, 3, candidate.Bounds.Min.begin());
        std::copy_n(header.Max, 3, candidate.Bounds.Max.begin());
        candidate.Mapping = {header.SourceX, header.SourceY, header.SourceStep};
        if (!MiniMapBuildingCache::ValidSpatial(candidate.Width, candidate.Height, candidate.Bounds, candidate.Mapping))
            return {S::Invalid, "sprite spatial mapping"};
        std::vector<uint8_t> png(header.PngBytes);
        if (!ReadFile(handle, png.data(), header.PngBytes, &read, nullptr)) return {S::IOError, "sprite payload read", GetLastError()};
        if (read != header.PngBytes) return {S::Invalid, "truncated sprite payload"};
        const auto result = Decode(png, header.Width, header.Height, candidate.Rgba);
        if (result.Status != S::Loaded) return result;
        out = std::move(candidate);
        return {S::Loaded, "loaded"};
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

    LoadResult TryLoad(const std::string& canonicalBlueprintPath, Sprite& out) noexcept
    {
        try
        {
            MiniMapCacheIdentity::FileAccess access;
            if (!access) return {LoadStatus::Unavailable, "cache file access closed"};
            std::string name;
            if (!BlueprintFileName(canonicalBlueprintPath, name)) return {LoadStatus::Invalid, "canonical cache filename"};
            std::filesystem::path root;
            if (!GetCacheRoot(root)) return {LoadStatus::Unavailable, "cache namespace unavailable"};
            return Load(root / std::filesystem::path(std::wstring(name.begin(), name.end())), out);
        }
        catch (const std::bad_alloc&) { return {LoadStatus::Unavailable, "cache allocation failure"}; }
        catch (const std::filesystem::filesystem_error& error)
        { return {LoadStatus::IOError, "cache filesystem exception", uint32_t(error.code().value())}; }
        catch (...) { return {LoadStatus::Unavailable, "cache read exception"}; }
    }

    bool Write(const std::string& canonicalBlueprintPath, const uint8_t* rgba,
        uint32_t width, uint32_t height, const SpatialBounds& bounds, const ImageMapping& mapping)
    {
        MiniMapCacheIdentity::FileAccess access;
        if (!access) return false;
        MiniMapCaptureTiming::Span timing(MiniMapCaptureTiming::Stage::Persistence);
        if (!rgba || !ValidSpatial(width, height, bounds, mapping) ||
            uint64_t(width) * height * 4 > kMaxRgbaBytes)
            return WriteRejected(canonicalBlueprintPath, "serialization input pointer/dimensions/byte bound rejected");
        std::filesystem::path path;
        const char* failure = nullptr;
        if (!Paths(canonicalBlueprintPath, path, nullptr, &failure)) return WriteRejected(canonicalBlueprintPath, failure);
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) return WriteRejected(canonicalBlueprintPath, "create cache directory failed", uint32_t(ec.value()), true);
        auto pixels=MiniMapCaptureTiming::Measure(MiniMapCaptureTiming::Stage::RgbaCopy,[&]{return std::vector<uint8_t>(rgba,rgba+size_t(width)*height*4);});
        std::vector<uint8_t> png;
        const auto pngResult = MiniMapCapturePng::Encode(width, height, pixels, false, png);
        if (!pngResult.Ok || pngResult.Bytes == 0 || pngResult.Bytes > kMaxPngBytes || png.size() != pngResult.Bytes)
            return WriteRejected(canonicalBlueprintPath, !pngResult.Ok ? pngResult.Stage : "serialized PNG byte length rejected", uint32_t(pngResult.Error), true);
        const auto temporary = path.wstring() + L".tmp";
        MiniMapCaptureTiming::Span headerTiming(MiniMapCaptureTiming::Stage::Header);
        SpriteHeader header{};
        std::memcpy(header.Magic, kMagic, sizeof(kMagic)); header.Version = kVersion;
        header.Width = width; header.Height = height; header.PngBytes = static_cast<uint32_t>(png.size());
        std::copy(bounds.Min.begin(), bounds.Min.end(), header.Min);
        std::copy(bounds.Max.begin(), bounds.Max.end(), header.Max);
        header.SourceX = mapping.X; header.SourceY = mapping.Y; header.SourceStep = mapping.Step;
        headerTiming.Stop();
        {
            MiniMapCaptureTiming::Span outputTiming(MiniMapCaptureTiming::Stage::SpriteWrite);
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) return WriteRejected(canonicalBlueprintPath, "open sprite output temporary failed");
            output.write(reinterpret_cast<const char*>(&header), sizeof(header));
            output.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
            output.close();
            if (!output) { std::filesystem::remove(temporary, ec); return WriteRejected(canonicalBlueprintPath, "write/close sprite temporary failed"); }
        }
        if (!MiniMapCaptureTiming::Measure(MiniMapCaptureTiming::Stage::SpriteRename,[&]{return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);}))
        {
            const auto error = GetLastError();
            std::filesystem::remove(temporary, ec);
            return WriteRejected(canonicalBlueprintPath, "atomic sprite publication failed", error, true);
        }
        return true;
    }

#if MINIMAP_BUILDING_CAPTURE_PNGS
    bool WriteDiagnosticPng(const std::string& canonicalBlueprintPath,
        const uint8_t* rgba, uint32_t width, uint32_t height) noexcept
    {
        MiniMapCaptureTiming::PngContext pngRole(1);
        MiniMapCaptureTiming::Span timing(MiniMapCaptureTiming::Stage::SourcePng);
        try
        {
            MiniMapCacheIdentity::FileAccess access;
            if (!access) return false;
            if (!rgba || !width || !height || width > kMaxDimension || height > kMaxDimension ||
                uint64_t(width) * height * 4 > kMaxRgbaBytes)
                return DiagnosticPngRejected(canonicalBlueprintPath, "source dimensions/byte bound rejected");
            std::filesystem::path sprite, png;
            const char* failure = nullptr;
            if (!Paths(canonicalBlueprintPath, sprite, &png, &failure)) return DiagnosticPngRejected(canonicalBlueprintPath, failure);
            std::error_code ec;
            std::filesystem::create_directories(png.parent_path(), ec);
            if (ec) return DiagnosticPngRejected(canonicalBlueprintPath, "create PNG parent directory failed", uint32_t(ec.value()), true);
            std::vector<uint8_t> pixels(rgba, rgba + size_t(width) * height * 4);
            const auto result = MiniMapCapturePng::Write(png, width, height, pixels, false);
            if (!result.Ok) return DiagnosticPngRejected(canonicalBlueprintPath, result.Stage, uint32_t(result.Error), true);
            return true;
        }
        catch (const std::filesystem::filesystem_error& error)
        { return DiagnosticPngRejected(canonicalBlueprintPath, "PNG filesystem exception", uint32_t(error.code().value()), true); }
        catch (...) { return DiagnosticPngRejected(canonicalBlueprintPath, "PNG CPU/encoding exception"); }
    }

#endif

#if MINIMAP_ATLAS_PNGS
    bool WriteAtlasDiagnosticPng(const std::vector<uint8_t>& rgba,
        uint32_t width, uint32_t height, uint64_t revision) noexcept
    {
        MiniMapCaptureTiming::PngContext pngRole(2);
        MiniMapCaptureTiming::Span preparation(MiniMapCaptureTiming::Stage::ExportPrep);
        MiniMapCaptureTiming::Span timing(MiniMapCaptureTiming::Stage::AtlasExport);
        std::string name;
        const char* stage = "diagnostic namespace";
        uint32_t code = 0;
        try
        {
            MiniMapCacheIdentity::FileAccess access;
            if (!access) return false;
            struct Temporary
            {
                std::filesystem::path Path;
                ~Temporary() { if (!Path.empty()) DeleteFileW(Path.c_str()); }
            } temporary;
            const auto attempt = [&]() -> bool
            {
                std::filesystem::path directory;
                if (!GetCacheRoot(directory)) return false;
                directory /= L"Diagnostics";
                std::error_code error;
                std::filesystem::create_directories(directory, error);
                if (error) { code = uint32_t(error.value()); return false; }

                stage = "diagnostic numbering";
                uint64_t highest = 0;
                const std::filesystem::directory_iterator end;
                for (auto it = std::filesystem::directory_iterator(directory, error); !error && it != end; it.increment(error))
                {
                    const auto file = it->path().filename().wstring();
                    if (file.size() < 11 || file.compare(0, 6, L"Atlas_") ||
                        file.compare(file.size() - 4, 4, L".png")) continue;
                    bool decimal = true;
                    for (size_t i = 6; i < file.size() - 4; ++i)
                        if (file[i] < L'0' || file[i] > L'9') { decimal = false; break; }
                    if (!decimal) continue;
                    uint64_t number = 0;
                    for (size_t i = 6; i < file.size() - 4; ++i)
                    {
                        const auto digit = uint64_t(file[i] - L'0');
                        if (number > ((std::numeric_limits<uint64_t>::max)() - digit) / 10)
                        { code = ERROR_ARITHMETIC_OVERFLOW; return false; }
                        number = number * 10 + digit;
                    }
                    highest = (std::max)(highest, number);
                }
                if (error) { code = uint32_t(error.value()); return false; }
                const auto numbered = [](uint64_t number)
                {
                    const auto digits = std::to_string(number);
                    return "Atlas_" + std::string(digits.size() < 6 ? 6 - digits.size() : 0, '0') + digits + ".png";
                };
                if (highest == (std::numeric_limits<uint64_t>::max)())
                { code = ERROR_ARITHMETIC_OVERFLOW; return false; }
                name = numbered(highest + 1); // Include the intended filename in encoding failures too.

                stage = "reserve PNG temporary";
                bool reserved = false;
                for (unsigned collision = 0; collision < 16; ++collision)
                {
                    GUID token{};
                    const auto hr = CoCreateGuid(&token);
                    if (FAILED(hr)) { code = uint32_t(hr); return false; }
                    std::wstring file = L".Atlas_";
                    constexpr wchar_t hex[] = L"0123456789ABCDEF";
                    const auto* bytes = reinterpret_cast<const uint8_t*>(&token);
                    for (size_t i = 0; i < sizeof(token); ++i)
                    { file.push_back(hex[bytes[i] >> 4]); file.push_back(hex[bytes[i] & 15]); }
                    file += L".tmp";
                    auto path = directory / file;
                    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (handle != INVALID_HANDLE_VALUE)
                    {
                        // The encoder may overwrite ONLY this operation's own
                        // reserved temporary, never a final Atlas_*.png file.
                        CloseHandle(handle);
                        temporary.Path = std::move(path); // Nonallocating ownership transfer.
                        reserved = true; break;
                    }
                    code = GetLastError();
                    if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS) return false;
                }
                if (!reserved) return false;
                preparation.Stop();
                stage = "PNG encoding";
                const auto encoded = MiniMapCapturePng::Write(temporary.Path, width, height, rgba, false);
                if (!encoded.Ok || !encoded.Bytes)
                { stage = encoded.Stage; code = uint32_t(encoded.Error); return false; }

                stage = "exclusive PNG publication";
                for (unsigned collision = 0; collision < 1024; ++collision)
                {
                    if (highest == (std::numeric_limits<uint64_t>::max)())
                    { code = ERROR_ARITHMETIC_OVERFLOW; return false; }
                    name = numbered(++highest);
                    const auto destination = directory / std::filesystem::path(std::wstring(name.begin(), name.end()));
                    // No REPLACE_EXISTING: a concurrent writer winning this name
                    // keeps its file. Retry the SAME encoded image at the next number.
                    if (MiniMapCaptureTiming::Measure(MiniMapCaptureTiming::Stage::ExportRename,[&]{return MoveFileExW(temporary.Path.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH);}))
                    { temporary.Path.clear(); return true; }
                    code = GetLastError();
                    if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS) return false;
                }
                return false;
            };
            if (attempt())
            {
                LOG_INFO("MiniMap: atlas PNG revision %llu %ux%u Diagnostics/%s saved",
                    static_cast<unsigned long long>(revision), width, height, name.c_str());
                return true;
            }
        }
        catch (...) { stage = "diagnostic exception"; }
        LOG_WARN("MiniMap: atlas PNG revision %llu %ux%u Diagnostics/%s failed at %s (0x%08X); GPU publication preserved",
            static_cast<unsigned long long>(revision), width, height,
            name.empty()?"<unallocated>":name.c_str(), stage, unsigned(code));
        return false;
    }
#endif
}

#endif
