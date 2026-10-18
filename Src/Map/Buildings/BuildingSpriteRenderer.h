#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include "Map/Sprites/SpriteQueues.h"

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

    void EndSession();

    void Shutdown();
}

#endif
