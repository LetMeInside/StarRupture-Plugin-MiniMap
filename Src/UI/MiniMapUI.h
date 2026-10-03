#pragma once

struct IPluginSelf;

namespace MiniMapUI
{
    bool Initialize(IPluginSelf* self);

    void Shutdown();

    void Show();

    void Hide();

    bool IsVisible();
}