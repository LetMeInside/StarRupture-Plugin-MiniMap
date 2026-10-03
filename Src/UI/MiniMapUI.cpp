#include "MiniMapUI.h"

#include "plugin.h"
#include "plugin_helpers.h"

#include <cstdint>

namespace
{
    WidgetHandle g_widget = nullptr;

    bool g_visible = false;
    bool g_keybindRegistered = false;

    PluginWindowHints g_hints = {};

    constexpr float kWindowWidth = 300.0f;
    constexpr float kWindowHeight = 300.0f;

    constexpr const char* kToggleKey = "F4";

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
        if (ui == nullptr ||
            !g_visible)
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

        constexpr uint32_t textColor =
            0xFFE8E8E8u;

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

        constexpr const char* placeholderText =
            "MiniMap";

        float textWidth = 0.0f;
        float textHeight = 0.0f;

        ui->CalcTextSize(
            placeholderText,
            &textWidth,
            &textHeight,
            false,
            0.0f);

        const float textX =
            windowX +
            (windowWidth - textWidth) * 0.5f;

        const float textY =
            windowY +
            (windowHeight - textHeight) * 0.5f;

        ui->DL_AddText(
            drawList,
            textX,
            textY,
            textColor,
            placeholderText);
    }

    void OnToggleKeyPressed(
        EModKey key,
        EModKeyEvent event)
    {
        (void)key;
        (void)event;

        if (g_visible)
        {
            MiniMapUI::Hide();
        }
        else
        {
            MiniMapUI::Show();
        }
    }

    bool RegisterToggleKeybind()
    {
        if (g_keybindRegistered)
        {
            return true;
        }

        if (g_self == nullptr ||
            g_self->hooks == nullptr ||
            g_self->hooks->Input == nullptr)
        {
            LOG_ERROR(
                "MiniMap: toggle key registration failed: "
                "input interface is unavailable");

            return false;
        }

        g_self->hooks->Input->RegisterKeybindByName(
            kToggleKey,
            EModKeyEvent::Pressed,
            &OnToggleKeyPressed);

        g_keybindRegistered = true;

        LOG_INFO(
            "MiniMap: registered toggle key: %s",
            kToggleKey);

        return true;
    }

    void UnregisterToggleKeybind()
    {
        if (!g_keybindRegistered)
        {
            return;
        }

        if (g_self != nullptr &&
            g_self->hooks != nullptr &&
            g_self->hooks->Input != nullptr)
        {
            g_self->hooks->Input->UnregisterKeybindByName(
                kToggleKey,
                EModKeyEvent::Pressed,
                &OnToggleKeyPressed);
        }

        g_keybindRegistered = false;
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

        g_visible = false;

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

        if (!RegisterToggleKeybind())
        {
            self->hooks->UI->UnregisterWidget(
                g_widget);

            g_widget = nullptr;

            return false;
        }

        LOG_INFO(
            "MiniMap: UI initialized");

        return true;
    }

    void Shutdown()
    {
        UnregisterToggleKeybind();

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
        g_visible = false;
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

        if (g_visible)
        {
            return;
        }

        g_visible = true;

        g_self->hooks->UI->SetWidgetVisible(
            g_widget,
            true);

        LOG_INFO(
            "MiniMap: widget shown");
    }

    void Hide()
    {
        if (!g_visible)
        {
            return;
        }

        g_visible = false;

        if (g_self != nullptr &&
            g_self->hooks != nullptr &&
            g_self->hooks->UI != nullptr &&
            g_widget != nullptr)
        {
            g_self->hooks->UI->SetWidgetVisible(
                g_widget,
                false);
        }

        LOG_INFO(
            "MiniMap: widget hidden");
    }

    bool IsVisible()
    {
        return g_visible;
    }
}