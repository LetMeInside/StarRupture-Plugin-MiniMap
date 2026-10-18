#pragma once
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

// Filesystem addressing only. Do not convert Unreal asset paths, configuration
// keys or URL/display identifiers through this helper. Physical directories stay unchanged.
namespace MiniMapPluginPaths
{
    inline const char ModuleAnchor = 0;

    inline bool Extended(const std::filesystem::path& absolute, std::filesystem::path& out)
    {
        auto path = absolute;
        path.make_preferred();
        path = path.lexically_normal();
        if (!path.is_absolute()) return false; // Never resolve through the game's CWD.
        auto text = path.wstring();
        if (text.compare(0, 4, L"\\\\.\\") == 0) return false; // Not a device-path utility.
        if (text.compare(0, 4, L"\\\\?\\") != 0)
            text = text.compare(0, 2, L"\\\\") == 0
                ? L"\\\\?\\UNC\\" + text.substr(2)
                : L"\\\\?\\" + text;
        out = std::filesystem::path(std::move(text));
        return true;
    }

    inline bool GetPluginDirectory(std::filesystem::path& out, const char** failure = nullptr)
    {
        struct CachedResolution
        {
            std::filesystem::path Directory;
            const char* Failure = "GetModuleHandleExW";
            bool Succeeded = false;
        };
        static const CachedResolution resolved = []
        {
            CachedResolution result;
            HMODULE module = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&ModuleAnchor), &module)) return result;
            std::wstring path(32768, L'\0');
            result.Failure = "GetModuleFileNameW (missing/truncated module path)";
            const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
            if (!length || length >= path.size()) return result;
            path.resize(length);
            result.Failure = "absolute extended plugin directory normalization";
            result.Succeeded = Extended(std::filesystem::path(path).parent_path(), result.Directory);
            return result;
        }();
        if (failure) *failure = resolved.Failure;
        if (!resolved.Succeeded) return false;
        out = resolved.Directory;
        return true;
    }

    // Avoid a second path-based attributes lookup for cache-size validation.
    // Callers supply the same absolute extended path used for their file streams.
    inline uintmax_t FileSize(const std::filesystem::path& path, std::error_code& error) noexcept
    {
        const HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        { error = std::error_code(int(GetLastError()), std::system_category()); return uintmax_t(-1); }
        LARGE_INTEGER length{};
        const bool sized = GetFileSizeEx(file, &length) != FALSE;
        const DWORD code = sized ? ERROR_SUCCESS : GetLastError();
        CloseHandle(file);
        if (!sized || length.QuadPart < 0)
        { error = std::error_code(int(sized ? ERROR_INVALID_DATA : code), std::system_category()); return uintmax_t(-1); }
        error.clear();
        return uintmax_t(length.QuadPart);
    }
}
