#if defined(MODLOADER_CLIENT_BUILD)

#include "Map/Buildings/BuildingScheduler.h"

#include "Map/Buildings/BuildingCache.h"

#include <array>
#include <string>
#include <string_view>

namespace MiniMapBuildingSchedulerInternal
{
    std::mutex LifecycleMutex;
}

namespace
{
    constexpr std::array<std::string_view, 0> BlacklistedBlueprints{};

    MiniMapSpriteStatus::Dictionary* g_statuses = nullptr;
    MiniMapSpriteQueues::SpriteFifo* g_fifo = nullptr;
    MiniMapSpriteQueues::SpriteFeedbackQueue g_feedback;
    bool g_sessionActive = false;
}

namespace MiniMapBuildingScheduler
{
    void Initialize(
        MiniMapSpriteStatus::Dictionary& statuses,
        MiniMapSpriteQueues::SpriteFifo& fifo)
    {
        g_sessionActive = false;
        g_statuses = &statuses;
        g_fifo = &fifo;
    }

    MiniMapSpriteQueues::SpriteFeedbackQueue& FeedbackQueue() noexcept
    {
        return g_feedback;
    }

    void InitializeSession()
    {
        if (!g_statuses || !g_fifo || g_sessionActive) return;

        std::string queuedBlueprint;
        while (g_fifo->Remove(queuedBlueprint))
        {
        }

        MiniMapSpriteQueues::Feedback feedback;
        while (g_feedback.Remove(feedback))
        {
        }

        g_statuses->clear();
        for (const std::string_view blueprint : BlacklistedBlueprints)
        {
            g_statuses->try_emplace(std::string(blueprint), MiniMapSpriteStatus::Location::Failed);
        }
        g_sessionActive = true;
    }

    void StartDraw()
    {
        if (!g_sessionActive || !g_statuses || !g_fifo) return;

        MiniMapSpriteQueues::Feedback feedback;
        while (g_feedback.Remove(feedback))
        {
            const auto it = g_statuses->find(feedback.BlueprintPath);
            if (it == g_statuses->end() || it->second.Where != MiniMapSpriteStatus::Location::Queued)
            {
                continue;
            }

            it->second.Where = feedback.Success
                ? MiniMapSpriteStatus::Location::FileCache
                : MiniMapSpriteStatus::Location::Failed;
        }
    }

    void DrawBuilding(const MiniMapBuildingCollector::Record& building)
    {
        if (!g_sessionActive || !g_statuses || !g_fifo || !building.CanonicalBlueprint || building.CanonicalBlueprint->empty())
        {
            return;
        }

        const std::string& blueprintPath = *building.CanonicalBlueprint;
        const auto it = g_statuses->find(blueprintPath);
        if (it != g_statuses->end())
        {
            return;
        }

        if (MiniMapBuildingCache::Exists(blueprintPath))
        {
            g_statuses->try_emplace(blueprintPath, MiniMapSpriteStatus::Location::FileCache);
            return;
        }

        g_fifo->Add(blueprintPath);
        g_statuses->try_emplace(blueprintPath, MiniMapSpriteStatus::Location::Queued);
    }

    void EndSession()
    {
        g_sessionActive = false;

        if (g_fifo)
        {
            std::string queuedBlueprint;
            while (g_fifo->Remove(queuedBlueprint))
            {
            }
        }

        MiniMapSpriteQueues::Feedback feedback;
        while (g_feedback.Remove(feedback))
        {
        }

        if (g_statuses)
        {
            g_statuses->clear();
        }
    }

    void Shutdown()
    {
        EndSession();
        g_statuses = nullptr;
        g_fifo = nullptr;
    }
}

#endif
