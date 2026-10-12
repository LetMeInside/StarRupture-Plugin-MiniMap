#pragma once
#include "plugin_interface.h"
#include <array>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <limits>
#include <optional>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#if defined(MODLOADER_CLIENT_BUILD)
#include "Native/NativeApi.h"
#endif
IPluginSelf* GetSelf();
namespace MiniMapCallbackTiming
{
    using Clock=std::chrono::steady_clock;
    enum class Id { PluginLoadHooks,PluginInfo,PluginInit,PluginShutdown,WorldBegin,ExperienceComplete,WorldEnd,FogTick,FoundablesTick,AlienTick,InventoryTick,CollectorTick,VisualsTick,PoiTick,TerrainTick,TerrainDiagnosticTick,TerrainDiagnosticKey,WidgetRender,ToggleKey,MouseTick,MouseShutdown,CaptureTick,CaptureShutdown,CaptureWorldEnd,RepresentationStop,RepresentationWorldEnd,DiscoveryTick,DiscoveryWorldEnd,WorldBeforeEnd,ResidentMetadata,CollectorMetadata,CollectorEnsure,CollectorReconcile,CollectorRefresh,CollectorPublish,CollectorDiagnostics,InventoryEnsure,InventoryPoll,DiscoveryRegistry,DiscoveryClasses,DiscoveryCandidate,DiscoveryDiagnostics,CaptureSetup,CaptureSubmit,CaptureReadback,CaptureExport,CaptureRetire,ActorIdentity,ResidentAuthoredClass, Count };
    inline constexpr const char* Names[]={"OnPluginLoadHooks [plugin.cpp]","GetPluginInfo [plugin.cpp]","PluginInit [plugin.cpp]","PluginShutdown [plugin.cpp]","OnWorldBeginPlay [plugin.cpp]","OnExperienceLoadComplete [plugin.cpp]","OnAfterWorldEndPlay [plugin.cpp]","OnTick [Map/FogOfWar.cpp]","OnTick [Map/Foundables.cpp]","OnTick [Map/AlienActors.cpp]","OnTick [Map/BuildingInventory.cpp]","OnTick [Map/BuildingCollector.cpp]","OnTick [Map/BuildingVisuals.cpp]","OnTick [Map/PointsOfInterest.cpp]","OnTerrainTick [Map/Terrain.cpp]","OnTick [Map/TerrainDiagnostics.cpp]","OnDiagnosticKeyPressed [Map/TerrainDiagnostics.cpp]","Render [UI/MiniMapUI.cpp]","OnToggleKeyPressed [UI/MiniMapUI.cpp]","OnTick [Input/MouseWheel.cpp]","OnEngineShutdown [Input/MouseWheel.cpp]","Tick [Map/BuildingSpriteCapture.cpp]","BeginShutdown [Map/BuildingSpriteCapture.cpp]","BeforeEndPlay [Map/BuildingSpriteCapture.cpp]","Stop [Map/BuildingRepresentation.cpp]","BeforeEndPlay [Map/BuildingRepresentation.cpp]","Tick [Map/SpriteDiscovery.cpp]","BeforeEndPlay [Map/SpriteDiscovery.cpp]","OnBeforeWorldEndPlay [plugin.cpp]","resident metadata lookup","collector metadata setup","collector ensure/query construction","collector identity reconciliation","collector refresh","collector indexed query/publication","collector diagnostics","inventory ensure/query construction","inventory oracle poll","discovery registry/snapshot","discovery native class preparation","discovery candidate processing","discovery diagnostic construction","capture component setup","public CaptureScene dispatch","capture readback service","capture image export/publication","capture retirement","authored ActorClass identity","resident authored class lookup"};

    static_assert(std::size(Names)==size_t(Id::Count));
    struct Stats
    {
        unsigned long long Calls{},FullCalls{},Over2{},Over8{},Over16{},Over33{},GameCalls{};
        unsigned long long Identity{},Readable{},Queries{};
        DWORD LastThread{},MaxThread{};
        double Last{},Min=(std::numeric_limits<double>::max)(),Max{},Total{},FullMax{},FullTotal{},ReportingMax{};
        void Add(double ms) noexcept {++Calls;Last=ms;Min=(std::min)(Min,ms);Max=(std::max)(Max,ms);Total+=ms;}
        void Finish(double ms,double report,DWORD thread,DWORD gameThread) noexcept
        {
            ++FullCalls;FullTotal+=ms;ReportingMax=(std::max)(ReportingMax,report);LastThread=thread;
            if(ms>FullMax){FullMax=ms;MaxThread=thread;}
            Over2+=ms>2;Over8+=ms>8;Over16+=ms>16;Over33+=ms>33;GameCalls+=gameThread&&thread==gameThread;
        }
    };
    inline std::array<Stats,size_t(Id::Count)> Samples{};
    inline std::mutex Access;
    inline Clock::time_point LastReport{},StartupStart{};
    inline DWORD GameThread{};
    struct Event {Id Kind{};DWORD Thread{};double Begin{},End{},Reporting{};};
    inline std::array<Event,128> Events{};
    inline size_t EventCount{},ReportedEvents{},StartupCalls{};
    inline void StartWorldTrace() noexcept
    {
        std::scoped_lock lock(Access);StartupStart=Clock::now();GameThread=GetCurrentThreadId();
        EventCount=ReportedEvents=StartupCalls=0;
    }
    struct Deferred {Id Kind{};double Ms{};};
    inline std::array<Deferred,32> PreLogger{};
    inline size_t PreLoggerCount{};
    inline void Report(IPluginSelf* self,bool=false) noexcept
    {
        if(!self||!self->logger)return;
        try {
            const auto now=Clock::now();std::array<Stats,size_t(Id::Count)> copy;double interval;
            std::array<Deferred,32> pending{};size_t count;
            std::array<Event,8> events{};size_t eventCount=0;
            {std::scoped_lock lock(Access);
             interval=std::chrono::duration<double>(now-LastReport).count();
             if(LastReport==Clock::time_point{}){LastReport=now;return;}
             // A lifecycle event must not force another complete synchronous dump.
             if(interval<10)return;
             count=PreLoggerCount;pending=PreLogger;PreLoggerCount=0;
             copy=Samples;LastReport=now;
             while(ReportedEvents<EventCount&&eventCount<events.size())events[eventCount++]=Events[ReportedEvents++];}
            for(size_t i=0;i<count;++i)self->logger->Info(self,"MiniMap: %s took %.3f ms to finish (entry preceded logger availability)",Names[size_t(pending[i].Kind)],pending[i].Ms);
            for(size_t i=0;i<copy.size();++i)if(copy[i].Calls){const auto& s=copy[i];
                self->logger->Info(self,"MiniMap: callback profile %s: calls=%llu bodyMax=%.3f bodyTotal=%.3f fullCalls=%llu fullMax=%.3f fullTotal=%.3f reportingMax=%.3f ms over(2/8/16/33)=%llu/%llu/%llu/%llu thread(last/max)=%lu/%lu gameThreadCalls=%llu identity=%llu readable=%llu virtualQueries=%llu interval=%.3fs; cumulative inclusive, nested totals not additive",
                    Names[i],s.Calls,s.Max,s.Total,s.FullCalls,s.FullMax,s.FullTotal,s.ReportingMax,
                    s.Over2,s.Over8,s.Over16,s.Over33,s.LastThread,s.MaxThread,s.GameCalls,s.Identity,s.Readable,s.Queries,interval);}
            for(size_t i=0;i<eventCount;++i){const auto& e=events[i];
                self->logger->Info(self,"MiniMap: startup trace %s: thread=%lu entry=%.3f exit=%.3f reporting=%.3f ms relativeToWorldBegin; bounded first/slow events, gaps alone do not prove blocking",
                    Names[size_t(e.Kind)],e.Thread,e.Begin,e.End,e.Reporting);}
        }catch(...){}
    }
    // Declare FIRST. Full duration includes body, local destruction and our
    // synchronous reporting, sampled just before final counter bookkeeping.
    class Scope
    {
        Clock::time_point Start=Clock::now();Id Kind;bool Frequent,Nested;IPluginSelf* Self;
        DWORD Thread=GetCurrentThreadId();
#if defined(MODLOADER_CLIENT_BUILD)
        MiniMapNative::ValidationProfile NativeProfile;
        std::optional<MiniMapNative::ScopedValidationProfile> NativeScope;
#endif
    public:
        Scope(Id kind,bool frequent,IPluginSelf* self=nullptr,bool nested=false) noexcept:Kind(kind),Frequent(frequent),Nested(nested),Self(self?self:GetSelf())
        {
#if defined(MODLOADER_CLIENT_BUILD)
            NativeProfile.MeasureTime=false;NativeScope.emplace(NativeProfile);
#endif
        }
        Scope(const Scope&)=delete;Scope& operator=(const Scope&)=delete;
        ~Scope() noexcept
        {
            const auto bodyEnd=Clock::now();const double ms=std::chrono::duration<double,std::milli>(bodyEnd-Start).count();
#if defined(MODLOADER_CLIENT_BUILD)
            NativeScope.reset(); // Merge nested counters before sampling parent.
#endif
            try {
                {std::scoped_lock lock(Access);auto& s=Samples[size_t(Kind)];s.Add(ms);
#if defined(MODLOADER_CLIENT_BUILD)
                 s.Identity+=NativeProfile.IdentityCalls;s.Readable+=NativeProfile.ReadableCalls;s.Queries+=NativeProfile.VirtualQueries;
#endif
                 if(!Frequent&&!Self&&PreLoggerCount<PreLogger.size())PreLogger[PreLoggerCount++]={Kind,ms};}
                if(!Nested){
                    if(!Frequent&&Self&&Self->logger)Self->logger->Info(Self,"MiniMap: %s took %.3f ms to finish (body; reporting measured separately)",Names[size_t(Kind)],ms);
                    Report(Self);
                }
                const auto end=Clock::now();const double full=std::chrono::duration<double,std::milli>(end-Start).count();
                const double reporting=std::chrono::duration<double,std::milli>(end-bodyEnd).count();
                std::scoped_lock lock(Access);Samples[size_t(Kind)].Finish(full,reporting,Thread,GameThread);
                if(StartupStart!=Clock::time_point{}&&end-StartupStart<std::chrono::seconds(120)&&Start>=StartupStart)
                {
                    const bool first=StartupCalls++<32;
                    if(EventCount<Events.size()&&(first||full>2||!Frequent))
                        Events[EventCount++]={Kind,Thread,std::chrono::duration<double,std::milli>(Start-StartupStart).count(),
                            std::chrono::duration<double,std::milli>(end-StartupStart).count(),reporting};
                }
            }catch(...){}
        }
    };
    struct Phase final:Scope {explicit Phase(Id kind) noexcept:Scope(kind,true,nullptr,true){}};
}
