#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include "Map/Sprites/SpriteQueues.h"

namespace MiniMapBuildingSpriteRenderer
{
    void Initialize(
        MiniMapSpriteQueues::SpriteFifo& fifo);

    void InitializeSession();

    void Tick(float deltaTime);

    void EndSession();

    void Shutdown();
}

#endif
