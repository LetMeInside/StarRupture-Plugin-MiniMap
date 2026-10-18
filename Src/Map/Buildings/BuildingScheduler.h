#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include "Map/Buildings/BuildingCollector.h"
#include "Map/Sprites/SpriteStatus.h"
#include "Map/Sprites/SpriteQueues.h"
#include <mutex>

// Integration callers serialize drawing and lifecycle transitions with this lock.
namespace MiniMapBuildingSchedulerInternal
{
    extern std::mutex LifecycleMutex;
}

namespace MiniMapBuildingScheduler
{
    void Initialize(
        MiniMapSpriteStatus::Dictionary& statuses,
        MiniMapSpriteQueues::SpriteFifo& fifo);

    // Scheduler-owned queue; renderer borrows it for terminal request feedback.
    MiniMapSpriteQueues::SpriteFeedbackQueue& FeedbackQueue() noexcept;

    void InitializeSession();

    void StartDraw();

    void DrawBuilding(
        const MiniMapBuildingCollector::Record& building);

    void EndSession();

    void Shutdown();
}

#endif
