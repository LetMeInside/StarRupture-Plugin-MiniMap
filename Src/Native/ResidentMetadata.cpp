#if defined(MODLOADER_CLIENT_BUILD)
#include "ResidentMetadata.h"
#include "NativeApi.h"
#include "Services/Objects.h"
#include "Services/Runtime.h"
#include <Windows.h>

namespace
{
    namespace N = MiniMapNative;

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
}

namespace MiniMapResidentMetadata
{
    bool FindResident(SDK::UClass* type, const wchar_t* path, bool exact,
        uintptr_t& result) noexcept
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

    bool Available()
    {
        return QuietGameThread();
    }
}
#endif
