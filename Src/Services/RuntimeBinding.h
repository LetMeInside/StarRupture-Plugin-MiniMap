#pragma once

#include "Runtime.h"

// Bootstrap-only lifetime mechanism. Consumers use Runtime.h, not this header.
// Binds the whole immutable composition, never individual service replacements.
namespace MiniMap::Services::Detail
{
    class RuntimeBinding
    {
        const Runtime& Context;
        bool Bound = false;
    public:
        explicit RuntimeBinding(const Runtime& runtime) noexcept;
        ~RuntimeBinding() noexcept;
        RuntimeBinding(const RuntimeBinding&) = delete;
        RuntimeBinding& operator=(const RuntimeBinding&) = delete;
        bool IsBound() const noexcept { return Bound; }
    };
}
