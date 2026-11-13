#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include <cstdint>

namespace SDK { class UClass; }

namespace MiniMapResidentMetadata
{
    // Synchronous game-thread operations only; no loading or object-table scan.
    bool Available();
    // Loader-neutral synchronous lookup; callers retain their existing
    // identity, GC, admission, and native-root checks.
    bool FindResident(SDK::UClass* type, const wchar_t* path, bool exact, uintptr_t& object) noexcept;
}
#endif
