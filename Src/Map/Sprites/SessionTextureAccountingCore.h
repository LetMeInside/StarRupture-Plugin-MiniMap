#pragma once
#include <cstddef>

namespace MiniMapSessionTextures
{
    struct Memory {size_t Count{},RawBytes{};};
    // Counts cached pixels, not backend overhead or old frame-held replacements.
    // Absence from the spatial registry is intentionally not a cache-eviction rule.
    template<class Slots> Memory Inspect(const Slots& slots) noexcept
    {
        Memory result;
        for(const auto& texture:slots)if(texture){++result.Count;result.RawBytes+=texture->RawBytes;}
        return result;
    }
}
