#include "RuntimeBinding.h"

#include <atomic>
#include <exception>

namespace
{
    std::atomic<const MiniMap::Services::Runtime*> g_runtime{nullptr};
}

namespace MiniMap::Services
{
    const Runtime* TryRuntime() noexcept
    {
        return g_runtime.load(std::memory_order_acquire);
    }

    const Runtime& GetRuntime() noexcept
    {
        const auto* runtime = TryRuntime();
        if (!runtime) std::terminate(); // Lifecycle precondition violation.
        return *runtime;
    }

    const ClientServices& GetClientServices() noexcept
    {
        const auto* client = GetRuntime().client;
        if (!client) std::terminate(); // Not available during Stage 2.1.
        return *client;
    }
}

namespace MiniMap::Services::Detail
{
    RuntimeBinding::RuntimeBinding(const Runtime& runtime) noexcept : Context(runtime)
    {
        const Runtime* expected = nullptr;
        Bound = g_runtime.compare_exchange_strong(expected, &Context,
            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    RuntimeBinding::~RuntimeBinding() noexcept
    {
        if (!Bound) return;
        const Runtime* expected = &Context;
        if (!g_runtime.compare_exchange_strong(expected, nullptr,
            std::memory_order_acq_rel, std::memory_order_acquire)) std::terminate();
    }
}
