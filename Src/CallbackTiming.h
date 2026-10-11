#pragma once
#include "plugin_interface.h"
#include <array>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <limits>
IPluginSelf* GetSelf();
namespace MiniMapCallbackTiming
{
    using Clock=std::chrono::steady_clock;
    enum class Id { PluginLoadHooks,PluginInfo,PluginInit,PluginShutdown,WorldBegin,ExperienceComplete,WorldEnd,FogTick,FoundablesTick,AlienTick,InventoryTick,CollectorTick,VisualsTick,PoiTick,TerrainTick,TerrainDiagnosticTick,TerrainDiagnosticKey,WidgetRender,ToggleKey,MouseTick,MouseShutdown,CaptureTick,CaptureShutdown,CaptureWorldEnd,RepresentationStop,RepresentationWorldEnd,DiscoveryTick,DiscoveryWorldEnd,ReadinessTick, Count };
    inline constexpr const char* Names[]={"OnPluginLoadHooks [plugin.cpp]","GetPluginInfo [plugin.cpp]","PluginInit [plugin.cpp]","PluginShutdown [plugin.cpp]","OnWorldBeginPlay [plugin.cpp]","OnExperienceLoadComplete [plugin.cpp]","OnAfterWorldEndPlay [plugin.cpp]","OnTick [Map/FogOfWar.cpp]","OnTick [Map/Foundables.cpp]","OnTick [Map/AlienActors.cpp]","OnTick [Map/BuildingInventory.cpp]","OnTick [Map/BuildingCollector.cpp]","OnTick [Map/BuildingVisuals.cpp]","OnTick [Map/PointsOfInterest.cpp]","OnTerrainTick [Map/Terrain.cpp]","OnTick [Map/TerrainDiagnostics.cpp]","OnDiagnosticKeyPressed [Map/TerrainDiagnostics.cpp]","Render [UI/MiniMapUI.cpp]","OnToggleKeyPressed [UI/MiniMapUI.cpp]","OnTick [Input/MouseWheel.cpp]","OnEngineShutdown [Input/MouseWheel.cpp]","Tick [Experiments/SmelterCaptureExperiment.cpp]","BeginShutdown [Experiments/SmelterCaptureExperiment.cpp]","BeforeEndPlay [Experiments/SmelterCaptureExperiment.cpp]","Stop [Map/BuildingRepresentation.cpp]","BeforeEndPlay [Map/BuildingRepresentation.cpp]","Tick [Map/SpriteDiscovery.cpp]","BeforeEndPlay [Map/SpriteDiscovery.cpp]","GameplayReadinessTick"};
    struct Stats
    {
        unsigned long long Calls{};double Last{},Min=(std::numeric_limits<double>::max)(),Max{},Total{};
        void Add(double ms) noexcept {++Calls;Last=ms;Min=(std::min)(Min,ms);Max=(std::max)(Max,ms);Total+=ms;}
    };
    inline std::array<Stats,size_t(Id::Count)> Samples{};
    inline std::mutex Access;
    inline Clock::time_point LastReport{};
    struct Deferred {Id Kind{};double Ms{};};
    inline std::array<Deferred,32> PreLogger{};
    inline size_t PreLoggerCount{};
    inline void Report(IPluginSelf* self,bool force=false) noexcept
    {
        if(!self||!self->logger)return;
        try {
            const auto now=Clock::now();std::array<Stats,size_t(Id::Count)> copy;double interval;
            std::array<Deferred,32> pending{};size_t count;
            {std::scoped_lock lock(Access);count=PreLoggerCount;pending=PreLogger;PreLoggerCount=0;}
            for(size_t i=0;i<count;++i)self->logger->Info(self,"MiniMap: %s took %.3f ms to finish (entry preceded logger availability)",Names[size_t(pending[i].Kind)],pending[i].Ms);
            {std::scoped_lock lock(Access);interval=std::chrono::duration<double>(now-LastReport).count();
             if(LastReport==Clock::time_point{}){LastReport=now;return;}
             if(!force&&interval<10)return;copy=Samples;LastReport=now;}
            for(size_t i=0;i<copy.size();++i)if(copy[i].Calls){const auto& s=copy[i];
                self->logger->Info(self,"MiniMap: callback %s: calls=%llu last=%.3f min=%.3f max=%.3f mean=%.3f total=%.3f ms reporting_interval=%.3f s; cumulative since plugin load",
                    Names[i],s.Calls,s.Last,s.Min,s.Max,s.Total/s.Calls,s.Total,interval);}
        }catch(...){}
    }
    // Declare FIRST: destruction runs after the body and all later local objects.
    // Logging/aggregation starts only after the end timestamp has been sampled.
    class Scope final
    {
        Clock::time_point Start=Clock::now();Id Kind;bool Frequent;IPluginSelf* Self;
    public:
        Scope(Id kind,bool frequent,IPluginSelf* self=nullptr) noexcept:Kind(kind),Frequent(frequent),Self(self?self:GetSelf()){}
        Scope(const Scope&)=delete;Scope& operator=(const Scope&)=delete;
        ~Scope() noexcept
        {
            const auto end=Clock::now();const double ms=std::chrono::duration<double,std::milli>(end-Start).count();
            try {{std::scoped_lock lock(Access);Samples[size_t(Kind)].Add(ms);
                 if(!Frequent&&!Self&&PreLoggerCount<PreLogger.size())PreLogger[PreLoggerCount++]={Kind,ms};}
                if(!Frequent&&Self&&Self->logger)Self->logger->Info(Self,"MiniMap: %s took %.3f ms to finish",Names[size_t(Kind)],ms);
                Report(Self,!Frequent);
            }catch(...){}
        }
    };
}

