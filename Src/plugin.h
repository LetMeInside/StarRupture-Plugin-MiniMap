#pragma once

#include <windows.h>
#include "plugin_interface.h"

extern IPluginSelf* g_self;

struct MiniMapPluginInitHookBaseline
{
    bool Captured = false;
    const void* Self = nullptr;
    const void* Hooks = nullptr;
    const void* Engine = nullptr;
    const void* ImGuiTextures = nullptr;
};

MiniMapPluginInitHookBaseline GetPluginInitHookBaseline();

extern "C"
{
    __declspec(dllexport) PluginInfo* GetPluginInfo();
    __declspec(dllexport) bool PluginInit(IPluginSelf* self);
    __declspec(dllexport) void PluginShutdown();
    __declspec(dllexport) void OnPluginLoadHooks(
        IPluginSelf* self,
        IPluginHookScanner* scanner);
}
