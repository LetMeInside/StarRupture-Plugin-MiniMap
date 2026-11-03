#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingComplexWorld.h"
#include "BuildingCollector.h"
#include "Map/Map.h"
#include <cmath>
#include <memory>

namespace
{
    std::unique_ptr<MiniMapBuildingCollector::BuildingCollector> Collector;
    uint64_t CollectorSession = 0;
    SDK::UWorld* CollectorWorld = nullptr;

    void ReleaseCollector() noexcept
    {
        if (Collector) Collector->ShutdownResources();
        Collector.reset();
        CollectorSession = 0;
        CollectorWorld = nullptr;
    }
}

namespace MiniMapBuildingComplexWorld
{
    void Shutdown() noexcept
    {
        ReleaseCollector();
    }

    bool OpenCollectorSession(uint64_t viewportSession, SDK::UWorld* world,
        double originX, double originY) noexcept
    {
        if (!viewportSession || !world || !std::isfinite(originX) || !std::isfinite(originY) ||
            Collector || CollectorSession || CollectorWorld || MiniMapMap::GetWorld() != world) return false;
        std::unique_ptr<MiniMapBuildingCollector::BuildingCollector> candidate;
        try
        {
            candidate = std::make_unique<MiniMapBuildingCollector::BuildingCollector>();
            if (!candidate->InitializeResources() || !candidate->BeginExplicitSession())
            {
                candidate->ShutdownResources();
                return false;
            }
        }
        catch (...)
        {
            if (candidate) candidate->ShutdownResources();
            return false;
        }
        if (MiniMapMap::GetWorld() != world)
        {
            candidate->ShutdownResources();
            return false;
        }
        Collector = std::move(candidate);
        CollectorSession = viewportSession;
        CollectorWorld = world;
        return true;
    }

    bool OwnsCollectorSession(uint64_t viewportSession, SDK::UWorld* world) noexcept
    {
        return viewportSession && world && Collector && CollectorSession == viewportSession &&
            CollectorWorld == world && MiniMapMap::GetWorld() == world;
    }

    void CloseCollectorSession(uint64_t viewportSession) noexcept
    {
        if (Collector && CollectorSession == viewportSession)
        {
            ReleaseCollector();
        }
    }

    bool ValidateCellImage(const Cell::CellImage&) noexcept { return false; }
    bool ValidateCellImage(const Cell::CellImageIdentity&) noexcept { return false; }
    bool ValidateTile(const TileIdentity&) noexcept { return false; }
    bool ValidateGroupedTileImage(const GroupedTileImage&) noexcept { return false; }
}
#endif







