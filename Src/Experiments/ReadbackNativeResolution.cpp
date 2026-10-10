#if defined(MODLOADER_CLIENT_BUILD) && (defined(MINIMAP_ASYNC_READBACK_EXPERIMENT) || defined(MINIMAP_STAGE_R2_SMELTER_CAPTURE))
#include "ReadbackNativeAdapter.h"
#include "ReadbackNativeBindings.h"
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
        Bindings bindings{};
        const char* status = "native prerequisites not resolved";
        struct Pattern
        {
            const char* Name;
            const char* Bytes;
            uintptr_t Bindings::* Field;
            int Displacement, InstructionSize;
            bool Leaf;
        };
        // CL-127004: instruction-aligned, relocations masked, independently
        // checked unique against .text. No runtime RVA arithmetic.
        const Pattern patterns[] = {
        { "FromValidEName", "48 89 5C 24 08 57 48 83 EC 20 80 3D ?? ?? ?? ?? 00 48 8B D9 48 8D 0D ?? ?? ?? ?? 8B FA 75 ?? E8 ?? ?? ?? ?? 48 8B C8 C6 05 ?? ?? ?? ?? 01 8B 8C B9 40 40 01 00 48 8B C3 89 0B 48 8B 5C 24 30 48 83 C4 20 5F C3", &Bindings::FromValidEName, -1, 0, false },
        { "Submit", "48 89 5C 24 18 48 89 6C 24 20 56 57 41 54 41 56 41 57 48 81 EC 80 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 70 4C 8B B4 24 D0 00 00 00 48 8B D9 48 89 4C 24 38", &Bindings::Submit, -1, 0, false },
        { "Immediate", "48 8D 05 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC 48 89 5C 24 08 57 48 83 EC 30 33 C0 48 63 DA 44 8B C3 48 8B F9 48 63 49 28 41 F7 D0 41 C1 E8 1F 3B D9 41 0F 4C C0 85 C0", &Bindings::Immediate, -1, 0, true },
        { "Create", "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 40 49 83 79 48 00 49 8B F9 48 8B 01 49 8B F0 4C 8B F2 48 C7 44 24 20 00 00 00 00 41 0F 95 C1 48 8D 54 24 30", &Bindings::Create, -1, 0, false },
        { "Construct", "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 89 03 33 C0 48 89 43 18 48 89 43 20 48 89 43 28 48 89 43 30 48 89 43 38 48 89 43 40 48 89 43 48 48 89 43 50 48 8B C3 48 83 C4 20 5B C3 CC CC CC CC CC CC CC CC CC CC CC CC CC 48 89 7C 24 08", &Bindings::Construct, -1, 0, false },
        { "Destroy", "48 89 5C 24 08 57 48 83 EC 20 48 8B 79 50 48 8B D9 48 85 FF 74 ?? 48 8D 4F 08 BA 03 00 00 00 E8 ?? ?? ?? ?? 85 C0 79 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 53 00 00 00 48 8D 15 ?? ?? ?? ??", &Bindings::Destroy, -1, 0, false },
        { "Copy", "40 55 57 41 54 41 56 48 8D AC 24 D8 FD FF FF 48 81 EC 28 03 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 F0 01 00 00 4C 8B B5 78 02 00 00 48 8B F9 48 89 4C 24 60 4C 8B E2", &Bindings::Copy, -1, 0, false },
        { "Lock", "4C 8B DC 49 89 5B 10 57 48 83 EC 50 0F BC 41 10 41 B9 20 00 00 00 49 8B D8 48 8B FA 48 8B D1 44 0F 45 C8 33 C0 4A 39 44 C9 18 4E 8D 04 C9 74 ?? 49 89 43 E8 89 44 24 78", &Bindings::Lock, -1, 0, false },
        { "Unlock", "40 53 48 83 EC 30 8B 41 14 48 8B D9 48 83 7C C1 18 00 75 ?? 48 8D 05 ?? ?? ?? ?? 41 B9 0C 01 00 00 4C 8D 05 ?? ?? ?? ?? 48 89 44 24 20 33 D2 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ??", &Bindings::Unlock, -1, 0, false },
        { "Poll", "48 8B C1 BA 01 00 00 00 8B 0D ?? ?? ?? ?? D3 E2 48 8B C8 FF CA E9 ?? ?? ?? ?? CC CC CC CC CC CC 40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 88 FB FF FF 48 81 EC 78 05 00 00", &Bindings::Poll, -1, 0, true },
        { "MarkForDelete", "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 0F 0D 49 08 8B 41 08 0F 1F 40 00 0F 1F 84 00 00 00 00 00 8B D0 0F BA EA 1E F0 0F B1 51 08 75 ?? 8B F8 85 C0 79 ?? 4C 8D 0D ?? ?? ?? ??", &Bindings::MarkForDelete, -1, 0, false },
        { "Malloc", "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 8B DA 48 8B 0D ?? ?? ?? ?? 48 85 C9 75 ?? E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 44 8B C3 48 8B D7 48 8B 5C 24 30 48 83 C4 20 5F 48 FF 60 28", &Bindings::Malloc, -1, 0, false },
        { "Free", "48 85 C9 74 ?? 53 48 83 EC 20 48 8B D9 48 8B 0D ?? ?? ?? ?? 48 85 C9 75 ?? E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 48 8B D3 FF 50 48 48 83 C4 20 5B C3 CC CC CC CC", &Bindings::Free, -1, 0, false },
        { "DynamicRHI", "48 8B 0D ?? ?? ?? ?? 4C 8D 44 24 20 C7 44 24 20 01 03 01 07 48 8D 54 24 40 C7 44 24 24 00 00 02 00 C7 44 24 28 07 00 00 00 66 C7 44 24 2C FF F4 48 8B 01 FF 50 58 48 8B 1D ?? ?? ?? ??", &Bindings::DynamicRHI, 3, 7, false },
        { "Pipe", "48 8D 0D ?? ?? ?? ?? 48 89 45 E7 48 8D 45 E7 48 89 44 24 20 E8 ?? ?? ?? ?? 48 39 7D E7 0F 84 ?? ?? ?? ?? 48 8B 45 0F 48 8D 4D F7 48 85 C0 48 0F 45 C8 48 8B 01 FF 50 10", &Bindings::Pipe, 3, 7, false },
        { "Threaded", "80 3D ?? ?? ?? ?? 00 74 ?? 8B 05 ?? ?? ?? ?? 83 F8 01 75 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 E5 00 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 8B 05 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B D0 48 8D 8B B0 00 00 00 E8 ?? ?? ?? ?? 48 83 BB C0 00 00 00 00", &Bindings::Threaded, 2, 7, false },
        { "Multithreaded", "80 3D ?? ?? ?? ?? 00 41 8B F9 49 8B E8 48 8B F2 74 ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 4C 8B 88 38 02 00 00 83 FF FF 75 ?? 0F BC 82 BC 01 00 00 BF 20 00 00 00 0F 45 F8 44 8B C7", &Bindings::Multithreaded, 2, 7, false },
        { "GpuCount", "8B 0D ?? ?? ?? ?? D3 E2 48 8B C8 FF CA E9 ?? ?? ?? ?? CC CC CC CC CC CC 40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 88 FB FF FF 48 81 EC 78 05 00 00 48 8B 05 ?? ?? ?? ??", &Bindings::GpuCount, 2, 6, false },
        };

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

#if defined(MINIMAP_STAGE_R2_SMELTER_CAPTURE)
    bool MatchesCaptureRetirementBuild()
    {
        const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (!base) return false;
        const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic!=IMAGE_DOS_SIGNATURE || dos->e_lfanew<=0 || dos->e_lfanew>0x100000) return false;
        const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
        if (nt->Signature!=IMAGE_NT_SIGNATURE || nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 ||
            nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
        const auto& dir=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
        const auto size=nt->OptionalHeader.SizeOfImage;
        if (!dir.VirtualAddress || dir.Size>4096 || dir.VirtualAddress>size || dir.Size>size-dir.VirtualAddress ||
            dir.Size%sizeof(IMAGE_DEBUG_DIRECTORY)) return false;
        const auto* entries=reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(base+dir.VirtualAddress);
        // RSDS GUID bytes in native little-endian storage, CL-127004 age 1.
        const unsigned char identity[]{0x52,0x53,0x44,0x53,0xa9,0xae,0xcb,0x41,0xc3,0x7a,0xda,0x99,
            0xb9,0x47,0xa1,0xdb,0x9c,0x10,0x21,0xed,1,0,0,0};
        for (size_t i=0;i<dir.Size/sizeof(*entries);++i)
        {
            const auto& entry=entries[i];
            if (entry.Type==IMAGE_DEBUG_TYPE_CODEVIEW && entry.SizeOfData>=sizeof(identity) &&
                entry.AddressOfRawData<=size && entry.SizeOfData<=size-entry.AddressOfRawData &&
                !std::memcmp(reinterpret_cast<const void*>(base+entry.AddressOfRawData),identity,sizeof(identity))) return true;
        }
        return false;
    }
#endif

    const Bindings& GetBindings() { return bindings; }
    const char* Status() { return status; }
    void SetStatus(const char* message) { status = message; }

    bool ResolvePrerequisites()
    {
        bindings = {};
        status = "invalid native executable image";
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
        const auto* sections = IMAGE_FIRST_SECTION(nt);
        const unsigned char* text = nullptr;
        size_t length = 0;
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
            if (memcmp(sections[i].Name, ".text", 5) == 0)
            {
                text = reinterpret_cast<const unsigned char*>(base + sections[i].VirtualAddress);
                length = sections[i].Misc.VirtualSize;
                break;
            }
        if (!text) return false;

        // The v70 scanner's OPTIONAL flag only changes its label: a miss still
        // refuses the plugin. Preflight privately to keep R1 genuinely optional.
        // This runs once during native resolution, never in a tick/Ui callback.
        Bindings candidate{};
        for (const auto& pattern : patterns)
        {
            auto address = FindUnique(text, length, pattern.Bytes);
            if (!address) { status = pattern.Name; return false; }
            if (pattern.Displacement >= 0)
            {
                int32_t displacement;
                memcpy(&displacement, reinterpret_cast<const void*>(address + pattern.Displacement), 4);
                address += pattern.InstructionSize + static_cast<intptr_t>(displacement);
                bool valid = false;
                for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
                {
                    const auto& section = sections[i];
                    if (address >= base + section.VirtualAddress &&
                        address + 8 <= base + section.VirtualAddress + section.Misc.VirtualSize &&
                        (section.Characteristics & IMAGE_SCN_MEM_READ) &&
                        !(section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) valid = true;
                }
                if (!valid) { status = pattern.Name; return false; }
            }
            else if (!pattern.Leaf)
            {
                DWORD64 imageBase = 0;
                const auto* entry = RtlLookupFunctionEntry(address, &imageBase, nullptr);
                if (!entry || imageBase + entry->BeginAddress != address)
                { status = pattern.Name; return false; }
            }
            candidate.*(pattern.Field) = address;
        }
        bindings = candidate;
        status = "native signatures resolved; GPU submission remains blocked";
        return true;
    }
}
#endif
