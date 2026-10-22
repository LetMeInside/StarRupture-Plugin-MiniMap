#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "VisualAccessCore.h"
namespace SDK { class UWorld; }
namespace MiniMapNativeVisualAccess
{
    // Game-thread world initialization only: bounded metadata acquisition/page
    // validation once per generation. Failure is latched for that generation.
    void InitializeWorld(const SDK::UWorld*,const void* manager,uint64_t generation);
    void ResetWorld();
    // No page probing, metadata acquisition or per-object native retention.
    // Callback checks existing collector ownership, never acquires its mutex.
    using CurrentFn=bool (*)(uint64_t,const SDK::UWorld*,const void*);
    MiniMapVisualAccess::Observation Observe(const MiniMapVisualAccess::Token&,CurrentFn);
}
#endif
