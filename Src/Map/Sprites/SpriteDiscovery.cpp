#include "Map/Map.h"
#include "CallbackTiming.h"
#if defined(MODLOADER_CLIENT_BUILD) && defined(MINIMAP_STAGE_R26A_SPRITE_DISCOVERY)
#include "SpriteDiscoveryCore.h"
#include "SpriteCandidateIdentity.h"
#if defined(MINIMAP_STAGE_R26B_SPRITE_VALIDATION)
#include "SpriteValidationCore.h"
#include "BuildingRepresentation.h"
#endif
#include "plugin.h"
#include "plugin_helpers.h"
#include "SpriteDiscovery.h"
#include "BuildingSpriteCapture.h"
#include "BuildingSprites.h"
#include "Map/Buildings/BuildingCollector.h"
#include "Map/Terrain/TerrainCache.h"
#include <chrono>
#include <fstream>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <array>

namespace MiniMapSpriteDiscovery
{
    namespace C=MiniMapSpriteDiscoveryCore;
    namespace B=MiniMapBuildingCollector;
    namespace
    {
        using Clock=std::chrono::steady_clock;
        // CPU strings and filesystem paths only; no UObject or callback on worker.
        // Four coalescing slots cap pending output at 1 MiB plus one in-flight file.
        class DiagnosticWriter
        {
            std::mutex Mutex;std::condition_variable Wake;
            std::array<std::string,4> Pending;
            std::array<uint64_t,4> Generations{};
            std::atomic<uint64_t> Generation{},Failures{};
            bool Stopping=false;std::thread Worker;
            void Run() noexcept
            {
                for(;;)
                {
                    std::string json;size_t kind=0;uint64_t generation=0;
                    {std::unique_lock lock(Mutex);Wake.wait(lock,[&]{return Stopping||Any();});
                     if(Stopping)return;
                     for(;kind<Pending.size();++kind)if(!Pending[kind].empty())break;
                     json=std::move(Pending[kind]);Pending[kind].clear();generation=Generations[kind];}
                    try {
#if defined(MINIMAP_BUILDING_SPRITES)
                        MiniMapBuildingSprites::TimingScope publicationTiming(MiniMapBuildingSprites::Timing::DiagnosticPublish);
#endif
                        std::filesystem::path directory;
                        if(generation!=Generation.load()||!MiniMapMap::GameplayReady())continue;
                        if(!MiniMapTerrainCache::GetBuildingDiagnosticsDirectory(directory)){++Failures;continue;}
                        std::error_code ec;std::filesystem::create_directories(directory,ec);if(ec){++Failures;continue;}
                        const wchar_t* names[]={L"SpriteDiscovery.json",L"SpriteValidation.json",L"SpriteClassification.json",L"SpritePipeline.json"};
                        const auto path=directory/names[kind];const auto temp=path.wstring()+L".tmp";
                        std::ofstream file(temp,std::ios::binary|std::ios::trunc);
                        file.write(json.data(),std::streamsize(json.size()));file.close();
                        if(file&&generation==Generation.load())
                        {if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING)){++Failures;std::filesystem::remove(temp,ec);}}
                        else {if(!file)++Failures;std::filesystem::remove(temp,ec);}
                    }catch(...){++Failures;} // Diagnostics cannot fail capture or gameplay.
                }
            }
            bool Any()const {for(const auto& s:Pending)if(!s.empty())return true;return false;}
        public:
            void Invalidate(uint64_t generation)
            {std::scoped_lock lock(Mutex);Generation.store(generation);for(auto& s:Pending)s.clear();}
            void Publish(std::string json,size_t kind,uint64_t generation)
            {
                if(json.size()>C::OutputLimit||kind>=Pending.size())return;
                std::scoped_lock lock(Mutex);if(Stopping||Generation.load()!=generation)return;
                if(!Worker.joinable())try{Worker=std::thread([this]{Run();});}catch(...){Stopping=true;++Failures;return;}
                Pending[kind]=std::move(json);Generations[kind]=generation;Wake.notify_one();
            }
            uint64_t ErrorCount()const noexcept{return Failures.load();}
            ~DiagnosticWriter()
            {{std::scoped_lock lock(Mutex);Stopping=true;for(auto& s:Pending)s.clear();}Wake.notify_one();if(Worker.joinable())Worker.join();}
        };
        struct Owner : C::Session<B::Snapshot>
        {
            DiagnosticWriter Writer;
            unsigned Dirty{};uint64_t WriterGeneration{},WriterErrors{};
#if defined(MINIMAP_STAGE_R26B_SPRITE_VALIDATION)
            MiniMapSpriteValidation::Scheduler<B::Snapshot> Validation;
            double NextValidationExport{};
            double NextPipelineExport{};
#endif
        };
        Owner* owner=nullptr; // Explicit runtime ownership, no static constructor.
        void Publish(std::string json,bool validation=false,bool classification=false,bool pipeline=false)
        {
            if(owner)owner->Writer.Publish(std::move(json),pipeline?3:classification?2:validation?1:0,owner->WriterGeneration);
        }
        void BeforeEndPlay(SDK::UWorld* world,const char*)
        {
        MiniMapCallbackTiming::Scope callbackTiming(MiniMapCallbackTiming::Id::DiscoveryWorldEnd,false);
            if(!world||!owner||world!=MiniMapMap::GetWorld())return;
            MiniMapMap::SetGameplayReady(false);
            const auto current=B::GetSnapshot();
            owner->Writer.Invalidate(0);owner->WriterGeneration=0;owner->Dirty=0;
            owner->Invalidate(current?current->Generation:owner->Published.Generation);
#if defined(MINIMAP_BUILDING_SPRITES)
            MiniMapBuildingSprites::Reset(0);
#endif
#if defined(MINIMAP_STAGE_R26B_SPRITE_VALIDATION)
            owner->Validation.Clear();owner->NextValidationExport=0;
#endif
        }
        C::Input CopiedRecord(const B::Record& r)
        {
            return MiniMapSpriteCandidates::Copy(r);
        }
#if defined(MINIMAP_BUILDING_SPRITES)
        MiniMapSpriteValidation::Result ProcessCandidate(const B::Record& record,const B::Record* alternate)
        {
            namespace P=MiniMapBuildingSpriteCore;namespace V=MiniMapSpriteValidation;
            const auto key=CopiedRecord(record).Identity;
            MiniMapCallbackTiming::Phase phase(MiniMapCallbackTiming::Id::DiscoveryCandidate);
            const auto start=Clock::now();
            auto out=MiniMapBuildingRepresentation::ValidateAndCapture(record,alternate);
            const double elapsed=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
            LOG_INFO("MiniMap: sprite candidate audit building=%u took %.3f ms; route=%s reason=%s static=%zu; synchronous audit duration, not a two-ms bounded unit",
                record.BuildingId,elapsed,out.Route.c_str(),out.Reason.c_str(),out.Static);
            if(out.Reason=="blueprint_blacklisted")
                MiniMapBuildingSprites::Record(key,P::State::Excluded,out.Reason);
            else if(out.Classification.CachedPolicy.ExcludeFromMiniMap==MiniMapSpriteClassification::Exclusion::Exclude)
                MiniMapBuildingSprites::Record(key,P::State::Excluded,out.Classification.CachedPolicy.Reason);
            else if(out.Status!=V::Outcome::Eligible)
                // Deferred admission/unknown validation is not an unsupported
                // renderer capability. In particular a provider reevaluation
                // without advancement must retain the provisional quality record.
                MiniMapBuildingSprites::Record(key,out.Status==V::Outcome::Unsupported?
                    P::State::Unsupported:P::State::Unavailable,out.Reason);
            else MiniMapBuildingSprites::Record(key,P::State::Queued,out.Route+"; static_instances="+std::to_string(out.Static)+
                "; skeletal_omitted="+std::to_string(out.Skeletal)+"; "+out.Reason);
            return out;
        }
#endif
        void Tick(float)
        {
        MiniMapCallbackTiming::Scope callbackTiming(MiniMapCallbackTiming::Id::DiscoveryTick,true);
        if(!MiniMapMap::GameplayReady())return;
            if(!owner)return;
#if defined(MINIMAP_BUILDING_SPRITES)
            MiniMapBuildingSprites::TimingScope timing(MiniMapBuildingSprites::Timing::DiscoveryService);
#endif
            const auto errors=owner->Writer.ErrorCount();
            if(errors!=owner->WriterErrors){owner->WriterErrors=errors;LOG_WARN("MiniMap: diagnostic publication failures=%llu; sprite capture/display unaffected",(unsigned long long)errors);}
            const double now=std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
            const auto tickStart=Clock::now();
            C::Publication result;
            {
#if defined(MINIMAP_BUILDING_SPRITES)
                MiniMapBuildingSprites::TimingScope registryTiming(MiniMapBuildingSprites::Timing::RegistryScan);
#endif
                MiniMapCallbackTiming::Phase phase(MiniMapCallbackTiming::Id::DiscoveryRegistry);
                result=owner->Service(B::GetSnapshot(),MiniMapMap::HasWorld(),now,&CopiedRecord);
            }
            if(owner->WriterGeneration!=owner->Published.Generation)
            {owner->WriterGeneration=owner->Published.Generation;owner->Writer.Invalidate(owner->WriterGeneration);owner->Dirty=0;}
            if(result!=C::Publication::None)owner->Dirty|=1;
#if defined(MINIMAP_STAGE_R26B_SPRITE_VALIDATION)
            auto& v=owner->Validation;
            if(result==C::Publication::Invalidated){v.Clear();owner->NextValidationExport=0;}
            if(result==C::Publication::Ready)v.Observe(owner->Published);
#if defined(MINIMAP_BUILDING_SPRITES)
            MiniMapBuildingSprites::Reset(MiniMapMap::HasWorld()?owner->Published.Generation:0);
            if(result==C::Publication::Ready)MiniMapBuildingSprites::ObserveRegistry(owner->Published);
            try{MiniMapBuildingSprites::ServiceRegistry();}
            catch(...){/* A failed view allocation preserves the prior view; retry CPU reconciliation. */}
            if(result==C::Publication::Ready)
                for(size_t i=0;i<owner->Published.Count;++i)
                    MiniMapBuildingSprites::Record(owner->Published.Entries[i].Identity,MiniMapBuildingSpriteCore::State::Awaiting,"pending_bounded_validation");
            const bool anchorsReady=[] {MiniMapCallbackTiming::Phase phase(MiniMapCallbackTiming::Id::DiscoveryClasses);return MiniMapBuildingRepresentation::PrepareNativeClasses();}();
            if(anchorsReady&&std::chrono::duration<double,std::milli>(Clock::now()-tickStart).count()<2.0&&MiniMapSmelterCapture::CanAcceptCandidate())
            {
                C::Key key;
                if(MiniMapBuildingSprites::TakeReadinessReevaluation(key,&MiniMapSmelterCapture::HasResourceWatchCapacity)&&v.Complete())
                    MiniMapBuildingSprites::AcknowledgeReevaluation(key,v.RequestReevaluation(key));
            }
            const bool changed=anchorsReady&&std::chrono::duration<double,std::milli>(Clock::now()-tickStart).count()<2.0&&MiniMapMap::HasWorld()&&MiniMapSmelterCapture::CanAcceptCandidate()&&
                v.Service(owner->Published,B::GetSnapshot(),&CopiedRecord,&ProcessCandidate);
            if(changed)MiniMapBuildingSprites::FinishReevaluation(v.LastCompletedKey,v.SourcePublication);
            if(now>=owner->NextPipelineExport)
            {owner->Dirty|=8;owner->NextPipelineExport=now+5;}
#else
            const bool changed=MiniMapMap::HasWorld()&&v.Service(owner->Published,B::GetSnapshot(),&CopiedRecord,&MiniMapBuildingRepresentation::ValidateRepresentation);
#endif
            if(result==C::Publication::Invalidated||((changed||result==C::Publication::Ready)&&
                (v.Complete()||now>=owner->NextValidationExport)))
            {
                owner->Dirty|=2;
#if defined(MINIMAP_STAGE_R26B4_CLASSIFICATION)
                owner->Dirty|=4;
#endif
                owner->NextValidationExport=now+2;
            }
#endif
            // Never combine a native audit and diagnostic serialization in one tick.
            // CPU-owned dirty documents resume on a later service invocation.
#if defined(MINIMAP_STAGE_R26B_SPRITE_VALIDATION)
            if(changed)return;
#endif
            if(std::chrono::duration<double,std::milli>(Clock::now()-tickStart).count()>=2.0)return;
#if defined(MINIMAP_BUILDING_SPRITES)
            MiniMapBuildingSprites::TimingScope buildTiming(MiniMapBuildingSprites::Timing::DiagnosticBuild);
#endif
            MiniMapCallbackTiming::Phase diagnosticPhase(MiniMapCallbackTiming::Id::DiscoveryDiagnostics);
            if(owner->Dirty&1){Publish(C::Json(owner->Published,C::OutputLimit,result==C::Publication::Invalidated));owner->Dirty&=~1u;}
#if defined(MINIMAP_STAGE_R26B_SPRITE_VALIDATION)
            else if(owner->Dirty&2){Publish(MiniMapSpriteValidation::Json(v,owner->Published),true);owner->Dirty&=~2u;}
#if defined(MINIMAP_STAGE_R26B4_CLASSIFICATION)
            else if(owner->Dirty&4){Publish(MiniMapSpriteValidation::ClassificationJson(v,owner->Published),false,true);owner->Dirty&=~4u;}
#endif
#endif
#if defined(MINIMAP_BUILDING_SPRITES)
            else if(owner->Dirty&8){Publish(MiniMapBuildingSprites::Diagnostics(),false,false,true);owner->Dirty&=~8u;}
#endif
        }
    }
    void Initialize()
    {
        auto* self=GetSelf();if(owner||!self||!self->hooks||!self->hooks->Engine||!self->hooks->World)return;
        if(!self->hooks->Engine->RegisterOnTick||!self->hooks->Engine->UnregisterOnTick||
            !self->hooks->World->RegisterOnBeforeWorldEndPlay||!self->hooks->World->UnregisterOnBeforeWorldEndPlay)return;
        owner=new Owner;self->hooks->Engine->RegisterOnTick(&Tick);
        self->hooks->World->RegisterOnBeforeWorldEndPlay(&BeforeEndPlay);
    }
    void Shutdown()
    {
        if(!owner)return;auto* self=GetSelf();
        self->hooks->Engine->UnregisterOnTick(&Tick);self->hooks->World->UnregisterOnBeforeWorldEndPlay(&BeforeEndPlay);
        delete owner;owner=nullptr;
#if defined(MINIMAP_BUILDING_SPRITES)
        MiniMapBuildingSprites::Reset(0);
#endif
    }
}
#endif

