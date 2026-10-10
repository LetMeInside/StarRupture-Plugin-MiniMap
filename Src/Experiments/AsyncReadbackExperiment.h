#pragma once

#if defined(MINIMAP_ASYNC_READBACK_EXPERIMENT) && MINIMAP_ASYNC_READBACK_EXPERIMENT && defined(MODLOADER_CLIENT_BUILD)
namespace MiniMapAsyncReadbackExperiment
{
    void Initialize();
    void CancelForWorldTransition();
    void Shutdown();
}
#endif
