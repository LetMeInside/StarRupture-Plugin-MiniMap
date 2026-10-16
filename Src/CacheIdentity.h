#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

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

    inline std::atomic<bool> g_authorized{false};
    inline bool IsReady() { return g_authorized.load(); }
    inline void Reset() { g_authorized.store(false); }

    // Hash installed bytes, never relocated/mutable executable memory.
    inline bool HashFile(const std::filesystem::path& path, Digest& out)
    {
        out = {};
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        Digest digest{};
        const bool success = [&]()
        {
            if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                nullptr, 0) < 0) return false;
            if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0)
                return false;
            std::array<uint8_t, 64 * 1024> buffer{};
            for (;;)
            {
                DWORD count = 0;
                if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                    &count, nullptr)) return false;
                if (count == 0) break;
                if (BCryptHashData(hash, buffer.data(), count, 0) < 0) return false;
            }
            return BCryptFinishHash(hash, digest.data(),
                static_cast<ULONG>(digest.size()), 0) >= 0;
        }();
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
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
            if (!HashFile(path, cached)) return false;
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
        if (std::filesystem::file_size(path, error) != sizeof(CacheInfo) || error)
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
        const char*& failure)
    {
        Reset();
        failure = nullptr;
        Digest digest{};
        if (!GetExecutableFingerprint(digest))
        {
            failure = "executable SHA-256 acquisition failed";
            return false;
        }
        const auto current = MakeInfo(digest);
        const auto path = root / L"CacheInfo";
        CacheInfo stored{};
        if (!ReadInfo(path, stored) || stored.ExecutableDigest != digest)
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
