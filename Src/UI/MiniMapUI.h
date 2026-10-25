#pragma once

#include <cstdint>

struct IPluginSelf;

namespace MiniMapUI
{
    bool Initialize(IPluginSelf* self);

    void Shutdown();

    void Show();

    void Hide();

    bool IsVisible();
    void RefreshVisibility(); // Syncs the widget property from the authoritative visibility state.
    uint32_t SetBlockingMenuActive(uint32_t menuBit, bool active);
    uint32_t ClearBlockingMenuSuppression();
}
