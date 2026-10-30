#include "Services/Viewport.h"
#include "../Map/Map.h"
#if defined(MODLOADER_CLIENT_BUILD)

#include "MiniMapUI.h"
#include "Config/Config.h"
#include <mutex>
#include "MapOverlays.h"
#include "DebugText.h"
#include "Input/MouseWheel.h"
#include "Map/FogOfWar/FogOfWar.h"
#include "Map/Foundables/Foundables.h"
#include "Map/AlienActors/AlienActors.h"
#include "Map/Buildings/BuildingCollector.h"
#include "Map/Buildings/BuildingVisuals.h"
#include "Map/PointsOfInterest/PointsOfInterest.h"
#include "Map/Terrain/Terrain.h"

#include "plugin.h"
#include "plugin_helpers.h"
#include "Services/Input.h"
#include "Services/Runtime.h"

#include <atomic>
#include <cstdint>

namespace
{
    namespace S = MiniMap::Services;
    S::Widget g_widget{};
    S::Acceptance g_widgetAcceptance = S::Acceptance::Rejected;

    std::atomic<bool> g_userVisible{false};
    std::atomic<uint32_t> g_blockingMenuMask{0};
    std::mutex g_bindingMutex;
    bool g_bindingsActive = false; // Protected by g_bindingMutex.
    std::atomic<bool> g_inputActive{false};
    S::Registration g_toggleBinding{}, g_zoomInBinding{}, g_zoomOutBinding{};


    constexpr float kWindowWidth = 300.0f;
    constexpr float kWindowHeight = 300.0f;

    // Keep all map content slightly inside the widget boundary. This outer
    // strip is reserved for the current/future MiniMap frame.
    constexpr float kMapViewportInset = 4.0f;


    bool IsEffectivelyVisible()
    {
        return g_userVisible.load(std::memory_order_acquire) &&
            g_blockingMenuMask.load(std::memory_order_acquire) == 0;
    }

    void RemoveWidget()
    {
        if (g_widget.id) if (auto* viewport = S::TryViewport())
        {
            const auto removal = viewport->Remove(g_widget);
            if (removal.registry == S::RegistryRemoval::RequestReturned &&
                removal.core == S::CoreDrain::Quiescent)
            {
                g_widget = {};
                g_widgetAcceptance = S::Acceptance::Rejected;
            }
        }
    }

    void Render(void*, S::ICanvas& ui)
    {
        if (!IsEffectivelyVisible())
        {
            return;
        }


        const auto position = ui.WindowPosition();
        const auto size = ui.WindowSize();
        const float windowX = position.x;
        const float windowY = position.y;
        const float windowWidth = size.x;
        const float windowHeight = size.y;

        if (windowWidth <= 1.0f ||
            windowHeight <= 1.0f)
        {
            return;
        }

        const float viewportX =
            windowX +
            kMapViewportInset;

        const float viewportY =
            windowY +
            kMapViewportInset;

        const float viewportWidth =
            windowWidth -
            kMapViewportInset *
            2.0f;

        const float viewportHeight =
            windowHeight -
            kMapViewportInset *
            2.0f;

        if (viewportWidth <= 1.0f ||
            viewportHeight <= 1.0f)
        {
            return;
        }

        /*
         * Reserve the complete content area.
         *
         * Although the visible widget is painted through the draw list,
         * ImGui still needs an item covering the window so that the
         * window behaves as a normal draggable widget.
         */
        const auto available = ui.ContentAvailable();
        if (available.x > 0.0f && available.y > 0.0f)
            ui.ReserveContent(available);

        MiniMap::Services::DrawLayer drawList =
            MiniMap::Services::DrawLayer::Window;

        /*
         * Stage 1 placeholder.
         *
         * These colors use ImGui's packed ABGR representation:
         * 0xAABBGGRR.
         */
        constexpr uint32_t backgroundColor =
            0xD0201818u;

        constexpr uint32_t borderColor =
            0x80606060u;

        constexpr float rounding = 8.0f;

        ui.FilledRect(drawList, {windowX, windowY}, {windowX + windowWidth, windowY + windowHeight}, backgroundColor, rounding);

        const auto settings = MiniMapConfig::GetRenderSettings();
        MiniMapMap::Transform transform = {};

        if (MiniMapTerrain::Render(
            ui,
            viewportX,
            viewportY,
            viewportWidth,
            viewportHeight,
            transform,
            settings))
        {
            MiniMapBuildingVisuals::Render(ui, transform, settings.Buildings);
            MiniMapFogOfWar::Render(
                ui,
                transform, settings.Fog);

            MiniMapAlienActors::SetViewport(transform);
            MiniMapBuildingCollector::SetViewport(transform);

            if (settings.Foundables.ShowLayer)
            {
                MiniMapFoundables::Render(
                    ui,
                    transform, settings.Foundables, settings.Fog);
            }

            if (settings.POI.ShowLayer)
            {
                MiniMapPointsOfInterest::Render(
                    ui,
                    transform, settings.POI, settings.Fog);
            }

            if (settings.ShowAlienActors)
            {
                MiniMapAlienActors::Render(ui, transform, settings.Fog);
            }

            MiniMapOverlays::Render(
                ui,
                transform, settings.ShowCoordinates, settings.CoordsInUEUnits, settings.ShowCompass, settings.Fog);
        }

        ui.Rect(drawList, {windowX, windowY}, {windowX + windowWidth, windowY + windowHeight}, borderColor, rounding, 1.0f);

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        const auto debugText = MiniMapDebugText::GetDebugText();
        if (debugText && !debugText->empty())
        {
            float textWidth = 0.0f, textHeight = 0.0f;
            { const auto measured = ui.MeasureText(debugText->c_str(), false, -1.0f); textWidth = measured.x; textHeight = measured.y; };
            const float x = windowX + kMapViewportInset;
            const float y = windowY + windowHeight + kMapViewportInset;
            const auto debugDrawList = MiniMap::Services::DrawLayer::Foreground;
            ui.FilledRect(debugDrawList, {x - 4.0f, y - 4.0f}, {x + textWidth + 4.0f, y + textHeight + 4.0f}, 0xC0202020, 3.0f);
            ui.Text(debugDrawList, {x, y}, 0xFFE0E0E0, debugText->c_str());
        }
#endif
    }

    void ApplyVisibility()
    {
        MiniMapMouseWheel::RefreshVisibility();
        // Reopening must wait for a fresh game-thread sample, even if no tick
        // occurred during a hidden interval. No native reads on this UI path.
        MiniMapTerrain::InvalidatePresentation();
        const bool visible = g_userVisible.load(std::memory_order_acquire);
        const bool effectiveVisible = visible &&
            g_blockingMenuMask.load(std::memory_order_acquire) == 0;
        if (g_widget.id) if (auto* viewport = S::TryViewport())
            viewport->SetVisible(g_widget, effectiveVisible);

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        LOG_INFO(
            effectiveVisible
            ? "MiniMap: widget shown"
            : "MiniMap: widget hidden");
#endif
    }


    void OnToggleKeyPressed(void*)
    {
        if (!g_inputActive.load(std::memory_order_acquire)) return;
        bool visible = g_userVisible.load(std::memory_order_acquire);
        while (!g_userVisible.compare_exchange_weak(
            visible, !visible, std::memory_order_acq_rel, std::memory_order_acquire))
        {
        }
        ApplyVisibility();
    }

    void OnManualZoom(float delta)
    {
        const bool active = g_inputActive.load(std::memory_order_acquire);
        const bool visible = IsEffectivelyVisible();
        const bool ready = MiniMapMap::GameplayReady();
        if (active && visible && ready)
        {
            MiniMapTerrain::AdjustZoom(delta);
        }
    }
    void OnZoomIn(void*)
    {
        OnManualZoom(1.0f);
    }
    void OnZoomOut(void*)
    {
        OnManualZoom(-1.0f);
    }
    bool RegisterAbsent(const MiniMapConfig::Keybind& binding,
        S::Registration& token, S::SettingName preference, S::ActionFn callback)
    {
        if (token.id || !binding[0]) return true;
        auto* input = S::TryInput();
        if (!input) return false;
        const auto receipt = input->BindAction({preference, binding.data()}, callback, nullptr);
        token = receipt.token; // Retain cancellation identity even on ambiguous submission.
        return receipt.acceptance == S::Acceptance::AcceptedUnverified ||
            receipt.acceptance == S::Acceptance::VerifiedOperational;
    }

}

namespace MiniMapUI
{
    bool Initialize(
        IPluginSelf* self)
    {
        auto* viewport = S::TryViewport();
        if (!viewport)
        {
            LOG_ERROR("MiniMap: UI initialization failed: viewport service unavailable");
            return false;
        }
        if (g_widget.id)
            return g_widgetAcceptance == S::Acceptance::AcceptedUnverified ||
                g_widgetAcceptance == S::Acceptance::VerifiedOperational;
        g_userVisible.store(false, std::memory_order_release);
        const auto receipt = viewport->CreateOverlay(
            {"MiniMap", {kWindowWidth, kWindowHeight}, std::nullopt}, &Render, nullptr);
        g_widget = receipt.token; // Also retain cancellation identity on ambiguity.
        g_widgetAcceptance = receipt.acceptance;
        if (receipt.acceptance != S::Acceptance::AcceptedUnverified &&
            receipt.acceptance != S::Acceptance::VerifiedOperational)
        {
            LOG_ERROR("MiniMap: failed to register widget");
            RemoveWidget();
            return false;
        }
        // Registration is separate from gameplay visibility, exactly as before.
        viewport->SetVisible(g_widget, false);

        if (!S::TryInput() || !MiniMap::Adapters::AlienX::KeyInputAvailable())
        {
            RemoveWidget();

            return false;
        }

        {
            std::lock_guard lock(g_bindingMutex);
            g_bindingsActive = true;
            g_inputActive.store(true, std::memory_order_release);
        }
        if (!RefreshKeybinds())
        {
            StopInput();
            RemoveWidget();
            return false;
        }

        if (!MiniMapMouseWheel::Initialize(
            self))
        {
            LOG_ERROR(
                "MiniMap: mouse-wheel input initialization failed");

            StopInput();

            RemoveWidget();

            return false;
        }

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        LOG_INFO(
            "MiniMap: UI initialized");
#endif

        return true;
    }

    bool RefreshKeybinds()
    {
        const auto settings = MiniMapConfig::Get(); // Before the registration lock.
        std::lock_guard lock(g_bindingMutex);
        if (!g_bindingsActive) return true;
        const bool toggle = RegisterAbsent(settings.ToggleMiniMap, g_toggleBinding, {"General", "ToggleMiniMap"}, &OnToggleKeyPressed);
        const bool zoomIn = RegisterAbsent(settings.ZoomInKey, g_zoomInBinding, {"Zoom", "ZoomInKey"}, &OnZoomIn);
        const bool zoomOut = RegisterAbsent(settings.ZoomOutKey, g_zoomOutBinding, {"Zoom", "ZoomOutKey"}, &OnZoomOut);
        return toggle && zoomIn && zoomOut;
    }

    void StopInput()
    {
        g_inputActive.store(false, std::memory_order_release);
        MiniMapMouseWheel::Shutdown();
        std::lock_guard lock(g_bindingMutex);
        g_bindingsActive = false;
        auto unregister = [](S::Registration& token)
        {
            if (token.id) if (auto* input = S::TryInput()) input->Remove(token);
            token = {};
        };
        unregister(g_toggleBinding);
        unregister(g_zoomInBinding);
        unregister(g_zoomOutBinding);
    }

    void Shutdown()
    {
        StopInput();
        if (g_widget.id) if (auto* viewport = S::TryViewport())
            viewport->SetVisible(g_widget, false);
        RemoveWidget();
        g_userVisible.store(false, std::memory_order_release);
    }

    void Show()
    {
        if (!S::TryViewport() || !g_widget.id) return;

        g_userVisible.store(true, std::memory_order_release);

        ApplyVisibility();
    }

    void Hide()
    {
        g_userVisible.store(false, std::memory_order_release);

        ApplyVisibility();
    }

    void RefreshVisibility(){ApplyVisibility();}

    uint32_t SetBlockingMenuActive(uint32_t menuBit, bool active)
    {
        const uint32_t previous = active
            ? g_blockingMenuMask.fetch_or(menuBit, std::memory_order_acq_rel)
            : g_blockingMenuMask.fetch_and(~menuBit, std::memory_order_acq_rel);
        const uint32_t current = active
            ? previous | menuBit
            : previous & ~menuBit;

        if (current != previous)
        {
            ApplyVisibility();
        }

        return current;
    }

    uint32_t ClearBlockingMenuSuppression()
    {
        return g_blockingMenuMask.exchange(0, std::memory_order_acq_rel);
    }

    bool IsVisible()
    {
        return IsEffectivelyVisible();
    }
}

#endif
