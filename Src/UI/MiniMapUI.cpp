#include "../Map/Map.h"
#include "../CallbackTiming.h"
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

#include <atomic>
#include <cstdint>

namespace
{
    WidgetHandle g_widget = nullptr;

    std::atomic<bool> g_userVisible{false};
    std::atomic<uint32_t> g_blockingMenuMask{0};
    std::mutex g_bindingMutex;
    bool g_bindingsActive = false; // Protected by g_bindingMutex.
    std::atomic<bool> g_inputActive{false};
    MiniMapConfig::Keybind g_toggleIdentity{}, g_zoomInIdentity{}, g_zoomOutIdentity{};

    PluginWindowHints g_hints = {};

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

    void UpdateHints()
    {
        /*
         * The MiniMap draws its own background and border.
         *
         * Deliberately do not use:
         *
         *   NoMove
         *   NoResize
         *   NoSavedSettings
         *
         * This allows the player to move and resize the MiniMap while the
         * ModLoader UI/cursor is available, while ImGui persists the chosen
         * size and position between game sessions.
         */
        g_hints.extra_window_flags =
            PluginWindowFlags_NoTitleBar |
            PluginWindowFlags_NoScrollbar |
            PluginWindowFlags_NoBackground;

        /*
         * Default MiniMap dimensions.
         *
         * AlienX passes size_cond directly to ImGui as ImGuiCond.
         * ImGuiCond_FirstUseEver is 1 << 2, i.e. 4.
         *
         * Note: the current AlienX SDK comment incorrectly documents
         * FirstUseEver as 1. A value of 1 is actually ImGuiCond_Always,
         * which would force the MiniMap back to this size every frame.
         */
        g_hints.width = kWindowWidth;
        g_hints.height = kWindowHeight;
        g_hints.size_cond = 4; // ImGuiCond_FirstUseEver

        /*
         * Do not specify an initial window position yet.
         *
         * AlienX skips SetNextWindowPos() whenever either coordinate is
         * negative.
         */
        g_hints.pos_x = -1.0f;
        g_hints.pos_y = -1.0f;

        g_hints.pivot_x = 0.0f;
        g_hints.pivot_y = 0.0f;

        /*
         * Currently unused because positioning is skipped.
         * Keep the correct ImGui FirstUseEver value for when we implement
         * initial positioning later.
         */
        g_hints.pos_cond = 4; // ImGuiCond_FirstUseEver
    }

    void Render(
        IModLoaderImGui* ui)
    {
        MiniMapCallbackTiming::Scope callbackTiming(MiniMapCallbackTiming::Id::WidgetRender,true);
        if (ui == nullptr || !IsEffectivelyVisible())
        {
            return;
        }


        float windowX = 0.0f;
        float windowY = 0.0f;
        float windowWidth = 0.0f;
        float windowHeight = 0.0f;

        ui->GetWindowPos(
            &windowX,
            &windowY);

        ui->GetWindowSize(
            &windowWidth,
            &windowHeight);

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
        float availableWidth = 0.0f;
        float availableHeight = 0.0f;

        ui->GetContentRegionAvail(
            &availableWidth,
            &availableHeight);

        if (availableWidth > 0.0f &&
            availableHeight > 0.0f)
        {
            ui->Dummy(
                availableWidth,
                availableHeight);
        }


        PluginDrawList drawList =
            ui->GetWindowDrawList();

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

        ui->DL_AddRectFilled(
            drawList,
            windowX,
            windowY,
            windowX + windowWidth,
            windowY + windowHeight,
            backgroundColor,
            rounding,
            PluginDrawFlags_RoundCornersAll);

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

        ui->DL_AddRect(
            drawList,
            windowX,
            windowY,
            windowX + windowWidth,
            windowY + windowHeight,
            borderColor,
            rounding,
            PluginDrawFlags_RoundCornersAll,
            1.0f);

#if MINIMAP_DEBUG_UI
        const auto debugText = MiniMapDebugText::GetDebugText();
        if (debugText && !debugText->empty())
        {
            float textWidth = 0.0f, textHeight = 0.0f;
            ui->CalcTextSize(debugText->c_str(), &textWidth, &textHeight, false, -1.0f);
            const float x = windowX + kMapViewportInset;
            const float y = windowY + windowHeight + kMapViewportInset;
            const auto debugDrawList = ui->GetForegroundDrawList();
            ui->DL_AddRectFilled(debugDrawList, x - 4.0f, y - 4.0f,
                x + textWidth + 4.0f, y + textHeight + 4.0f,
                0xC0202020, 3.0f, PluginDrawFlags_RoundCornersAll);
            ui->DL_AddText(debugDrawList, x, y, 0xFFE0E0E0, debugText->c_str());
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

        if (g_self != nullptr &&
            g_self->hooks != nullptr &&
            g_self->hooks->UI != nullptr &&
            g_widget != nullptr)
        {
            g_self->hooks->UI->SetWidgetVisible(
                g_widget,
                effectiveVisible);
        }

        LOG_INFO(
            effectiveVisible
            ? "MiniMap: widget shown"
            : "MiniMap: widget hidden");
    }


    void OnToggleKeyPressed(
        EModKey key,
        EModKeyEvent event)
    {
        MiniMapCallbackTiming::Scope callbackTiming(MiniMapCallbackTiming::Id::ToggleKey,false);
        (void)key;
        (void)event;

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
    void OnZoomIn(EModKey, EModKeyEvent)
    {
        OnManualZoom(1.0f);
    }
    void OnZoomOut(EModKey, EModKeyEvent)
    {
        OnManualZoom(-1.0f);
    }
    void RegisterAbsent(const MiniMapConfig::Keybind& binding,
        MiniMapConfig::Keybind& identity, PluginKeybindCallback callback)
    {
        if (identity[0] || !binding[0]) return;
        GetSelf()->hooks->Input->RegisterKeybindByName(binding.data(), EModKeyEvent::Pressed, callback);
        identity = binding; // Loader retains this original identity across native rebinding.
    }

}

namespace MiniMapUI
{
    bool Initialize(
        IPluginSelf* self)
    {
        if (self == nullptr ||
            self->hooks == nullptr ||
            self->hooks->UI == nullptr)
        {
            LOG_ERROR(
                "MiniMap: UI initialization failed: "
                "UI interface is unavailable");

            return false;
        }

        if (g_widget != nullptr)
        {
            return true;
        }

        g_userVisible.store(false, std::memory_order_release);

        UpdateHints();

        static PluginWidgetDesc widgetDesc = {
            "MiniMap",
            &Render,
            &g_hints
        };

        g_widget =
            self->hooks->UI->RegisterWidget(
                &widgetDesc);

        if (g_widget == nullptr)
        {
            LOG_ERROR(
                "MiniMap: failed to register widget");

            return false;
        }

        /*
         * Registration and gameplay visibility are separate.
         *
         * The plugin lifecycle will explicitly Show() the widget when
         * the game world begins.
         */
        self->hooks->UI->SetWidgetVisible(
            g_widget,
            false);

        if (!self->hooks->Input || !self->hooks->Input->RegisterKeybindByName ||
            !self->hooks->Input->UnregisterKeybindByName)
        {
            self->hooks->UI->UnregisterWidget(
                g_widget);

            g_widget = nullptr;

            return false;
        }

        {
            std::lock_guard lock(g_bindingMutex);
            g_bindingsActive = true;
            g_inputActive.store(true, std::memory_order_release);
        }
        RefreshKeybinds();

        if (!MiniMapMouseWheel::Initialize(
            self))
        {
            LOG_ERROR(
                "MiniMap: mouse-wheel input initialization failed");

            StopInput();

            self->hooks->UI->UnregisterWidget(
                g_widget);

            g_widget = nullptr;

            return false;
        }

        LOG_INFO(
            "MiniMap: UI initialized");

        return true;
    }

    void RefreshKeybinds()
    {
        const auto settings = MiniMapConfig::Get(); // Before the registration lock.
        std::lock_guard lock(g_bindingMutex);
        if (!g_bindingsActive) return;
        RegisterAbsent(settings.ToggleMiniMap, g_toggleIdentity, &OnToggleKeyPressed);
        RegisterAbsent(settings.ZoomInKey, g_zoomInIdentity, &OnZoomIn);
        RegisterAbsent(settings.ZoomOutKey, g_zoomOutIdentity, &OnZoomOut);
    }

    void StopInput()
    {
        g_inputActive.store(false, std::memory_order_release);
        MiniMapMouseWheel::Shutdown();
        std::lock_guard lock(g_bindingMutex);
        g_bindingsActive = false;
        auto unregister = [](MiniMapConfig::Keybind& identity, PluginKeybindCallback callback)
        {
            if (identity[0]) GetSelf()->hooks->Input->UnregisterKeybindByName(
                identity.data(), EModKeyEvent::Pressed, callback);
            identity = {};
        };
        unregister(g_toggleIdentity, &OnToggleKeyPressed);
        unregister(g_zoomInIdentity, &OnZoomIn);
        unregister(g_zoomOutIdentity, &OnZoomOut);
    }

    void Shutdown()
    {
        StopInput();

        if (g_self != nullptr &&
            g_self->hooks != nullptr &&
            g_self->hooks->UI != nullptr &&
            g_widget != nullptr)
        {
            g_self->hooks->UI->SetWidgetVisible(
                g_widget,
                false);

            g_self->hooks->UI->UnregisterWidget(
                g_widget);
        }

        g_widget = nullptr;
        g_userVisible.store(false, std::memory_order_release);
        g_hints = {};
    }

    void Show()
    {
        if (g_self == nullptr ||
            g_self->hooks == nullptr ||
            g_self->hooks->UI == nullptr ||
            g_widget == nullptr)
        {
            return;
        }

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
