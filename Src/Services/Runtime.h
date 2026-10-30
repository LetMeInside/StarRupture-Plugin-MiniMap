#pragma once

#include "Log.h"

namespace MiniMap::Services
{
    class IConfiguration;
    class IRuntimeEvents;
    class IViewport;
    class ITextures;
    class IInput;
    class INativeHooks;
    class IObjects;

    // Final client composition shape only: no instance is supplied in Stage 2.1.
    struct ClientServices
    {
        IRuntimeEvents& events;
        IViewport& viewport;
        ITextures& textures;
        IInput& input;
        INativeHooks& hooks;
        IObjects& objects;
    };

    struct Runtime
    {
        ILog& log;
        // The selected adapter supplies configuration before core initialization.
        // Client services remain unbound until their migration stages.
        IConfiguration* const config;
        const ClientServices* const client;

        explicit Runtime(ILog& logService, IConfiguration* configuration = nullptr,
            const ClientServices* clientServices = nullptr) noexcept
            : log(logService), config(configuration), client(clientServices) {}
        Runtime(const Runtime&) = delete;
        Runtime& operator=(const Runtime&) = delete;
    };

    const Runtime* TryRuntime() noexcept;
    const Runtime& GetRuntime() noexcept; // Requires an active binding.
    const ClientServices& GetClientServices() noexcept; // Requires populated client.
}
