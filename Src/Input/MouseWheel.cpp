#if defined(MODLOADER_CLIENT_BUILD)

#include "MouseWheel.h"
#include "../plugin_helpers.h"
#include "../Config/Config.h"
#include "../UI/MiniMapUI.h"
#include "../Map/Map.h"
#include "../CacheIdentity.h"

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
    // No loader or native gameplay calls are made while holding this mutex.
    std::mutex g_inputMutex;
    bool g_accepting = false;
    bool g_configEnabled = false;
    uint32_t g_modifier = 0;
    uint64_t g_configRevision = 0;
    std::atomic<int32_t> g_pendingRawWheelDelta = 0;
    bool g_registered = false;

    // Called only with g_inputMutex. These accessors read atomics, not Config.
    bool AcceptingZoom()
    {
        return g_accepting && g_configEnabled && g_modifier != 0 &&
            MiniMapUI::IsVisible() && MiniMapMap::GameplayReady();
    }

    bool OnMouseWheel(const PluginMouseWheelEvent* event, void* userData)
    {
        (void)userData;
        if (event == nullptr || event->size < sizeof(PluginMouseWheelEvent))
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(g_inputMutex);
        if (!AcceptingZoom() || event->uiCapturing || event->rawDelta == 0 ||
            event->rawDeltaH != 0 || (event->modifiers & g_modifier) == 0)
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

        const auto settings = MiniMapConfig::GetMouseWheelSettings();
        {
            std::lock_guard lock(g_inputMutex);
            g_accepting = true;
            g_pendingRawWheelDelta.store(0, std::memory_order_relaxed);
        }
        Configure(settings.Enabled, settings.Modifier, settings.Revision);
        // The public registration API returns void; no success status is exposed.
        self->hooks->Input->RegisterMouseWheel(&OnMouseWheel, nullptr);
        self->hooks->Engine->RegisterOnShutdown(&OnEngineShutdown);
        g_registered = true;
        LOG_INFO("MiniMap: registered ModLoader mouse-wheel callback");
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
