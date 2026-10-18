#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include "Map/Sprites/SpriteQueues.h"

namespace MiniMapBuildingSpriteRenderer
{
    // Borrowed queues must outlive the renderer. Future native service and
    // lifecycle calls must be serialized on the existing game-thread owner.
    void Initialize(
        MiniMapSpriteQueues::SpriteFifo& fifo,
        MiniMapSpriteQueues::SpriteFeedbackQueue& feedback);

    void InitializeSession();

    void Tick(float deltaTime);

    void EndSession();

    void Shutdown();
}

#endif
