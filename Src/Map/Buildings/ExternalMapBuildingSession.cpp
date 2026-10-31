#if defined(MODLOADER_CLIENT_BUILD)
#include "ExternalMapBuildingSession.h"
#include "Map/Map.h"
#include "Native/NativeApi.h"
#include <cmath>
#include <utility>

namespace MiniMapBuildingCollector
{
    namespace
    {
        bool ValidWorldRectangle(const Bounds& rectangle)
        {
            if(!rectangle.Valid || !std::isfinite(rectangle.MinX) || !std::isfinite(rectangle.MinY) ||
                !std::isfinite(rectangle.MaxX) || !std::isfinite(rectangle.MaxY) ||
                rectangle.MinX>=rectangle.MaxX || rectangle.MinY>=rectangle.MaxY)return false;
            const double width=rectangle.MaxX-rectangle.MinX;
            const double height=rectangle.MaxY-rectangle.MinY;
            return std::isfinite(width) && std::isfinite(height) && std::isfinite(std::hypot(width,height));
        }
    }

    ExternalMapBuildingSession::~ExternalMapBuildingSession(){Shutdown();}

    bool ExternalMapBuildingSession::IsCurrentAdmission(SDK::UWorld*& world,uint64_t& worldGeneration) const
    {
        world=nullptr;worldGeneration=0;
        if(!MiniMapMap::GameplayReady())return false;
        const auto* native=MiniMapNative::Get();
        world=MiniMapMap::GetWorld();
        if(!world || !native || !native->player.isGameThread())return false;
        worldGeneration=MiniMapMap::GetWorldGeneration();
        return true;
    }

    bool ExternalMapBuildingSession::OpenSession(const Bounds& worldRectangle)
    {
        if(active_ || collector_ || !ValidWorldRectangle(worldRectangle))return false;
        SDK::UWorld* world=nullptr;uint64_t worldGeneration=0;
        if(!IsCurrentAdmission(world,worldGeneration))return false;

        std::unique_ptr<BuildingCollector> candidate;
        try
        {
            candidate=std::make_unique<BuildingCollector>();
            if(!candidate->InitializeResources() || !candidate->BeginExplicitSession())
            {
                candidate->ShutdownResources();
                return false;
            }
            candidate->SetRegion(worldRectangle);
        }
        catch(...)
        {
            if(candidate)candidate->ShutdownResources();
            return false;
        }

        SDK::UWorld* currentWorld=nullptr;uint64_t currentGeneration=0;
        if(!IsCurrentAdmission(currentWorld,currentGeneration) || currentWorld!=world || currentGeneration!=worldGeneration)
        {
            candidate->ShutdownResources();
            return false;
        }
        collector_=std::move(candidate);
        admittedWorld_=world;
        admittedWorldGeneration_=worldGeneration;
        active_=true;
        worldInvalidated_=false;
        ++sessionGeneration_;
        return true;
    }

    bool ExternalMapBuildingSession::UpdateWorldRectangle(const Bounds& worldRectangle)
    {
        if(!active_ || !collector_ || !ValidWorldRectangle(worldRectangle))return false;
        const auto* native=MiniMapNative::Get();
        if(!native || !native->player.isGameThread())return false;
        SDK::UWorld* world=nullptr;uint64_t worldGeneration=0;
        if(!IsCurrentAdmission(world,worldGeneration) || world!=admittedWorld_ || worldGeneration!=admittedWorldGeneration_)
        {
            InvalidateOnWorldEnd();
            return false;
        }
        collector_->SetRegion(worldRectangle);
        return true;
    }

    void ExternalMapBuildingSession::Service(float delta)
    {
        if(!active_ || !collector_)return;
        const auto* native=MiniMapNative::Get();
        if(!native || !native->player.isGameThread())return;
        SDK::UWorld* world=nullptr;uint64_t worldGeneration=0;
        if(!IsCurrentAdmission(world,worldGeneration) || world!=admittedWorld_ || worldGeneration!=admittedWorldGeneration_)
        {
            InvalidateOnWorldEnd();
            return;
        }
        collector_->OnTick(delta);
    }

    std::shared_ptr<const Snapshot> ExternalMapBuildingSession::GetSnapshot()
    {
        if(!active_ || !collector_)return {};
        const auto* native=MiniMapNative::Get();
        if(!native || !native->player.isGameThread())return {};
        SDK::UWorld* world=nullptr;uint64_t worldGeneration=0;
        if(!IsCurrentAdmission(world,worldGeneration) || world!=admittedWorld_ || worldGeneration!=admittedWorldGeneration_)
        {
            InvalidateOnWorldEnd();
            return {};
        }
        return collector_->GetSnapshot();
    }

    ExternalMapSessionDiagnostics ExternalMapBuildingSession::GetDiagnostics() const
    {
        ExternalMapSessionDiagnostics result{};
        result.SessionGeneration=sessionGeneration_;
        result.Active=active_ && collector_!=nullptr;
        SDK::UWorld* world=nullptr;uint64_t worldGeneration=0;
        result.Admitted=result.Active && IsCurrentAdmission(world,worldGeneration) &&
            world==admittedWorld_ && worldGeneration==admittedWorldGeneration_;
        result.WorldInvalidated=worldInvalidated_;
        if(result.Admitted && collector_)result.Collector=collector_->GetDiagnostics();
        return result;
    }

    void ExternalMapBuildingSession::CloseSession()
    {
        if(collector_)
        {
            collector_->ShutdownResources();
            collector_.reset();
        }
        admittedWorld_=nullptr;
        admittedWorldGeneration_=0;
        if(active_)++sessionGeneration_;
        active_=false;
    }

    void ExternalMapBuildingSession::InvalidateOnWorldEnd(){worldInvalidated_=active_;CloseSession();}
    void ExternalMapBuildingSession::Shutdown(){CloseSession();}
}
#endif
