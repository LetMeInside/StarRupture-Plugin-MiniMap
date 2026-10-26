#include "Map/Map.h"
#include "CallbackTiming.h"
#if defined(MODLOADER_CLIENT_BUILD) && defined(MINIMAP_STAGE_R26A_SPRITE_DISCOVERY)
#include "SpriteDiscoveryCore.h"
#include "SpriteCandidateIdentity.h"
#if defined(MINIMAP_STAGE_R26B_SPRITE_VALIDATION)
#include "SpriteValidationCore.h"
#if defined(MINIMAP_BUILDING_SPRITES)
#include "CaptureFifoRuntimeCore.h"
#endif
#include "BuildingRepresentation.h"
#endif
#include "plugin.h"
#include "plugin_helpers.h"
#include "SpriteDiscovery.h"
#include "BuildingSpriteCapture.h"
#include "BuildingSprites.h"
#include "Map/Buildings/BuildingCollector.h"
#include "UI/MiniMapUI.h"
#include "Map/Terrain/TerrainCache.h"
#include "CacheIdentity.h"
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
        // CPU strings, copied collector records and filesystem paths only;
        // no UObject ownership, native calls or world scanning on the worker.
        // Four coalescing slots cap pending output at 1 MiB plus one in-flight file.
        class DiagnosticWriter
        {
            std::mutex Mutex;std::condition_variable Wake;
            std::array<std::string,4> Pending;
            std::array<uint64_t,4> Generations{};
            std::atomic<uint64_t> Generation{},Failures{};
            bool Stopping=false;std::thread Worker;
            std::shared_ptr<C::SourceRetirement<B::Snapshot>> Sources=std::make_shared<C::SourceRetirement<B::Snapshot>>();
            void Run() noexcept
            {
                for(;;)
                {
                    // Collector records own only copied CPU geometry and strings.
                    // Large vector/shared-metadata destruction never runs in Tick.
                    if(Sources->ServiceOne())continue;
                    std::string json;size_t kind=0;uint64_t generation=0;
                    {std::unique_lock lock(Mutex);Wake.wait_for(lock,std::chrono::milliseconds(20),[&]{return Stopping||Any();});
                     // Conditional P2: this drain does not wait for outstanding
                     // aliases. Releases after worker exit may go unserviced;
                     // ownership remains safe, but eventual reclamation is not assured.
                     if(Stopping){lock.unlock();if(!Sources->DrainReleased())++Failures;return;}
                     if(!Any())continue;
                     for(;kind<Pending.size();++kind)if(!Pending[kind].empty())break;
                     json=std::move(Pending[kind]);Pending[kind].clear();generation=Generations[kind];}
                    try {
#if defined(MINIMAP_BUILDING_SPRITES)
                        MiniMapBuildingSprites::TimingScope publicationTiming(MiniMapBuildingSprites::Timing::DiagnosticPublish);
#endif
                        MiniMapCacheIdentity::FileAccess access;
                        if (!access) continue;
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
            std::shared_ptr<const B::Snapshot> HoldSource(std::shared_ptr<const B::Snapshot> source)
            {
                if(!source)return {};
                {std::scoped_lock lock(Mutex);
                 if(Stopping)return {};
                 if(!Worker.joinable())Worker=std::thread([this]{Run();});}
                return Sources->Hold(std::move(source));
            }
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
            bool ConsumersReady=false;
            bool ConsumerBlockedReported=false,SourceFailureReported=false;
#if defined(MINIMAP_STAGE_R26B_SPRITE_VALIDATION)
            MiniMapSpriteValidation::Scheduler<B::Snapshot> Validation;
#if defined(MINIMAP_BUILDING_SPRITES)
            MiniMapCaptureFifoRuntime::Runtime<B::Snapshot> Fifo;
#endif
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
            owner->Invalidate(current?current->Generation:(owner->Published?owner->Published->Generation:0));
#if defined(MINIMAP_BUILDING_SPRITES)
            MiniMapBuildingSprites::Reset(0);
#endif
#if defined(MINIMAP_STAGE_R26B_SPRITE_VALIDATION)
            owner->Validation.Clear();owner->NextValidationExport=0;
#if defined(MINIMAP_BUILDING_SPRITES)
            owner->Fifo.CancelGeneration(0);
#endif
#endif
        }
        C::Input CopiedRecord(const B::Record& r)
        {
            return MiniMapSpriteCandidates::Copy(r);
        }
#if defined(MINIMAP_BUILDING_SPRITES)
        MiniMapSpriteValidation::Result ProcessCandidateImpl(const B::Record& record,const B::Record* alternate,bool auditOnly)
        {
            namespace P=MiniMapBuildingSpriteCore;namespace V=MiniMapSpriteValidation;
            const auto key=CopiedRecord(record).Identity;
            MiniMapCallbackTiming::Phase phase(MiniMapCallbackTiming::Id::DiscoveryCandidate);
            const auto start=Clock::now();
            auto out=MiniMapBuildingRepresentation::ValidateAndCapture(record,alternate,auditOnly);
            // Partial observations cannot exclude or otherwise mutate renderer
            // policy. Already-admitted native work retains its exact ownership.
            if(out.Disposition!=V::AuditDisposition::Completed)return out;
            const double elapsed=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
            LOG_INFO("MiniMap: sprite candidate audit building=%u took %.3f ms; route=%s reason=%s static=%zu; synchronous candidate validation",
                record.BuildingId,elapsed,out.Route.c_str(),out.Reason.c_str(),out.Static);
            if(out.Classification.CachedPolicy.ExcludeFromMiniMap==MiniMapSpriteClassification::Exclusion::Exclude)
                MiniMapBuildingSprites::Record(key,P::State::Excluded,out.Classification.CachedPolicy.Reason);
            else if(out.Status!=V::Outcome::Eligible)
                MiniMapBuildingSprites::Record(key,out.Status==V::Outcome::Unsupported?
                    P::State::Unsupported:P::State::Unavailable,out.Reason);
            else MiniMapBuildingSprites::Record(key,P::State::Queued,out.Route+"; "+out.Reason);
            return out;
        }
        MiniMapSpriteValidation::Result ProcessCandidate(const B::Record& record,const B::Record* alternate)
        {return ProcessCandidateImpl(record,alternate,false);}
        struct FifoHooks
        {
            decltype(&CopiedRecord) Copy=&CopiedRecord;
            auto Renderer(const C::Key& key,const std::shared_ptr<const C::Registry>& root){return MiniMapBuildingSprites::InspectDispatch(key,root);}
            auto Owner(){return MiniMapBuildingCapture::ObserveNativeOwner();}
            auto Audit(const B::Record& a,const B::Record* b){return ProcessCandidateImpl(a,b,true);}
            auto Handoff(const B::Record& a,const B::Record* b){return ProcessCandidate(a,b);}
            auto Arm(const MiniMapCaptureDispatch::OwnerObservation& before,const C::Key& key){return MiniMapBuildingCapture::ArmRetirementObservation(before,key);}
            auto Reconcile(const MiniMapCaptureLeases::ObservationTicket& ticket)noexcept{return MiniMapBuildingCapture::ReconcileRetirementObservation(ticket);}
            auto Observe(const MiniMapCaptureLeases::ObservationTicket& ticket)noexcept{return MiniMapBuildingSprites::ObserveRetirement(ticket);}
            bool Acknowledge(const MiniMapCaptureLeases::ObservationTicket& ticket,const MiniMapCaptureLeases::Token& lease)noexcept{return MiniMapBuildingSprites::AcknowledgeRetirement(ticket,lease);}
            bool CancelObservation(const MiniMapCaptureLeases::ObservationTicket& ticket)noexcept{return MiniMapBuildingSprites::CancelUnadmittedObservation(ticket);}
            void AuditFinished(const C::Key&,double){}
        };
#endif
        void Tick(float)
        {
        MiniMapCallbackTiming::Scope callbackTiming(MiniMapCallbackTiming::Id::DiscoveryTick,true);
#if defined(MINIMAP_BUILDING_SPRITES)
        MiniMapBuildingSprites::ServiceMaintenance();
#if defined(MINIMAP_STAGE_R26B_SPRITE_VALIDATION)
        if(owner){
            FifoHooks hooks;owner->Fifo.Reconcile(hooks);
            owner->Fifo.Work.MaintainRetired(0.25,4);
        }
#endif
#endif
#if defined(MINIMAP_STAGE_R26B_SPRITE_VALIDATION)
        MiniMapBuildingRepresentation::ServiceIdentityMaintenance();
#endif
        if(!MiniMapUI::IsVisible())return;
        if(!MiniMapMap::GameplayReady())
        {
            if(owner){const auto start=Clock::now();owner->MaintainRetired(8,2.0);
#if defined(MINIMAP_STAGE_R26B_SPRITE_VALIDATION)
                owner->Validation.MaintainRetired(8,(std::max)(0.0,2.0-std::chrono::duration<double,std::milli>(Clock::now()-start).count()));
#endif
            }return;
        }
            if(!owner)return;
#if defined(MINIMAP_BUILDING_SPRITES)
            MiniMapBuildingSprites::TimingScope timing(MiniMapBuildingSprites::Timing::DiscoveryService);
#endif
            const auto errors=owner->Writer.ErrorCount();
            if(errors!=owner->WriterErrors){owner->WriterErrors=errors;LOG_WARN("MiniMap: diagnostic publication failures=%llu; sprite capture/display unaffected",(unsigned long long)errors);}
            const double now=std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
            const auto tickStart=Clock::now();
            C::Publication result;
            std::shared_ptr<const B::Snapshot> source;
            try{source=owner->Writer.HoldSource(B::GetSnapshot());owner->SourceFailureReported=false;}
            catch(...)
            {
                if(!owner->SourceFailureReported)LOG_WARN("MiniMap: discovery source-retirement preparation failed; admission deferred, scan retained");
                owner->SourceFailureReported=true;owner->MaintainRetired(16,2.0);return;
            }
            {
#if defined(MINIMAP_BUILDING_SPRITES)
                MiniMapBuildingSprites::TimingScope registryTiming(MiniMapBuildingSprites::Timing::RegistryScan);
#endif
                MiniMapCallbackTiming::Phase phase(MiniMapCallbackTiming::Id::DiscoveryRegistry);
                const auto remaining=(std::max)(0.0,2.0-std::chrono::duration<double,std::milli>(Clock::now()-tickStart).count());
                result=owner->Service(source,MiniMapMap::HasWorld(),now,&CopiedRecord,remaining);
            }
            if(owner->WriterGeneration!=(owner->Published?owner->Published->Generation:0))
            {owner->WriterGeneration=(owner->Published?owner->Published->Generation:0);owner->Writer.Invalidate(owner->WriterGeneration);owner->Dirty=0;}
            if(result!=C::Publication::None)owner->Dirty|=1;
            const auto publication=owner->Published; // O(1) immutable owner, never a registry copy.
            const bool compatible=publication&&publication->Complete()&&source&&publication->Generation==source->Generation;
            const C::Registry empty; // Null is an invalidated/no-publication state, not authoritative empty membership.
            const auto& registry=publication?*publication:empty;
            if(result==C::Publication::Invalidated)owner->ConsumersReady=false;
            if(result==C::Publication::Ready)
            {
                owner->ConsumersReady=false;
                // Both membership and validation adoption are dynamic; native
                // work remains gated until exact publication adoption completes.
                if(!compatible&&!owner->ConsumerBlockedReported)
                    LOG_WARN("MiniMap: complete discovery publication retained (%zu keys, %zu records); source generation invalid; native validation deferred",registry.Count,registry.SnapshotRecords);
                owner->ConsumerBlockedReported=!compatible;
            }
#if defined(MINIMAP_STAGE_R26B_SPRITE_VALIDATION)
            auto& v=owner->Validation;
            if(result==C::Publication::Invalidated){v.Clear();owner->NextValidationExport=0;
#if defined(MINIMAP_BUILDING_SPRITES)
                owner->Fifo.CancelGeneration(0);
#endif
            }
            // Request incremental adoption of every complete publication.
            if(result==C::Publication::Ready)v.Observe(publication,source?source->Generation:0);
            v.MaintainRetired(16,(std::max)(0.0,2.0-std::chrono::duration<double,std::milli>(Clock::now()-tickStart).count()));
            const auto adoptionRemaining=(std::max)(0.0,2.0-std::chrono::duration<double,std::milli>(Clock::now()-tickStart).count());
            const bool adoptionCompleted=v.ServiceAdoption(adoptionRemaining);
            const bool validationCurrent=v.ReadyFor(registry);
            if(adoptionCompleted||v.AdoptionAllocationBlocked)
            {
                owner->Dirty|=2;
#if defined(MINIMAP_STAGE_R26B4_CLASSIFICATION)
                owner->Dirty|=4;
#endif
            }
#if defined(MINIMAP_BUILDING_SPRITES)
            MiniMapBuildingSprites::Reset(MiniMapMap::HasWorld()?(owner->Published?owner->Published->Generation:0):0);
            // O(1), allocation-free membership adoption; repeat calls are no-ops.
            // Invalidated/null publication explicitly closes previous membership.
            const bool rendererCurrent=MiniMapBuildingSprites::ObserveRegistry(publication);
            // Initialization is resumable; no complete-publication metadata loop.
            if(rendererCurrent&&publication&&!owner->ConsumersReady)
            {
                owner->ConsumersReady=MiniMapBuildingSprites::ServiceInitialization(
                    (std::max)(0.0,2.0-std::chrono::duration<double,std::milli>(Clock::now()-tickStart).count()));
            }
            bool changed=false;
            if(compatible&&MiniMapMap::HasWorld()&&std::chrono::duration<double,std::milli>(Clock::now()-tickStart).count()<2.0)
            {
                FifoHooks hooks;
                try{
                    if(MiniMapBuildingRepresentation::PrepareNativeClasses())changed=owner->Fifo.Service(v,source,hooks,
                        (std::max)(0.0,2.0-std::chrono::duration<double,std::milli>(Clock::now()-tickStart).count()));
                }catch(...){if(owner->Fifo.Failures++==0)LOG_WARN("MiniMap: FIFO CPU service interrupted; existing native observation retained; admission deferred");}
            }
            if(now>=owner->NextPipelineExport)
            {owner->Dirty|=8;owner->NextPipelineExport=now+5;}
#else
            const bool changed=compatible&&validationCurrent&&MiniMapMap::HasWorld()&&v.Service(registry,source,&CopiedRecord,&MiniMapBuildingRepresentation::ValidateRepresentation,(std::max)(0.0,2.0-std::chrono::duration<double,std::milli>(Clock::now()-tickStart).count()));
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
            if(owner->Dirty&1)
            {
                // Legacy detailed JSON counts definitions quadratically. Keep that
                // bounded until its incremental serializer is implemented.
                if(registry.Count<=C::DiagnosticCandidateLimit)Publish(C::Json(registry,C::OutputLimit,!publication));
                else Publish(MiniMapRepresentationCore::Object({
                    {"world_generation",std::to_string(registry.Generation)},
                    {"source_publication_steady_seconds",MiniMapRepresentationCore::Number(registry.PublicationSeconds)},
                    {"registry_complete",registry.Complete()?"true":"false"},
                    {"distinct_candidate_keys",std::to_string(registry.Count)},
                    {"snapshot_records",std::to_string(registry.SnapshotRecords)},
                    {"output_truncated","true"},{"diagnostic_detail_omitted","true"},
                    {"detail_omission_reason",MiniMapRepresentationCore::Quote("diagnostic_candidate_limit")},{"candidates","[]"}}));
                owner->Dirty&=~1u;
            }
#if defined(MINIMAP_STAGE_R26B_SPRITE_VALIDATION)
            else if(owner->Dirty&2)
            {
                Publish(compatible&&validationCurrent?MiniMapSpriteValidation::Json(v,registry):
                    MiniMapSpriteValidation::AdoptionStatusJson(v,registry),true);
                owner->Dirty&=~2u;
            }
#if defined(MINIMAP_STAGE_R26B4_CLASSIFICATION)
            else if(owner->Dirty&4)
            {
                Publish(compatible&&validationCurrent?MiniMapSpriteValidation::ClassificationJson(v,registry):
                    MiniMapSpriteValidation::AdoptionStatusJson(v,registry,"R2.6b.4"),false,true);
                owner->Dirty&=~4u;
            }
#endif
#endif
#if defined(MINIMAP_BUILDING_SPRITES)
            else if(owner->Dirty&8){Publish(MiniMapBuildingSprites::Diagnostics(),false,false,true);owner->Dirty&=~8u;}
#endif
        }
    }
#if defined(MINIMAP_STAGE_R26B_SPRITE_VALIDATION) && defined(MINIMAP_BUILDING_SPRITES)
    MiniMapCaptureDispatch::Readiness QueryDispatchReadiness(const C::Key& key,MiniMapCaptureWork::Revision expected) noexcept
    {
        try{
            const auto native=MiniMapBuildingCapture::ObserveNativeOwner();
            if(!native.QueryAllowed)return {};
            if(!owner)return {MiniMapCaptureDispatch::State::Cancelled,MiniMapCaptureDispatch::Reason::Generation};
            // Scheduling-thread CPU borrow: no native callbacks or ownership escape during this query.
            const auto source=B::GetSnapshot();
            if(!source||!source->Complete)return {};
            if(key.Generation!=source->Generation||owner->Validation.Generation!=key.Generation)
                return {MiniMapCaptureDispatch::State::Cancelled,MiniMapCaptureDispatch::Reason::Generation};
            if(!MiniMapMap::GameplayReady())return {};
            const auto latest=owner->Published;
            const auto renderer=MiniMapBuildingSprites::InspectDispatch(key,latest);
            return MiniMapCaptureDispatch::Query(owner->Validation,latest,key,expected,renderer,source->Generation,source->PublishedAtSeconds,native.Available);
        }catch(...){return {};}
    }
#endif
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
        // Release every discovery-owned source alias before joining its CPU
        // disposal worker. Final shutdown is a deliberate CPU drain boundary.
#if defined(MINIMAP_STAGE_R26B_SPRITE_VALIDATION)
        owner->Validation.Clear();
        while(owner->Validation.MaintainRetired(16,(std::numeric_limits<double>::max)())){}
#endif
        owner->Clear();
        auto retirement=owner->Retirement;
        delete owner;owner=nullptr;
#if defined(MINIMAP_BUILDING_SPRITES)
        MiniMapBuildingSprites::Shutdown(); // Detach renderer-held discovery publications first.
#endif
        if(!retirement->DrainReleased())
            LOG_WARN("MiniMap: discovery shutdown has retained registry readers; deferred retirement remains owned, future servicing required");
    }
}
#endif

