#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include <windows.h>
#include "PluginPaths.h"
#include <bcrypt.h>
#include <winver.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

// Plugin-owned file-cache identity. Native compatibility does not use this value.
namespace MiniMapCacheIdentity
{
    using Digest = std::array<uint8_t, 32>;
    inline constexpr char kMagic[8] = {'M','M','C','A','C','H','E','1'};
#pragma pack(push, 1)
    struct CacheInfo
    {
        char Magic[8];
        uint32_t FormatVersion;
        Digest ExecutableDigest;
    };
#pragma pack(pop)
    static_assert(sizeof(CacheInfo) == 44);

    // A lease covers the complete file operation, including stream destruction,
    // temporary cleanup and publication. Exit takes exclusive ownership to drain
    // readers/writers and permanently close admission for this plugin session.
    inline std::shared_mutex g_fileAccessMutex;
    inline bool g_fileAccessClosed = false; // Protected by g_fileAccessMutex.
    inline bool g_exitClearAttempted = false; // Same lock; startup clearing is separate.
    class FileAccess
    {
        std::shared_lock<std::shared_mutex> Lock{g_fileAccessMutex};
    public:
        explicit operator bool() const { return !g_fileAccessClosed; }
    };
    // Implemented beside the existing persistent-cache clearing routine.
    void ClearOnExit();

    inline std::atomic<bool> g_authorized{false};
    inline bool IsReady() { return g_authorized.load(); }
    inline void Reset() { g_authorized.store(false); }

    // Validate the VERSIONINFO block envelope recursively through an explicit
    // work list. Unknown keys/values remain valid and are included in the raw hash.
    inline bool ValidVersionInfo(const uint8_t* data, size_t size)
    {
        if (!data || size < 6) return false;
        auto word = [&](size_t at) { uint16_t v; std::memcpy(&v, data+at, 2); return v; };
        auto aligned = [](size_t at) { return (at+3) & ~size_t(3); };
        std::vector<std::pair<size_t,size_t>> pending{{0,size}};
        while (!pending.empty())
        {
            const auto [start, limit] = pending.back(); pending.pop_back();
            if (limit-start < 6) return false;
            const size_t length = word(start), valueLength = word(start+2);
            const uint16_t type = word(start+4);
            if (length < 8 || length > limit-start || type > 1) return false;
            const size_t end = start+length;
            size_t key = start+6;
            while (key+2 <= end && word(key)) key += 2;
            if (key+2 > end) return false;
            key += 2; // Include the UTF-16 terminator.
            const size_t value = (std::min)(aligned(key), end);
            const size_t bytes = valueLength * (type == 1 ? 2 : 1);
            if (bytes > end-value) return false;
            if (!start)
            {
                constexpr wchar_t rootKey[] = L"VS_VERSION_INFO";
                if (type != 0 || key-6 != sizeof(rootKey) ||
                    std::memcmp(data+6, rootKey, sizeof(rootKey)) ||
                    (bytes != 0 && bytes != sizeof(VS_FIXEDFILEINFO))) return false;
                if (bytes)
                {
                    VS_FIXEDFILEINFO fixed{};
                    std::memcpy(&fixed, data+value, sizeof(fixed));
                    if (fixed.dwSignature != VS_FFI_SIGNATURE) return false;
                }
                // Resource data may have only DWORD alignment padding after its root.
                if (size-end > 3) return false;
                for (size_t i=end; i<size; ++i) if (data[i]) return false;
            }
            if (type == 1 && bytes && word(value+bytes-2)) return false;
            size_t child = aligned(value+bytes);
            while (child < end)
            {
                if (end-child < 8) return false;
                const size_t childLength = word(child);
                if (childLength < 8 || childLength > end-child) return false;
                pending.emplace_back(child, child+childLength);
                child = aligned(child+childLength);
            }
        }
        return true;
    }

    struct VersionResource
    {
        bool Named;
        uint16_t Id;
        std::wstring Name;
        WORD Language;
        DWORD CodePage;
        const uint8_t* Data;
        DWORD Size;
    };

    // Walk only the executable's RT_VERSION directory. Reading exact directory
    // leaves avoids FindResourceEx's language-zero/UI-language fallback.
    inline bool ReadVersionResources(const uint8_t* file, size_t size,
        std::vector<VersionResource>& resources)
    {
        auto read = [&](size_t at, auto& value)
        {
            if (at > size || sizeof(value) > size-at) return false;
            std::memcpy(&value, file+at, sizeof(value)); return true;
        };
        IMAGE_DOS_HEADER dos{}; DWORD signature{}; IMAGE_FILE_HEADER header{};
        IMAGE_OPTIONAL_HEADER64 optional{};
        if (!read(0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
            !read(size_t(dos.e_lfanew), signature) || signature != IMAGE_NT_SIGNATURE ||
            !read(size_t(dos.e_lfanew)+4, header) || header.SizeOfOptionalHeader < sizeof(optional) ||
            !read(size_t(dos.e_lfanew)+4+sizeof(header), optional) ||
            optional.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            optional.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_RESOURCE) return false;
        const size_t sections = size_t(dos.e_lfanew)+4+sizeof(header)+header.SizeOfOptionalHeader;
        auto fileOffset = [&](DWORD rva, size_t bytes, size_t& offset)
        {
            for (unsigned i=0; i<header.NumberOfSections; ++i)
            {
                IMAGE_SECTION_HEADER section{};
                if (!read(sections+size_t(i)*sizeof(section), section)) return false;
                if (rva < section.VirtualAddress) continue;
                const size_t relative = size_t(rva)-section.VirtualAddress;
                if (relative > section.SizeOfRawData || bytes > section.SizeOfRawData-relative) continue;
                offset = size_t(section.PointerToRawData)+relative;
                return offset <= size && bytes <= size-offset;
            }
            return false;
        };
        const auto& directory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
        size_t root{};
        if (!directory.VirtualAddress || !directory.Size ||
            !fileOffset(directory.VirtualAddress, directory.Size, root)) return false;
        auto resourceRead = [&](size_t at, auto& value)
        {
            return at <= directory.Size && sizeof(value) <= directory.Size-at && read(root+at, value);
        };
        auto entries = [&](DWORD at, std::vector<IMAGE_RESOURCE_DIRECTORY_ENTRY>& result)
        {
            IMAGE_RESOURCE_DIRECTORY dir{};
            if (!resourceRead(at, dir)) return false;
            const size_t count = size_t(dir.NumberOfNamedEntries)+dir.NumberOfIdEntries;
            const size_t begin = size_t(at)+sizeof(dir);
            if (begin > directory.Size || count > (directory.Size-begin)/sizeof(IMAGE_RESOURCE_DIRECTORY_ENTRY)) return false;
            result.resize(count);
            for (size_t i=0; i<count; ++i)
                if (!resourceRead(begin+i*sizeof(result[i]), result[i])) return false;
            return true;
        };
        std::vector<IMAGE_RESOURCE_DIRECTORY_ENTRY> types, names, languages;
        if (!entries(0, types)) return false;
        bool found = false;
        for (const auto& type : types)
        {
            if (type.NameIsString || type.Id != reinterpret_cast<ULONG_PTR>(RT_VERSION)) continue;
            if (found || !type.DataIsDirectory || !entries(type.OffsetToDirectory, names)) return false;
            found = true;
            for (const auto& name : names)
            {
                if (!name.DataIsDirectory || !entries(name.OffsetToDirectory, languages) || languages.empty()) return false;
                VersionResource resource{};
                resource.Named = name.NameIsString != 0;
                if (resource.Named)
                {
                    WORD length{};
                    if (!resourceRead(name.NameOffset, length)) return false;
                    const size_t at = size_t(name.NameOffset)+sizeof(length), bytes = size_t(length)*sizeof(wchar_t);
                    if (at > directory.Size || bytes > directory.Size-at) return false;
                    resource.Name.resize(length);
                    std::memcpy(resource.Name.data(), file+root+at, bytes);
                }
                else { if (name.Name > UINT16_MAX) return false; resource.Id = name.Id; }
                for (const auto& language : languages)
                {
                    IMAGE_RESOURCE_DATA_ENTRY leaf{}; size_t offset{};
                    if (language.NameIsString || language.Name > UINT16_MAX || language.DataIsDirectory ||
                        !resourceRead(language.OffsetToData, leaf) || !leaf.Size ||
                        !fileOffset(leaf.OffsetToData, leaf.Size, offset) ||
                        !ValidVersionInfo(file+offset, leaf.Size)) return false;
                    resource.Language = language.Id;
                    resource.CodePage = leaf.CodePage;
                    resource.Data = file+offset;
                    resource.Size = leaf.Size;
                    resources.push_back(resource);
                }
            }
        }
        return found && !resources.empty();
    }

    inline bool HashVersionInfo(const std::filesystem::path& path, Digest& out)
    {
        out = {};
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER length{};
        const bool sized = GetFileSizeEx(file, &length) && length.QuadPart > 0 &&
            uint64_t(length.QuadPart) <= SIZE_MAX;
        const HANDLE mapping = sized ? CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr) : nullptr;
        const auto* data = mapping ? static_cast<const uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0)) : nullptr;
        if (!data)
        {
            if (mapping) CloseHandle(mapping);
            CloseHandle(file);
            return false;
        }
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        Digest digest{};
        bool success = false;
        try
        {
            success = [&]()
            {
                std::vector<VersionResource> resources;
                if (!ReadVersionResources(data, static_cast<size_t>(length.QuadPart), resources)) return false;
                std::sort(resources.begin(), resources.end(), [](const auto& a, const auto& b)
                {
                    if (a.Named != b.Named) return a.Named < b.Named;
                    if (a.Named && a.Name != b.Name) return a.Name < b.Name;
                    if (!a.Named && a.Id != b.Id) return a.Id < b.Id;
                    return a.Language < b.Language;
                });
                for (size_t i=1; i<resources.size(); ++i)
                {
                    const auto& a = resources[i-1]; const auto& b = resources[i];
                    if (a.Named == b.Named && a.Id == b.Id && a.Name == b.Name && a.Language == b.Language) return false;
                }
                if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
                    BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) return false;
                auto bytes = [&](const void* data, ULONG size)
                { return BCryptHashData(hash, const_cast<PUCHAR>(static_cast<const uint8_t*>(data)), size, 0) >= 0; };
                auto number = [&](uint32_t value)
                {
                    const uint8_t encoded[] = {uint8_t(value), uint8_t(value>>8), uint8_t(value>>16), uint8_t(value>>24)};
                    return bytes(encoded, sizeof(encoded));
                };
                // Fixed domain, then length-framed records sorted by resource identity.
                constexpr char domain[] = "MiniMap VERSIONINFO";
                if (resources.size() > UINT32_MAX || !bytes(domain, sizeof(domain)) ||
                    !number(static_cast<uint32_t>(resources.size()))) return false;
                for (const auto& resource : resources)
                {
                    if (resource.Name.size() > UINT32_MAX/sizeof(wchar_t) ||
                        !number(resource.Named ? 1 : 0) || !number(resource.Id) ||
                        !number(static_cast<uint32_t>(resource.Name.size()))) return false;
                    if (resource.Named && !bytes(resource.Name.data(), static_cast<ULONG>(resource.Name.size()*sizeof(wchar_t)))) return false;
                    if (!number(resource.Language) || !number(resource.CodePage) || !number(resource.Size) ||
                        !bytes(resource.Data, resource.Size)) return false;
                }
                return BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
            }();
        }
        catch (...) { success = false; }
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        UnmapViewOfFile(data);
        CloseHandle(mapping);
        CloseHandle(file);
        if (success) out = digest;
        return success;
    }

    inline bool GetExecutableFingerprint(Digest& out)
    {
        static std::mutex access;
        static Digest cached{};
        static bool acquired = false;
        std::lock_guard<std::mutex> lock(access);
        out = {};
        if (!acquired)
        {
            std::wstring path(32768, L'\0');
            const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                static_cast<DWORD>(path.size()));
            if (!length || length >= path.size()) return false;
            path.resize(length);
            std::filesystem::path executable;
            if (!MiniMapPluginPaths::Extended(path, executable) || !HashVersionInfo(executable, cached)) return false;
            acquired = true;
        }
        out = cached;
        return true;
    }

    inline CacheInfo MakeInfo(const Digest& digest)
    {
        CacheInfo info{};
        std::memcpy(info.Magic, kMagic, sizeof(kMagic));
        info.FormatVersion = 1;
        info.ExecutableDigest = digest;
        return info;
    }

    inline bool ReadInfo(const std::filesystem::path& path, CacheInfo& out)
    {
        out = {};
        std::error_code error;
        if (MiniMapPluginPaths::FileSize(path, error) != sizeof(CacheInfo) || error)
            return false;
        std::ifstream stream(path, std::ios::binary);
        stream.read(reinterpret_cast<char*>(&out), sizeof(out));
        return stream && !std::memcmp(out.Magic, kMagic, sizeof(kMagic)) &&
            out.FormatVersion == 1;
    }

    inline bool WriteInfo(const std::filesystem::path& path, const CacheInfo& info)
    {
        const std::filesystem::path temporary = path.wstring() + L".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            stream.write(reinterpret_cast<const char*>(&info), sizeof(info));
            stream.close();
            if (!stream)
            {
                std::error_code error;
                std::filesystem::remove(temporary, error);
                return false;
            }
        }
        if (MoveFileExW(temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        std::error_code error;
        std::filesystem::remove(temporary, error);
        return false;
    }

    // Called once by PluginInit before consumers. Only the supplied known cache
    // owner is cleared; unknown root contents and diagnostics are never enumerated.
    inline bool Initialize(const std::filesystem::path& root, bool (*clearTerrain)(),
        const char*& failure, bool forceClear = false)
    {
        std::unique_lock access(g_fileAccessMutex);
        g_fileAccessClosed = false;
        g_exitClearAttempted = false;
        Reset();
        failure = nullptr;
        Digest digest{};
        if (!GetExecutableFingerprint(digest))
        {
            // No identity is fabricated. Start a fresh usable cache for this run;
            // leave no root record that could bless these entries on a later run.
            if (!clearTerrain())
            {
                failure = "terrain cache invalidation failed";
                return false;
            }
            std::error_code error;
            std::filesystem::remove(root / L"CacheInfo", error);
            if (error)
            {
                failure = "stale root CacheInfo removal failed";
                return false;
            }
            g_authorized.store(true);
            return true;
        }
        const auto current = MakeInfo(digest);
        const auto path = root / L"CacheInfo";
        CacheInfo stored{};
        if (forceClear || !ReadInfo(path, stored) || stored.ExecutableDigest != digest)
        {
            if (!clearTerrain())
            {
                failure = "terrain cache invalidation failed";
                return false;
            }
            std::error_code error;
            std::filesystem::create_directories(root, error);
            if (error || !WriteInfo(path, current))
            {
                failure = "root CacheInfo write failed";
                return false;
            }
        }
        g_authorized.store(true);
        return true;
    }
}

#endif
