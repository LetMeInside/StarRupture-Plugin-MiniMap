#pragma once

#include "ServiceTypes.h"

namespace MiniMap::Services
{
    struct Hook { std::uint64_t id = 0; };
    using PublishOriginalFn = void (*)(void* owner, void* callableOriginal) noexcept;
    struct NativeDetour
    {
        std::uintptr_t target;
        void* entry;
        PublishOriginalFn publishOriginal;
        void* owner;
    };
    enum class HookInstallState { Active, Rejected, BackendUnknown };
    struct HookInstallation
    {
        Hook handle;
        HookInstallState state;
        Status status;
    };
    class INativeHooks
    {
    public:
        virtual ~INativeHooks() = default;
        virtual HookInstallation Install(const NativeDetour&) = 0;
        virtual Removal Remove(Hook) = 0;
    };
}
