#if defined(MODLOADER_CLIENT_BUILD)

#include "MouseWheel.h"
#include "../plugin_helpers.h"

#include <atomic>
#include <cstdint>
#include <mutex>

// PluginInfo advertises the mandatory SDK version; the loader rejects unsupported
// tables before PluginInit. Pointer checks below are availability checks, not an
// older-interface compatibility probe.
static_assert(PLUGIN_INTERFACE_VERSION == 70, "Review the wheel ABI when updating the SDK");

namespace
{
    // Serialize acceptance with disable/clear/drain: an in-flight callback must
    // not refill the queue after a visibility transition or shutdown clears it.
    // No loader or gameplay calls are made while holding this mutex.
    std::mutex g_inputMutex;
    bool g_zoomEnabled = false;
    std::atomic<int32_t> g_pendingRawWheelDelta = 0;
    bool g_registered = false;

    bool OnMouseWheel(const PluginMouseWheelEvent* event, void* userData)
    {
        (void)userData;
        if (event == nullptr || event->size < sizeof(PluginMouseWheelEvent))
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(g_inputMutex);
        if (!g_zoomEnabled || event->uiCapturing || event->rawDelta == 0 ||
            (event->modifiers & PluginWheelMod_LeftCtrl) == 0)
        {
            return false;
        }

        // Ignore horizontal delta. Keep signed precision-wheel fractions until
        // drain, including cancellation of opposite directions.
        g_pendingRawWheelDelta.fetch_add(event->rawDelta, std::memory_order_relaxed);
        return true;
    }

    void OnEngineShutdown()
    {
        MiniMapMouseWheel::Shutdown();
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
            self->hooks->Input == nullptr ||
            self->hooks->Input->RegisterMouseWheel == nullptr ||
            self->hooks->Input->UnregisterMouseWheel == nullptr ||
            self->hooks->Engine == nullptr ||
            self->hooks->Engine->RegisterOnShutdown == nullptr ||
            self->hooks->Engine->UnregisterOnShutdown == nullptr)
        {
            LOG_ERROR("MiniMap: v70 mouse-wheel input/shutdown interface unavailable");
            return false;
        }

        SetZoomEnabled(false);
        // The public registration API returns void; no success status is exposed.
        self->hooks->Input->RegisterMouseWheel(&OnMouseWheel, nullptr);
        self->hooks->Engine->RegisterOnShutdown(&OnEngineShutdown);
        g_registered = true;
        LOG_INFO("MiniMap: registered ModLoader mouse-wheel callback");
        return true;
    }

    void Shutdown()
    {
        SetZoomEnabled(false);
        if (!g_registered)
        {
            return;
        }

        // Plugin/engine lifecycle teardown runs while the existing self is valid.
        // Static callback state and null userData also survive registry snapshots;
        // unregister is not an in-flight callback barrier or a DLL-unload fix.
        auto* self = GetSelf();
        self->hooks->Input->UnregisterMouseWheel(&OnMouseWheel, nullptr);
        self->hooks->Engine->UnregisterOnShutdown(&OnEngineShutdown);
        g_registered = false;
        LOG_INFO("MiniMap: unregistered ModLoader mouse-wheel callback");
    }

    void SetZoomEnabled(bool enabled)
    {
        std::lock_guard<std::mutex> lock(g_inputMutex);
        g_zoomEnabled = enabled;
        if (!enabled)
        {
            g_pendingRawWheelDelta.store(0, std::memory_order_relaxed);
        }
    }

    float DrainZoomDelta()
    {
        std::lock_guard<std::mutex> lock(g_inputMutex);
        const int32_t rawDelta = g_pendingRawWheelDelta.exchange(0, std::memory_order_relaxed);
        return static_cast<float>(rawDelta) / 120.0f;
    }

    void ClearPendingZoom()
    {
        std::lock_guard<std::mutex> lock(g_inputMutex);
        g_pendingRawWheelDelta.store(0, std::memory_order_relaxed);
    }
}

#endif
