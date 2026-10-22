#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include <cstddef>
#include <cstdint>

namespace MiniMapResidentMetadata
{
    // Synchronous game-thread operations only; no loading or object-table scan.
    bool Available();
    // Shared existing ModLoader resident lookup operation, also used by capture.
    // No binding/cache/load is introduced; callers provide the established
    // Engine lookup address and own admission, identity and retention checks.
    bool FindResident(uintptr_t address, uintptr_t type, const wchar_t* path, bool exact, uintptr_t& object) noexcept;
    bool Validate(uintptr_t object, size_t extent = 0x28);
    bool Serial(uintptr_t object, int32_t& serial);
    bool Retain(uintptr_t object, size_t extent = 0x28);
    // False preserves the caller's ownership record; an interrupted release is
    // never invoked again. Remaining native roots are conservatively retained.
    bool Release(uintptr_t object);
    // Success transfers exactly one native reference to the caller.
    uintptr_t Acquire(const wchar_t* path, bool scriptStruct, size_t instanceSize);
}
#endif
