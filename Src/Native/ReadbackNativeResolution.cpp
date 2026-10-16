#if defined(MODLOADER_CLIENT_BUILD) && defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
#include "ReadbackNativeAdapter.h"
#include "NativeApi.h"
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <array>
#include <cstring>
#include <vector>

namespace MiniMapReadbackNative
{
    namespace
    {
        const char* status = "native prerequisites not resolved";
        uintptr_t FindUnique(const unsigned char* text, size_t length, const char* pattern)
        {
            std::vector<int> bytes;
            for (auto p = pattern; *p;)
            {
                if (*p == ' ') { ++p; continue; }
                if (*p == '?') { bytes.push_back(-1); p += 2; continue; }
                auto hex = [](char c) { return c <= '9' ? c - '0' : c - 'A' + 10; };
                bytes.push_back(hex(p[0]) * 16 + hex(p[1])); p += 2;
            }
            if (bytes.empty() || bytes.size() > length || bytes[0] < 0) return 0;
            uintptr_t result = 0;
            const auto* end = text + length - bytes.size() + 1;
            for (auto* cursor = text; cursor < end;)
            {
                cursor = static_cast<const unsigned char*>(memchr(cursor, bytes[0], end - cursor));
                if (!cursor) break;
                size_t i = 1;
                for (; i < bytes.size(); ++i)
                    if (bytes[i] >= 0 && cursor[i] != bytes[i]) break;
                if (i == bytes.size())
                {
                    if (result) return 0;
                    result = reinterpret_cast<uintptr_t>(cursor);
                }
                ++cursor;
            }
            return result;
        }
    }

    uintptr_t ResolveOptionalFunction(const char* pattern, bool leaf)
    {
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (!base) return 0;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        const auto* sections = IMAGE_FIRST_SECTION(nt);
        for (unsigned i=0;i<nt->FileHeader.NumberOfSections;++i)
            if (!memcmp(sections[i].Name,".text",5))
            {
                auto address=FindUnique(reinterpret_cast<const unsigned char*>(base+sections[i].VirtualAddress),sections[i].Misc.VirtualSize,pattern);
                if (!address || leaf) return address;
                DWORD64 imageBase=0;
                const auto* entry=RtlLookupFunctionEntry(address,&imageBase,nullptr);
                return entry && imageBase+entry->BeginAddress==address ? address : 0;
            }
        return 0;
    }

#if defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
    bool MatchesCaptureRetirementBuild()
    {
        const uintptr_t base =
            reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (!base) return false;

        const auto readAt = [base](size_t offset, void* output, size_t bytes)
        {
            return offset <= UINTPTR_MAX - base &&
                MiniMapNative::ReadMemory(base + offset, output, bytes);
        };

        IMAGE_DOS_HEADER dos{};
        if (!readAt(0, &dos, sizeof(dos)) ||
            dos.e_magic != IMAGE_DOS_SIGNATURE ||
            dos.e_lfanew <= 0 ||
            dos.e_lfanew > 0x100000) return false;

        IMAGE_NT_HEADERS64 nt{};
        if (!readAt(static_cast<size_t>(dos.e_lfanew), &nt, sizeof(nt)) ||
            nt.Signature != IMAGE_NT_SIGNATURE ||
            nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;

        const auto& directory =
            nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
        const DWORD imageSize = nt.OptionalHeader.SizeOfImage;
        if (!directory.VirtualAddress || !directory.Size || directory.Size > 4096 ||
            directory.VirtualAddress > imageSize ||
            directory.Size > imageSize - directory.VirtualAddress ||
            directory.Size % sizeof(IMAGE_DEBUG_DIRECTORY)) return false;

        std::array<IMAGE_DEBUG_DIRECTORY,
            4096 / sizeof(IMAGE_DEBUG_DIRECTORY)> entries{};
        if (!readAt(directory.VirtualAddress, entries.data(), directory.Size)) return false;

        // RSDS GUID bytes in native little-endian storage, CL-127004 age 1.
        constexpr unsigned char identity[] = {
            0x52, 0x53, 0x44, 0x53, 0xa9, 0xae, 0xcb, 0x41,
            0xc3, 0x7a, 0xda, 0x99, 0xb9, 0x47, 0xa1, 0xdb,
            0x9c, 0x10, 0x21, 0xed, 1, 0, 0, 0
        };

        const size_t entryCount =
            directory.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
        for (size_t i = 0; i < entryCount; ++i)
        {
            const auto& entry = entries[i];
            if (entry.Type != IMAGE_DEBUG_TYPE_CODEVIEW ||
                entry.SizeOfData < sizeof(identity) ||
                entry.AddressOfRawData > imageSize ||
                entry.SizeOfData > imageSize - entry.AddressOfRawData) continue;

            unsigned char candidate[sizeof(identity)] = {};
            if (readAt(entry.AddressOfRawData, candidate, sizeof(candidate)) &&
                std::memcmp(candidate, identity, sizeof(identity)) == 0) return true;
        }
        return false;
    }
#endif

    const char* Status() { return status; }
    void SetStatus(const char* message) { status = message; }

    bool ResolvePrerequisites()
    {
        if (!MiniMapNative::IsInitialized())
        {
            status = "native prerequisites not resolved";
            return false;
        }
        status = "native signatures resolved; GPU submission remains blocked";
        return true;
    }

}
#endif
