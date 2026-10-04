#pragma once

#include <windows.h>
#include "plugin_interface.h"

#include <cstdint>

extern IPluginSelf* g_self;

#ifdef MODLOADER_CLIENT_BUILD
uintptr_t GetSoftObjectLoadSynchronousAddress();
uintptr_t GetBrushResourceAsTexture2DAddress();

uintptr_t GetSetForceMipLevelsToBeResidentAddress();
uintptr_t GetWaitForStreamingAddress();
uintptr_t GetNumResidentMipsAddress();
uintptr_t GetNumMipsAllowedAddress();
uintptr_t GetNumMipsAddress();

uintptr_t GetStreamInAddress();
uintptr_t GetWaitForPendingInitOrStreamingAddress();
uintptr_t GetFirstPlayerControllerAddress();
uintptr_t GetPlayerPawnAddress();
uintptr_t GetComponentLocationAddress();

uintptr_t GetPlatformDataAddress();
uintptr_t GetBulkDataSizeAddress();
uintptr_t GetCanLoadFromDiskAddress();
uintptr_t GetBulkDataCopyAddress();
uintptr_t GetMemoryFreeAddress();
#endif

extern "C"
{
    __declspec(dllexport) PluginInfo* GetPluginInfo();
    __declspec(dllexport) bool PluginInit(IPluginSelf* self);
    __declspec(dllexport) void PluginShutdown();
    __declspec(dllexport) void OnPluginLoadHooks(
        IPluginSelf* self,
        IPluginHookScanner* scanner);
}