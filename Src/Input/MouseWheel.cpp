#if defined(MODLOADER_CLIENT_BUILD)

#include "MouseWheel.h"
#include "../DiagnosticExports.h"
#include "../plugin_helpers.h"
#include "../Config/Config.h"
#include "../UI/MiniMapUI.h"
#include "../Map/Map.h"
#include "../CacheIdentity.h"
#include "../Services/Input.h"
#include "../Services/Runtime.h"
#include "../Services/RuntimeEvents.h"

#include <atomic>
#include <cstdint>
#include <mutex>

namespace
{
    namespace S = MiniMap::Services;
    // Serialize acceptance with disable/clear/drain: an in-flight callback must
    // not refill the queue after a visibility transition or shutdown clears it.
    // No loader or native gameplay calls are made while holding this mutex.
    std::mutex g_inputMutex;
    bool g_accepting = false;
    bool g_configEnabled = false;
    uint32_t g_modifier = 0;
    uint64_t g_configRevision = 0;
    std::atomic<int32_t> g_pendingRawWheelDelta = 0;
    bool g_registered = false;
    S::Registration g_wheelRegistration{};
    S::Registration g_shutdownRegistration{};

    void RemoveShutdownRegistration()
    {
        if (!g_shutdownRegistration.id) return;
        if (auto* events = S::TryRuntimeEvents())
        {
            const auto removal = events->Remove(g_shutdownRegistration);
            if (removal.registry == S::RegistryRemoval::RequestReturned && removal.core == S::CoreDrain::Quiescent)
                g_shutdownRegistration = {};
        }
    }

    // Called only with g_inputMutex. These accessors read atomics, not Config.
    bool AcceptingZoom()
    {
        return g_accepting && g_configEnabled && g_modifier != 0 &&
            MiniMapUI::IsVisible() && MiniMapMap::GameplayReady();
    }

    bool OnMouseWheel(void* userData, const S::WheelEvent& event)
    {
        (void)userData;
        std::lock_guard<std::mutex> lock(g_inputMutex);
        if (!AcceptingZoom() || event.exclusiveUiCapture || event.verticalRaw == 0 ||
            event.horizontalRaw != 0 || (event.sidedModifiers & g_modifier) == 0)
        {
            return false;
        }

        // Ignore horizontal delta. Keep signed precision-wheel fractions until
        // drain, including cancellation of opposite directions.
        g_pendingRawWheelDelta.fetch_add(event.verticalRaw, std::memory_order_relaxed);
        return true;
    }

    void OnEngineShutdown(void*)
    {
        MiniMapMouseWheel::Shutdown();
        MiniMapCacheIdentity::ClearOnExit();
    }
}

namespace MiniMapMouseWheel
{
    bool Initialize(IPluginSelf* self)
    {
        if (g_registered)
        {
            return true;
        }
        if (self == nullptr || self->hooks == nullptr ||
            S::TryInput() == nullptr || !MiniMap::Adapters::AlienX::WheelInputAvailable() ||
            S::TryRuntimeEvents() == nullptr)
        {
            LOG_ERROR("MiniMap: v70 mouse-wheel input/shutdown interface unavailable");
            return false;
        }

        const auto settings = MiniMapConfig::GetMouseWheelSettings();
        {
            std::lock_guard lock(g_inputMutex);
            g_accepting = true;
            g_pendingRawWheelDelta.store(0, std::memory_order_relaxed);
        }
        Configure(settings.Enabled, settings.Modifier, settings.Revision);
        const auto receipt = S::TryInput()->OnWheel(&OnMouseWheel, nullptr);
        g_wheelRegistration = receipt.token;
        if (receipt.acceptance == S::Acceptance::Rejected || receipt.acceptance == S::Acceptance::BackendUnknown)
        {
            Shutdown();
            return false;
        }
        const auto shutdown = S::TryRuntimeEvents()->OnEngineShutdown(&OnEngineShutdown, nullptr);
        if (shutdown.token.id) g_shutdownRegistration = shutdown.token;
        if (shutdown.acceptance == S::Acceptance::Rejected || shutdown.acceptance == S::Acceptance::BackendUnknown)
        {
            Shutdown();
            return false;
        }
        g_registered = true;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        LOG_INFO("MiniMap: registered ModLoader mouse-wheel callback");
#endif
        return true;
    }

    void Shutdown()
    {
        {
            std::lock_guard lock(g_inputMutex);
            g_accepting = false;
            g_pendingRawWheelDelta.store(0, std::memory_order_relaxed);
        }
        if (!g_registered)
        {
            if (g_wheelRegistration.id)
            {
                if (auto* input = S::TryInput()) input->Remove(g_wheelRegistration);
                g_wheelRegistration = {};
            }
            RemoveShutdownRegistration();
            return;
        }

        // Plugin/engine lifecycle teardown runs while the existing self is valid.
        // The adapter retains closed wheel records for registry snapshots;
        // unregister is not an in-flight callback barrier or a DLL-unload fix.
        if (auto* input = S::TryInput()) input->Remove(g_wheelRegistration);
        g_wheelRegistration = {};
        RemoveShutdownRegistration();
        g_registered = false;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        LOG_INFO("MiniMap: unregistered ModLoader mouse-wheel callback");
#endif
    }

    void Configure(bool enabled, uint32_t modifier, uint64_t revision)
    {
        std::lock_guard<std::mutex> lock(g_inputMutex);
        if (revision < g_configRevision) return;
        if (!enabled || modifier != g_modifier || !AcceptingZoom())
        {
            g_pendingRawWheelDelta.store(0, std::memory_order_relaxed);
        }
        g_configRevision = revision;
        g_configEnabled = enabled;
        g_modifier = modifier;
    }

    void RefreshVisibility()
    {
        std::lock_guard lock(g_inputMutex);
        // Every visibility/readiness transition withdraws old queued input,
        // even if a newer reopening has already changed the visibility atomics.
        // Admission itself always reads the current atomics in AcceptingZoom.
        g_pendingRawWheelDelta.store(0, std::memory_order_relaxed);
    }

    float DrainZoomDelta()
    {
        std::lock_guard<std::mutex> lock(g_inputMutex);
        const int32_t rawDelta = g_pendingRawWheelDelta.exchange(0, std::memory_order_relaxed);
        return AcceptingZoom() ? static_cast<float>(rawDelta) / 120.0f : 0.0f;
    }

    void ClearPendingZoom()
    {
        std::lock_guard<std::mutex> lock(g_inputMutex);
        g_pendingRawWheelDelta.store(0, std::memory_order_relaxed);
    }
}

#endif
