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
        // Stage 2.3 partial client binding. Move into the complete client
        // composition when its other services exist; never a mutable slot.
        IInput* const input;

        explicit Runtime(ILog& logService, IConfiguration* configuration = nullptr,
            const ClientServices* clientServices = nullptr, IInput* inputService = nullptr) noexcept
            : log(logService), config(configuration), client(clientServices), input(inputService) {}
        Runtime(const Runtime&) = delete;
        Runtime& operator=(const Runtime&) = delete;
    };

    const Runtime* TryRuntime() noexcept;
    inline IInput* TryInput() noexcept
    {
        const auto* runtime = TryRuntime();
        return runtime ? runtime->input : nullptr;
    }
    const Runtime& GetRuntime() noexcept; // Requires an active binding.
    const ClientServices& GetClientServices() noexcept; // Requires populated client.
}
