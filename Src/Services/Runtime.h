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
        // Stage 2.4 partial client binding; client remains null until the full
        // immutable ClientServices composition is populated.
        IObjects* const objects;
        // Stage 2.6 partial client binding, fixed before any callback submission.
        IRuntimeEvents* const events;
        INativeHooks* const hooks;
        // Immutable partial bindings, fixed before core initialization.
        IViewport* const viewport;
        ITextures* const textures;

        explicit Runtime(ILog& logService, IConfiguration* configuration = nullptr,
            const ClientServices* clientServices = nullptr, IInput* inputService = nullptr,
            IObjects* objectsService = nullptr, IRuntimeEvents* eventService = nullptr,
            INativeHooks* hookService = nullptr, IViewport* viewportService = nullptr,
            ITextures* textureService = nullptr) noexcept
            : log(logService), config(configuration), client(clientServices), input(inputService), objects(objectsService),
              events(eventService), hooks(hookService), viewport(viewportService), textures(textureService) {}
        Runtime(const Runtime&) = delete;
        Runtime& operator=(const Runtime&) = delete;
    };

    const Runtime* TryRuntime() noexcept;
    inline ITextures* TryTextures() noexcept
    {
        const auto* runtime = TryRuntime();
        return runtime ? runtime->textures : nullptr;
    }
    inline IViewport* TryViewport() noexcept
    {
        const auto* runtime = TryRuntime();
        return runtime ? runtime->viewport : nullptr;
    }
    inline IRuntimeEvents* TryRuntimeEvents() noexcept
    {
        const auto* runtime = TryRuntime();
        return runtime ? runtime->events : nullptr;
    }
    inline INativeHooks* TryNativeHooks() noexcept
    {
        const auto* runtime = TryRuntime();
        return runtime ? runtime->hooks : nullptr;
    }
    inline IInput* TryInput() noexcept
    {
        const auto* runtime = TryRuntime();
        return runtime ? runtime->input : nullptr;
    }
    inline IObjects* TryObjects() noexcept
    {
        const auto* runtime = TryRuntime();
        return runtime ? runtime->objects : nullptr;
    }
    const Runtime& GetRuntime() noexcept; // Requires an active binding.
    const ClientServices& GetClientServices() noexcept; // Requires populated client.
}
