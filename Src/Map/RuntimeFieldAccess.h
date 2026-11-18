#pragma once

#include <cstddef>
#include <cstdint>

// Provides allocation-free access using offsets and strides published by the
// selected game adapter, without depending on that adapter's runtime helpers.
namespace MiniMap::MapRuntimeAccess
{
    // Returns the member address for a valid object and nonnegative offset.
    template<class T>
    inline T* Field(void* object, std::int32_t offset) noexcept
    {
        return offset < 0 || object == nullptr
            ? nullptr
            : reinterpret_cast<T*>(static_cast<std::uint8_t*>(object) + offset);
    }

    // Returns a const member address while preserving the input object's constness.
    template<class T>
    inline const T* Field(const void* object, std::int32_t offset) noexcept
    {
        return offset < 0 || object == nullptr
            ? nullptr
            : reinterpret_cast<const T*>(static_cast<const std::uint8_t*>(object) + offset);
    }

    // Reads one reflected bool bit using its byte offset and mask.
    inline bool GetBit(const void* object, std::int32_t offset, std::uint8_t mask) noexcept
    {
        return offset >= 0 && object != nullptr &&
            (static_cast<const std::uint8_t*>(object)[offset] & mask) != 0;
    }

    // Updates one reflected bool bit with the SDK helper's read-modify-write behavior.
    inline bool SetBit(void* object, std::int32_t offset, std::uint8_t mask, bool value) noexcept
    {
        if (offset < 0 || object == nullptr || mask == 0)
            return false;

        std::uint8_t& byte = static_cast<std::uint8_t*>(object)[offset];
        byte = value
            ? static_cast<std::uint8_t>(byte | mask)
            : static_cast<std::uint8_t>(byte & ~mask);
        return true;
    }

    // Returns one element at the supplied runtime stride after the SDK helper's guards.
    inline const std::uint8_t* ElementAt(
        const void* data,
        std::int32_t count,
        std::int32_t index,
        std::uint32_t stride) noexcept
    {
        if (data == nullptr || index < 0 || index >= count || stride == 0)
            return nullptr;

        return static_cast<const std::uint8_t*>(data) + static_cast<std::size_t>(index) * stride;
    }

    // Mutable overload with the same checks and address calculation.
    inline std::uint8_t* ElementAt(
        void* data,
        std::int32_t count,
        std::int32_t index,
        std::uint32_t stride) noexcept
    {
        return const_cast<std::uint8_t*>(ElementAt(
            static_cast<const void*>(data), count, index, stride));
    }
}
