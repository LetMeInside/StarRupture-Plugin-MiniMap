#pragma once
#include <cstdint>

namespace MiniMapDefinitionAcquisition
{
    // FUObjectItem internal flags, NOT UObject::EObjectFlags. Verified against
    // UE_5.6 CoreUObject/Public/UObject/ObjectMacros.h EInternalObjectFlags;
    // these are the existing CL-127004 identity mask bits. Garbage must not be
    // referenced; Unreachable is outside the live object graph. Preserve both.
    enum class ObjectItemFlag : uint32_t { Garbage=uint32_t(1)<<21, Unreachable=uint32_t(1)<<28 };
    inline constexpr uint32_t UnusableObjectItemFlags=
        uint32_t(ObjectItemFlag::Garbage)|uint32_t(ObjectItemFlag::Unreachable);
}
