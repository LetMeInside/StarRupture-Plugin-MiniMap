#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include "Map/Buildings/SpriteQueues.h"

namespace MiniMapBuildingSpriteRenderer
{
    // Borrowed queues must outlive the renderer. Integration callers hold
    // LifecycleMutex for every service/lifecycle call; native work is game-thread only.
    void Initialize(
        MiniMapSpriteQueues::SpriteFifo& fifo,
        MiniMapSpriteQueues::SpriteFeedbackQueue& feedback);

    // False leaves admission closed when prior native ownership remains blocked.
    bool InitializeSession();

    void Tick(float deltaTime);

    // Diagnostic CPU metadata only. Caller is the existing game-thread service;
    // no UObject dereference, admission change, lease, or native call is made.
    struct DiagnosticSnapshot
    {
        uintptr_t PrivateWorld = 0;
        uint64_t Session = 0;
        unsigned WorldPhase = 0, RequestPhase = 0;
        bool Active = false;
        char BlueprintTail[96]{};
    };
    DiagnosticSnapshot GetDiagnosticSnapshot() noexcept;

    void EndSession();

    void Shutdown();
}

#endif
