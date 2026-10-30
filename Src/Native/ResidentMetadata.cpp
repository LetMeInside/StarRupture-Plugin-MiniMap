#if defined(MODLOADER_CLIENT_BUILD)
#include "ResidentMetadata.h"
#include "NativeApi.h"
#include "DefinitionAcquisitionCore.h"
#include "Map/Map.h"
#include "Services/Objects.h"
#include "Services/Runtime.h"
#include "plugin_helpers.h"
#include <Engine_classes.hpp>
#include <Windows.h>
#include <cwchar>
#include <limits>

namespace
{
    namespace N = MiniMapNative;
    bool g_uncertain = false; // Fault latch, not an ownership/cache registry.
    struct SlotReader
    {
        template<class T> bool Read(uintptr_t address, T& value) { return N::ReadMemory(address, &value, sizeof(value)); }
    };
    bool Held(uintptr_t object)
    {
        int32_t index{}, count{}; MiniMapDefinitionAcquisition::Slot slot; SlotReader reader;
        return reader.Read(object + 0xc, index) &&
            MiniMapDefinitionAcquisition::ReadSlot(reader, reinterpret_cast<uintptr_t>(N::Get()->asset.objectArray), index, slot) == MiniMapDefinitionAcquisition::Status::Success &&
            slot.Object == object && (slot.Flags & 0x20000000) && reader.Read(slot.Item + 0x14, count) && count > 0;
    }
    void Uncertain(const char* reason)
    {
        if (!g_uncertain) LOG_ERROR("MiniMap: resident metadata ownership uncertain: %s; remaining roots retained and native reference operations disabled", reason);
        g_uncertain = true;
    }
    bool QuietGameThread() noexcept
    {
        const auto* b = N::Get();
        __try
        {
            return N::IsInitialized() && b && b->player.isGameThread && b->player.isGameThread() &&
                b->asset.objectArray && b->asset.retainObject && b->asset.releaseObject &&
                b->asset.exitPurge && !*b->asset.exitPurge && b->asset.activeGarbageCollection && !*b->asset.activeGarbageCollection &&
                b->world.isGarbageCollectingAndLocking && !b->world.isGarbageCollectingAndLocking() &&
                b->world.isIncrementalPurgePending && !b->world.isIncrementalPurgePending() &&
                b->world.isIncrementalReachabilityPending && !b->world.isIncrementalReachabilityPending();
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
    }
    bool Reference(uintptr_t object, bool retain) noexcept
    {
        __try
        {
            if (retain) N::Get()->asset.retainObject(reinterpret_cast<SDK::UObject*>(object));
            else N::Get()->asset.releaseObject(reinterpret_cast<SDK::UObject*>(object));
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
    }
}
namespace MiniMapResidentMetadata
{
    bool FindResident(SDK::UClass* type, const wchar_t* path, bool exact, uintptr_t& result) noexcept
    {
        auto* objects = MiniMap::Services::TryObjects();
        if (!objects || !path) return false;
        try
        {
            auto found = objects->FindResident(type, nullptr, path, exact);
            if (!found.value) return false;
            result = reinterpret_cast<uintptr_t>(*found.value);
            return true;
        }
        catch (...) { return false; }
    }
    bool Available() { return !g_uncertain && QuietGameThread(); }
    bool Validate(uintptr_t object, size_t extent)
    {
        const auto* b = N::Get();
        return b && N::ValidateObjectIdentity(object, reinterpret_cast<uintptr_t>(b->asset.objectArray), extent);
    }
    bool Serial(uintptr_t object, int32_t& serial)
    {
        int32_t index{}; MiniMapDefinitionAcquisition::Slot slot; SlotReader reader;
        if (!Available() || !Validate(object) || !reader.Read(object + 0xc, index) ||
            MiniMapDefinitionAcquisition::ReadSlot(reader, reinterpret_cast<uintptr_t>(N::Get()->asset.objectArray), index, slot) != MiniMapDefinitionAcquisition::Status::Success ||
            slot.Object != object || (slot.Flags & MiniMapDefinitionAcquisition::UnusableObjectItemFlags)) return false;
        serial = slot.Serial; return true;
    }
    bool Retain(uintptr_t object, size_t extent)
    {
        if (!Available() || !Validate(object, extent)) return false;
        if (!Reference(object, true) || !Validate(object, extent) || !Held(object))
        { Uncertain("retention interrupted or post-retain identity invalid"); return false; }
        return true;
    }
    bool Release(uintptr_t object)
    {
        if (!object) return true;
        if (!Available() || !Validate(object) || !Held(object) || !Reference(object, false))
        { Uncertain("release unavailable, identity invalid or invocation interrupted"); return false; }
        return true;
    }
    uintptr_t Acquire(const wchar_t* path, bool scriptStruct, size_t instanceSize)
    {
        if (!path || std::wcsncmp(path, L"/Script/", 8) || !instanceSize ||
            instanceSize > size_t((std::numeric_limits<int32_t>::max)()) || !MiniMapMap::GameplayReady() || !Available()) return 0;
        auto* objects = MiniMap::Services::TryObjects();
        if (!objects || !objects->Ready()) return 0;
        uintptr_t core{}, expected{}, object{}, actual{};
        bool coreHeld = false, expectedHeld = false;
        auto lookup = [&](uintptr_t type, const wchar_t* name, bool exact, uintptr_t& result)
        {
            if (FindResident(reinterpret_cast<SDK::UClass*>(type), name, exact, result)) return true;
            Uncertain("resident lookup interrupted"); return false;
        };
        const auto read = [](uintptr_t object, uintptr_t& type) { return N::ReadMemory(object + 0x10, &type, sizeof(type)); };
        if (!lookup(0, L"/Script/CoreUObject.Class", false, core) || !Validate(core, sizeof(SDK::UClass)) || !read(core, actual) || actual != core) return 0;
        coreHeld = Retain(core);
        if (!coreHeld) return 0;
        bool valid = true;
        if (scriptStruct)
        {
            valid = lookup(core, L"/Script/CoreUObject.ScriptStruct", true, expected) && Validate(expected, sizeof(SDK::UClass)) && read(expected, actual) && actual == core;
            if (valid) { expectedHeld = Retain(expected); valid = expectedHeld; }
        }
        else expected = core;
        int32_t size{};
        valid = valid && lookup(expected, path, true, object) && Validate(object, scriptStruct ? sizeof(SDK::UScriptStruct) : sizeof(SDK::UClass)) &&
            read(object, actual) && actual == expected && N::ReadMemory(object + offsetof(SDK::UStruct, Size), &size, sizeof(size)) && size == int32_t(instanceSize);
        if (valid) valid = Retain(object);
        // Explicit releases only. A fault latches acquisition off and preserves
        // all remaining native roots rather than speculatively unwinding them.
        if (expectedHeld && !Release(expected)) return 0;
        if (coreHeld && !Release(core)) return 0;
        return valid ? object : 0;
    }
}
#endif
