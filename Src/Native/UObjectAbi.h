#pragma once

#include <cstddef>
#include <cstdint>

// Where the engine keeps an object's slot number and its slot's reference count.
// Both are UE object-table layout (UObjectBase::InternalIndex, FUObjectItem::RefCount),
// the same on every build of the engine version, so they are compiled in on purpose.
namespace MiniMapUObjectAbi
{
    inline constexpr size_t kInternalIndex = 0xc;
    inline constexpr size_t kItemRefCount = 0x14;

    // Address of the object's index in the object array.
    inline uintptr_t IndexAddress(uintptr_t object) { return object + kInternalIndex; }
    // Address of the reference count in the object array's item for the slot.
    inline uintptr_t RefCountAddress(uintptr_t item) { return item + kItemRefCount; }
}
