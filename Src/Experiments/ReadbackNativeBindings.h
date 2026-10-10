#pragma once
#include <cstdint>

namespace MiniMapReadbackNative
{
    // POD only: safe before OnPluginLoadHooks. No owning UE objects here.
    struct Bindings
    {
        uintptr_t Submit, Immediate, Create, Construct, Destroy, Copy, Lock, Unlock;
        uintptr_t Poll, MarkForDelete, Malloc, Free;
        uintptr_t DynamicRHI, Pipe, Threaded, Multithreaded, GpuCount;
        uintptr_t FromValidEName;
    };
    const Bindings& GetBindings();
    void SetStatus(const char* message);
}
