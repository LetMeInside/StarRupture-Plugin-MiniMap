#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include "Map/Buildings/BuildingCollector.h"
#include "Map/Sprites/SpriteStatus.h"
#include "Map/Sprites/SpriteQueues.h"

namespace MiniMapBuildingScheduler
{
    void Initialize(
        MiniMapSpriteStatus::Dictionary& statuses,
        MiniMapSpriteQueues::SpriteFifo& fifo);

    void InitializeSession();

    void StartTick();

    void DrawBuilding(
        const MiniMapBuildingCollector::Record& building);

    void EndSession();

    void Shutdown();
}

#endif
