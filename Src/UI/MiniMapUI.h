#pragma once

struct IPluginSelf;

namespace MiniMapUI
{
    bool Initialize(IPluginSelf* self);

    void Shutdown();

    void Show();

    void Hide();

    void SetGameplaySuppressed(
        bool suppressed);

    bool IsVisible();
    void RefreshVisibility(); // Applies startup readiness without changing user preference.
}
