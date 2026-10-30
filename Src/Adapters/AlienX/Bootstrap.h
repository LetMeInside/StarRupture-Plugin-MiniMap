#pragma once

#include "Compatibility.h"

extern "C"
{
    __declspec(dllexport) PluginInfo* GetPluginInfo();
    __declspec(dllexport) bool PluginInit(IPluginSelf* self);
    __declspec(dllexport) void PluginShutdown();
    __declspec(dllexport) void OnPluginLoadHooks(IPluginSelf* self, IPluginHookScanner* scanner);
}
