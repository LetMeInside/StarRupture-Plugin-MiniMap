#if defined(MODLOADER_CLIENT_BUILD) && defined(MINIMAP_BUILDING_SPRITES)
#include "BuildingSprites.h"
#include "Map/FogOfWar/FogOfWar.h"
#include "SpriteCandidateIdentity.h"
#include "BuildingSpriteCapture.h"
#include "CapturePng.h"
#include "RendererReadiness.h"
#include "plugin_helpers.h"
#include <mutex>
#include <limits>
#include <unordered_map>

namespace MiniMapBuildingSprites
{
    namespace P=MiniMapBuildingSpriteCore;namespace J=MiniMapRepresentationCore;
    struct Texture
    {
        PluginTextureHandle Handle{};void(*Free)(PluginTextureHandle){};
        MiniMapStaticAssembly::Footprint Bounds;
        std::array<double,4> Region{0,0,1,1}; // Original capture coordinates, normalized; includes rounding margins.
        ~Texture(){if(Handle&&Free)Free(Handle);} // v70 deferred texture retirement.
    };
    struct DrawState
    {
        std::unordered_map<MiniMapSpriteDiscoveryCore::Key,std::shared_ptr<Texture>,P::KeyHash> Ready;
    };
    namespace
    {
        struct TimingStats
        {
            size_t Calls{};double Last{},Min=(std::numeric_limits<double>::max)(),Max{},Total{};
            void Add(double ms){if(std::isfinite(ms)&&ms>=0){++Calls;Last=ms;Min=(std::min)(Min,ms);Max=(std::max)(Max,ms);Total+=ms;}}
        };
        struct Owner
        {
            P::Queue Jobs;
            std::array<std::shared_ptr<Texture>,MiniMapSpriteDiscoveryCore::Capacity> Textures;
            std::shared_ptr<const DrawState> View;
            std::array<TimingStats,size_t(Timing::Count)> Timings{};
            size_t Hits{},Fallbacks{},Completed{},Failed{};
            uint64_t UploadSequence{},RegistryRevision{};
            std::array<MiniMapSpriteDiscoveryCore::Key,MiniMapSpriteDiscoveryCore::Capacity> RegistryKeys{};
            size_t RegistryCount{},ReconcileCursor{},DependencyCursor{};
            bool Reconcile=false;
            std::chrono::steady_clock::time_point NextDependencyService{};
            std::array<size_t,size_t(DrawCounter::Count)> DrawCounts{};
            size_t DiagnosticFrames{},DiagnosticReports{},SampledHandleChecks{},SampledInvalidHandles{};
            size_t SpatialReports{},SpatialCompleted{};
            uint32_t ReportedDrawReasons{};
        };
        // Explicit lifetime through discovery; no owning static initializer.
        Owner* owner=nullptr;
        std::mutex access;
        bool SourceCurrent(const MiniMapSpriteDiscoveryCore::Key& key) // access held
        {
            if(!owner||owner->Jobs.Generation!=key.Generation)return false;
            for(size_t i=0;i<owner->RegistryCount;++i)if(owner->RegistryKeys[i]==key)return true;
            return false;
        }
        MiniMapRendererReadiness::NotificationAck NotifyLocked(P::Entry& e,
            const MiniMapRendererReadiness::ProviderObservation& observation,const std::vector<uint64_t>& source)
        {
            namespace R=MiniMapRendererReadiness;using Ack=R::NotificationAck;
            auto& d=e.Dependencies;const auto* provider=R::FindProvider(observation.Provider);
            if(observation.World!=e.Key.Generation)return Ack::Invalidated;
            if(source.empty())return Ack::Rejected;
            if(source!=d.Source)return Ack::Invalidated; // committed, validated recipe generation changed
            if(!provider||observation.Conditions!=provider->Conditions)return Ack::Rejected;
            for(size_t i=0;i<d.ResourceCount;++i)
            {
                auto& dependency=d.Resources[i];const auto& r=dependency.Evidence;
                if(!R::SameResource(r,observation)||dependency.ObjectIndex!=observation.ObjectIndex||
                    dependency.NameIndex!=observation.NameIndex||dependency.NameNumber!=observation.NameNumber)continue;
                if(r.ResourceVersion!=observation.ResourceVersion)
                    return r.ResourceVersion>observation.ResourceVersion?Ack::Invalidated:Ack::Rejected;
                const auto& recorded=dependency.Advancement;
                if(dependency.AdvancementRevision&&dependency.AdvancementSource==d.SourceRevision&&
                    R::SameResource(recorded,observation)&&recorded.ResourceVersion==observation.ResourceVersion&&
                    recorded.ObservationRevision==observation.ObservationRevision)return Ack::AlreadyRecorded;
                if(!d.Tracked||!observation.ObservationRevision||observation.ObservationRevision!=r.ObservationRevision)return Ack::Rejected;
                if(observation.RecaptureEligible)
                {
                    // Record provider-owned progress even when its current capture
                    // relevance is temporarily false; eligibility is independent.
                    const bool relevant=provider->Relevant(observation,r);
                    // Commit proof before acknowledging the notification. No
                    // allocation or engine dispatch occurs in these scalar writes.
                    dependency.Advancement=observation;dependency.AdvancementSource=d.SourceRevision;
                    dependency.AdvancementRevision=++d.AdvancementRevision;dependency.Relevant=relevant;
                }
                else if(observation.ObservationRevision<=dependency.LastNotificationRevision)return Ack::Observed;
                dependency.LastNotificationRevision=observation.ObservationRevision;
                ++d.ObservationRevision;d.Reevaluate|=observation.RequiresEvaluation;
                if(observation.RequiresEvaluation||observation.RecaptureEligible)++d.ChangeRevision;
                return observation.RecaptureEligible?Ack::Recorded:Ack::Observed;
            }
            return Ack::Rejected; // missing/mismatched provider is not proof of invalidation
        }
        bool CertifiedForEntry(const P::Entry& e,const MiniMapRendererReadiness::Result& quality)
        {
            const auto& current=e.Resources;
            return MiniMapRendererReadiness::Certifiable(quality,e.Key.Generation)&&
                current.State==MiniMapRendererReadiness::RenderReadiness::Ready&&!current.Unknown&&!current.Pending&&!current.Unsupported&&
                current.Generation==quality.Generation;
        }
        const char* DrawLabel(DrawCounter reason)
        {
            const char* labels[]={"calls","no_ready_view","generation_mismatch","invalid_candidate_inputs",
                "lookup_attempts","lookup_misses","lookup_matches","invalid_instance_transform",
                "quad_validation_failure","projection_failure","viewport_rejection","budget_rejection",
                "fog_of_war_rejection","invalid_texture_handle","image_quad_submissions"};
            return size_t(reason)<size_t(DrawCounter::Count)?labels[size_t(reason)]:"none";
        }

    }
    void Reset(uint64_t generation)
    {
        std::scoped_lock lock(access);
        if(!owner&&generation)owner=new Owner;
        if(owner&&owner->Jobs.Generation!=generation)
        {if(owner->Completed)LOG_INFO("MiniMap: sprite draw view retired: generation=%llu nextGeneration=%llu completed=%zu submissions=%zu",
            (unsigned long long)owner->Jobs.Generation,(unsigned long long)generation,owner->Completed,owner->Hits);
         owner->View.reset();owner->Textures={};
         owner->DrawCounts={};owner->DiagnosticFrames=owner->DiagnosticReports=0;
         owner->SpatialReports=owner->SpatialCompleted=0;
         owner->SampledHandleChecks=owner->SampledInvalidHandles=0;owner->ReportedDrawReasons=0;
         owner->Timings={};owner->Jobs.Reset(generation);owner->Hits=owner->Fallbacks=owner->Completed=owner->Failed=0;
         owner->RegistryCount=owner->ReconcileCursor=owner->DependencyCursor=0;owner->Reconcile=false;owner->NextDependencyService={};}
        if(!generation){delete owner;owner=nullptr;}
    }
    void Record(const MiniMapSpriteDiscoveryCore::Key& key,P::State state,const std::string& reason)
    {
        std::scoped_lock lock(access);if(!owner)return;
        std::shared_ptr<DrawState> view;
        if(state==P::State::Excluded&&owner->View)
        {view=std::make_shared<DrawState>(*owner->View);view->Ready.erase(key);}
        auto* e=owner->Jobs.Set(key,state,reason);if(!e)return;
        if(state==P::State::Excluded&&owner->Textures[size_t(e-owner->Jobs.Entries.data())])
        {owner->Textures[size_t(e-owner->Jobs.Entries.data())].reset();owner->View=std::move(view);}
    }
    P::Admission Queue(const MiniMapSpriteDiscoveryCore::Key& key)
    {
        std::scoped_lock lock(access);
        if(!owner||key.Generation!=owner->Jobs.Generation)return P::Admission::InvalidGeneration;
        if(!SourceCurrent(key))return P::Admission::SourceUnavailable;
        const auto result=owner->Jobs.QueueCapture(key);
        if(auto* e=owner->Jobs.Find(key))e->LastAdmission=result;return result;
    }
    bool WatchCurrent(const MiniMapSpriteDiscoveryCore::Key& key,const std::vector<uint64_t>& source)
    {std::scoped_lock lock(access);auto* e=SourceCurrent(key)?owner->Jobs.Find(key):nullptr;
        return e&&e->Dependencies.Tracked&&e->Dependencies.Source==source;}
    bool ReservationCurrent(const MiniMapSpriteDiscoveryCore::Key& key)
    {std::scoped_lock lock(access);auto* e=SourceCurrent(key)?owner->Jobs.Find(key):nullptr;return e&&owner->Jobs.ReservationCurrent(*e);}
    bool StartCapture(const MiniMapSpriteDiscoveryCore::Key& key)
    {std::scoped_lock lock(access);auto* e=SourceCurrent(key)?owner->Jobs.Find(key):nullptr;return e&&owner->Jobs.StartCapture(*e);}
    void AbandonCapture(const MiniMapSpriteDiscoveryCore::Key& key,bool transient) noexcept
    {
        try{std::scoped_lock lock(access);if(owner)if(auto* e=owner->Jobs.Find(key)){
            owner->Jobs.ReleaseCapture(*e,transient);
            if(transient){auto& d=e->Dependencies;const auto snapshot=MiniMapBuildingCollector::GetSnapshot();
                d.BlockedPublication=snapshot?snapshot->PublishedAtSeconds:-1;d.BlockedObservation=d.ChangeRevision;d.Reevaluate=false;}}}catch(...){}
    }
    bool DependencySnapshot(const MiniMapSpriteDiscoveryCore::Key& key,MiniMapRendererReadiness::Dependencies& out)
    {std::scoped_lock lock(access);auto* e=SourceCurrent(key)?owner->Jobs.Find(key):nullptr;if(!e)return false;out=e->Dependencies;return true;}
    void WatchState(const MiniMapSpriteDiscoveryCore::Key& key,MiniMapRendererReadiness::Dependencies::Watch state)
    {
        namespace R=MiniMapRendererReadiness;
        std::scoped_lock lock(access);auto* e=SourceCurrent(key)?owner->Jobs.Find(key):nullptr;if(!e)return;
        auto& d=e->Dependencies;d.ObservationWatch=state;
        if(state==R::Dependencies::Watch::None&&d.Demand)d.ObservationWatch=R::Dependencies::Watch::Waiting;
    }
    bool WaitingDemand()
    {
        // CPU-only and fixed registry capacity, invoked only on a 50-ms watch
        // service opportunity. No UObject work or inventory discovery.
        std::scoped_lock lock(access);if(!owner)return false;
        for(size_t i=0;i<owner->Jobs.Count;++i){const auto& d=owner->Jobs.Entries[i].Dependencies;
            if(d.Tracked&&d.Demand&&d.ObservationWatch==MiniMapRendererReadiness::Dependencies::Watch::Waiting)return true;}
        return false;
    }
    void AcknowledgeReevaluation(const MiniMapSpriteDiscoveryCore::Key& key,bool accepted)
    {
        std::scoped_lock lock(access);auto* e=SourceCurrent(key)?owner->Jobs.Find(key):nullptr;if(!e||!accepted)return;
        auto& d=e->Dependencies;d.EvaluationQueued=true;d.EvaluationRevision=d.ObservationRevision;d.Reevaluate=false;
    }
    void FinishReevaluation(const MiniMapSpriteDiscoveryCore::Key& key,double sourcePublication)
    {
        std::scoped_lock lock(access);auto* e=SourceCurrent(key)?owner->Jobs.Find(key):nullptr;if(!e)return;
        auto& d=e->Dependencies;d.EvaluationQueued=false;
        // A successful observation that is merely waiting for capacity may
        // retry as soon as a slot opens. Missing representatives/exceptions wait
        // for source/provider evidence to change.
        d.BlockedPublication=d.ObservationRevision==d.EvaluationRevision?sourcePublication:-1;
        d.BlockedObservation=d.ChangeRevision;
    }
    void ObserveResources(const MiniMapSpriteDiscoveryCore::Key& key,const MiniMapRendererReadiness::Result& resources)
    {
        std::scoped_lock lock(access);if(!SourceCurrent(key)||resources.Generation.World!=key.Generation)return;
        if(auto* entry=owner->Jobs.Find(key))
        {
            entry->Resources=resources;auto& d=entry->Dependencies;
            d.Missing=resources.Unknown|resources.Pending;
            if(resources.State==MiniMapRendererReadiness::RenderReadiness::Unknown&&!d.Missing)d.Missing=d.Required;
            d.Tracked=resources.State!=MiniMapRendererReadiness::RenderReadiness::Unsupported&&
                !MiniMapRendererReadiness::Certifiable(entry->Quality,key.Generation);
            d.ProviderEpoch=MiniMapRendererReadiness::ProviderEpoch();++d.ObservationRevision;
        }
    }
    void ObserveAssembly(const MiniMapSpriteDiscoveryCore::Key& key,const MiniMapRendererReadiness::AssemblyObservation& observation)
    {
        namespace R=MiniMapRendererReadiness;
        if(observation.TaskCount>observation.Tasks.size())return;
        ObserveResources(key,observation.Readiness);
        std::scoped_lock lock(access);if(!SourceCurrent(key))return;
        if(auto* e=owner->Jobs.Find(key))
        {
            auto& d=e->Dependencies;
            if(observation.Source.empty())return; // Unknown preparation cannot invalidate an established source
            const bool sameSource=d.Source==observation.Source;
            // Copy before mutation: allocation failure preserves earlier evidence.
            if(!sameSource){auto source=observation.Source;d.Source.swap(source);++d.SourceRevision;++d.ChangeRevision;}
            const auto previous=d.Resources;const auto previousCount=d.ResourceCount;
            d.ResourceCount=observation.TaskCount;d.Demand=false;
            for(size_t i=0;i<d.ResourceCount;++i)
            {
                const auto& t=observation.Tasks[i];auto& r=d.Resources[i];r={};
                if(sameSource)for(size_t n=0;n<previousCount;++n)
                    if(R::SameResource(previous[n].Evidence,t.Evidence)&&
                        previous[n].Evidence.ResourceVersion==t.Evidence.ResourceVersion&&previous[n].ObjectIndex==t.Resource.Index&&
                        previous[n].NameIndex==t.Resource.Name&&previous[n].NameNumber==t.Resource.Number){r=previous[n];break;}
                r.Evidence=t.Evidence;r.ObjectIndex=t.Resource.Index;r.NameIndex=t.Resource.Name;
                r.NameNumber=t.Resource.Number;r.Contributors=t.Contributors;
                const auto* provider=R::FindProvider(t.Evidence.Provider);
                r.Relevant=provider&&provider->Relevant&&provider->Relevant(r.Advancement,r.Evidence);
                d.Demand|=provider&&t.Evidence.Pending&&t.Evidence.CanChange&&t.Evidence.Method!=R::ObservationMethod::Unavailable;
                // Observed state and fresh advancement are committed together.
                // The separate notification is now an exact acknowledgment/replay.
                if(t.Evidence.RecaptureEligible){auto proof=t.Evidence;proof.RequiresEvaluation=false;
                    NotifyLocked(*e,proof,observation.Source);}
            }
            if(d.ObservationWatch!=R::Dependencies::Watch::Active&&d.ObservationWatch!=R::Dependencies::Watch::Retiring)
                d.ObservationWatch=d.Demand?R::Dependencies::Watch::Waiting:R::Dependencies::Watch::None;
        }
    }
    bool TracksDependencies(const MiniMapSpriteDiscoveryCore::Key& key)
    {std::scoped_lock lock(access);auto* e=SourceCurrent(key)?owner->Jobs.Find(key):nullptr;return e&&e->Dependencies.Tracked;}
    MiniMapRendererReadiness::NotificationAck NotifyProviderChange(const MiniMapSpriteDiscoveryCore::Key& key,
        const MiniMapRendererReadiness::ProviderObservation& observation,const std::vector<uint64_t>& source)
    {
        namespace R=MiniMapRendererReadiness;using Ack=R::NotificationAck;
        std::scoped_lock lock(access);
        if(!SourceCurrent(key))return Ack::Invalidated; // complete registry membership/world guard
        auto* e=owner->Jobs.Find(key);if(!e)return Ack::Rejected;
        if(e->Status==P::State::Excluded||e->Status==P::State::Unsupported||
            e->Resources.State==R::RenderReadiness::Unsupported)return Ack::Invalidated;
        return NotifyLocked(*e,observation,source);
    }
    std::array<uint64_t,5> PublicationWake(const MiniMapSpriteDiscoveryCore::Key& key)
    {
        std::scoped_lock lock(access);auto* e=SourceCurrent(key)?owner->Jobs.Find(key):nullptr;
        if(!e)return {};
        const auto& d=e->Dependencies;
        return {key.Generation,d.SourceRevision,d.ChangeRevision,owner->RegistryRevision,MiniMapRendererReadiness::ProviderEpoch()};
    }

    bool RequestProviderReevaluation(const MiniMapSpriteDiscoveryCore::Key& key,uint32_t id)
    {
        // For a future verified provider event whose dependency was previously
        // Unknown. This requests evaluation only; it never authorizes a recapture.
        const auto* p=MiniMapRendererReadiness::FindProvider(id);if(!p)return false;
        std::scoped_lock lock(access);auto* e=SourceCurrent(key)?owner->Jobs.Find(key):nullptr;
        if(!e||!e->Dependencies.Tracked||!(e->Dependencies.Missing&p->Conditions))return false;
        e->Dependencies.Reevaluate=true;++e->Dependencies.ChangeRevision;return true;
    }
    bool TakeReadinessReevaluation(MiniMapSpriteDiscoveryCore::Key& key,bool (*watchCapacity)())
    {
        namespace R=MiniMapRendererReadiness;
        std::scoped_lock lock(access);if(!owner)return false;
        const auto now=std::chrono::steady_clock::now();if(now<owner->NextDependencyService)return false;
        owner->NextDependencyService=now+std::chrono::milliseconds(50);
        // One CPU record per eligible invocation; Unknown-only has no native ticket.
        if(!owner->Jobs.Count)return false;
        auto& e=owner->Jobs.Entries[owner->DependencyCursor++%owner->Jobs.Count];auto& d=e.Dependencies;
        if(!d.Tracked||e.InFlight||d.EvaluationQueued||!SourceCurrent(e.Key))return false;
        const auto epoch=R::ProviderEpoch();
        if(d.ProviderEpoch!=epoch){d.Reevaluate|=(d.Missing&R::ProviderConditions())!=0;d.ProviderEpoch=epoch;}
        const bool waiting=d.Demand&&d.ObservationWatch==R::Dependencies::Watch::Waiting&&watchCapacity&&watchCapacity();
        // Preserve proof while the separate new-image budget cannot admit
        // an initial capture; capacity alone must not cause repeated source audits.
        const bool captureCapacity=e.Reserved||owner->Jobs.Reservations<P::Capacity;
        const bool advance=captureCapacity&&owner->Jobs.EligibleAdvancement(e)!=0;
        if(!d.Reevaluate&&!waiting&&!advance)return false;
        const auto snapshot=MiniMapBuildingCollector::GetSnapshot();
        if(!snapshot||!snapshot->Complete||snapshot->Generation!=e.Key.Generation)return false;
        // Failed reacquisition is retried on a new source publication/provider
        // observation, rather than auditing the unchanged source every 50 ms.
        if(!d.Reevaluate&&d.BlockedPublication==snapshot->PublishedAtSeconds&&d.BlockedObservation==d.ChangeRevision)return false;
        key=e.Key;return true;
    }
    void ObserveRegistry(const MiniMapSpriteDiscoveryCore::Registry& registry)
    {
        std::scoped_lock lock(access);
        if(!owner||registry.Generation!=owner->Jobs.Generation||!registry.Complete())return;
        ++owner->RegistryRevision;owner->RegistryCount=registry.Count;
        for(size_t i=0;i<registry.Count;++i)owner->RegistryKeys[i]=registry.Entries[i].Identity;
        owner->ReconcileCursor=0;owner->Reconcile=true;
    }
    void ServiceRegistry()
    {
        std::scoped_lock lock(access);if(!owner||!owner->Reconcile)return;
        // One CPU entry and at most 256 key comparisons, only after registry publication.
        if(owner->ReconcileCursor==owner->Jobs.Count){owner->Reconcile=false;return;}
        auto& e=owner->Jobs.Entries[owner->ReconcileCursor];bool present=false;
        for(size_t i=0;i<owner->RegistryCount;++i)if(owner->RegistryKeys[i]==e.Key){present=true;break;}
        if(!present)
        {
            // Allocate before invalidation so exceptions leave a coherent published view.
            std::shared_ptr<DrawState> view;
            if(owner->View){view=std::make_shared<DrawState>(*owner->View);view->Ready.erase(e.Key);}
            std::string reason="source_removed_from_complete_registry";
            e.Reason.swap(reason);e.InFlight=false;e.Dependencies={};e.Status=P::State::Unavailable;
            owner->Textures[owner->ReconcileCursor].reset();owner->View=std::move(view);
        }
        ++owner->ReconcileCursor;
    }
    void Complete(const MiniMapSpriteDiscoveryCore::Key& key,const MiniMapStaticAssembly::Footprint& bounds,const std::vector<uint8_t>& rgba,const MiniMapRendererReadiness::Result& quality)
    {
        TimingScope timing(Timing::TextureUpload);
        struct Finish {const MiniMapSpriteDiscoveryCore::Key& Key;~Finish(){AbandonCapture(Key);}} finish{key};
        uint64_t uploadSequence=0;
        // Game-thread completion only; no image work or uploads occur in Draw.
        {
            std::scoped_lock lock(access);
            const auto* entry=SourceCurrent(key)?owner->Jobs.Find(key):nullptr;
            if(!entry||!entry->InFlight)return;
            if(entry->Status==P::State::Ready&&!CertifiedForEntry(*entry,quality))return;
            uploadSequence=++owner->UploadSequence;
        }
        if(!P::HasCoverage(rgba))
        {LOG_WARN("MiniMap: sprite processing rejected: empty or invalid 256x256 capture; building=%u",key.Building);
         Record(key,P::State::Failed,"empty_or_invalid_sprite_coverage; footprint_preserved");return;}
        const auto start=std::chrono::steady_clock::now();
        double maxScale=1;
        bool scaleKnown=false;
        const auto snapshot=MiniMapBuildingCollector::GetSnapshot();
        if(snapshot&&snapshot->Complete&&snapshot->Generation==key.Generation&&
            snapshot->Records.size()<=MiniMapSpriteDiscoveryCore::RecordLimit)
            for(const auto& record:snapshot->Records)
            {
                if(!record.TransformValid||MiniMapSpriteCandidates::Copy(record).Identity!=key)continue;
                bool supported=true;for(double v:record.Scale)supported&=std::isfinite(v)&&v>0&&v<=1000;
                if(!supported)continue;
                scaleKnown=true;maxScale=(std::max)({maxScale,record.Scale[0],record.Scale[1]});
            }
        // The current loader Win32 backend uses framebuffer scale one and exposes no scale query.
        // Font/window resizing does not change MiniMap's ImGui-coordinate projection density.
        const double density=1/(100*MiniMapMap::MinimumMetersPerPixel)*maxScale;
        const double extent=MiniMapStaticAssembly::OrthoWidth(bounds);
        MiniMapCapturePng::ProcessedImage processed;
        if(scaleKnown&&bounds.Initialized)processed=MiniMapCapturePng::Process(rgba,256,256,extent,density);
        else processed.Reason="current_instance_scale_or_bounds_unavailable";
        const double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        const bool useProcessed=processed.Ok;
        const auto* pixels=useProcessed&&!processed.Passthrough?&processed.Rgba:&rgba;
        size_t width=useProcessed?processed.Width:256,height=useProcessed?processed.Height:256;
        std::array<double,4> region{0,0,1,1};
        if(useProcessed)region={double(processed.X0)/256,double(processed.Y0)/256,
            (processed.X0+width*processed.SourceStep)/256,(processed.Y0+height*processed.SourceStep)/256};
        LOG_INFO("MiniMap: sprite spatial processing: building=%u source=256x256 crop=[%zu,%zu)-[%zu,%zu) cropped=%zux%zu output=%zux%zu ratio=%.9g orthoCm=%.9g targetPixelsPerMeter=%.9g maxInstanceScale=%.9g framebufferScale=1 insufficient=%s durationMs=%.3f status=%s fallback=%s",
            key.Building,processed.X0,processed.Y0,processed.X1,processed.Y1,
            processed.X1>=processed.X0?processed.X1-processed.X0:0,processed.Y1>=processed.Y0?processed.Y1-processed.Y0:0,
            width,height,processed.Ratio,extent,density*100,maxScale,processed.Insufficient?"true":"false",elapsed,processed.Reason,useProcessed?"false":"true");
        auto* self=GetSelf();auto* api=self&&self->hooks?self->hooks->ImGuiTextures:nullptr;
        if(!api||!api->LoadFromRGBA||!api->FreeTexture||!api->GetFreeSlotCount||!api->GetFreeSlotCount())
        {LOG_WARN("MiniMap: sprite publication failed: texture upload unavailable or capacity; building=%u",key.Building);
         Record(key,P::State::Failed,"texture_upload_unavailable_or_capacity");return;}
        try
        {
            // Loader names are a deduplication key: the first image wins. Give
            // each upload its own name while preserving the logical sprite key.
            const auto name="MiniMap_Building_session_"+MiniMapSpriteDiscoveryCore::KeyText(key)+
                "_Image_"+std::to_string(uploadSequence);
            auto texture=std::make_shared<Texture>();texture->Free=api->FreeTexture;texture->Bounds=bounds;texture->Region=region;
            try {texture->Handle=api->LoadFromRGBA(pixels->data(),int(width),int(height),name.c_str());}catch(...){texture->Handle=nullptr;}
            if(!texture->Handle&&useProcessed&&!processed.Passthrough)
            {
                // A failed processed upload may retry the original validated canvas once.
                texture->Region={0,0,1,1};width=height=256;
                LOG_WARN("MiniMap: processed sprite upload failed; original canvas fallback; building=%u",key.Building);
                try {texture->Handle=api->LoadFromRGBA(rgba.data(),256,256,name.c_str());}catch(...){texture->Handle=nullptr;}
            }
            std::scoped_lock lock(access);if(!SourceCurrent(key))return;auto* entry=owner->Jobs.Find(key);
            if(!entry||!entry->InFlight)return;
            if(entry->Status==P::State::Ready&&!CertifiedForEntry(*entry,quality))return;
            if(!texture->Handle)
            {entry->InFlight=false;if(!owner->Textures[size_t(entry-owner->Jobs.Entries.data())])entry->Status=P::State::Failed;entry->Reason="texture_upload_failed; previous_sprite_retained_if_present";++owner->Failed;
             LOG_WARN("MiniMap: sprite publication failed: upload; building=%u",key.Building);return;}
            const auto& previous=owner->Textures[size_t(entry-owner->Jobs.Entries.data())];
            if(previous&&(previous->Bounds.Min!=bounds.Min||previous->Bounds.Max!=bounds.Max))
            {entry->InFlight=false;entry->Reason="replacement_canvas_changed; previous_sprite_retained";return;}
            // Stable key and authored canvas; cropped UV region may change.
            // Construct the complete immutable view before committing either pixels or metadata.
            auto view=std::make_shared<DrawState>();view->Ready.reserve(P::Capacity);
            for(size_t i=0;i<owner->Jobs.Count;++i)
                if((owner->Jobs.Entries[i].Status==P::State::Ready||owner->Jobs.Entries[i].Status==P::State::Provisional)&&owner->Textures[i])
                    view->Ready.emplace(owner->Jobs.Entries[i].Key,owner->Textures[i]);
            view->Ready.insert_or_assign(key,texture);
            const bool certified=CertifiedForEntry(*entry,quality);
            std::string reason=certified?"quality_certified_session_sprite":
                "provisional_session_sprite; renderer_quality_uncertified; static_reference_only";
            // No allocating metadata operations after beginning the commit.
            entry->Reason.swap(reason);
            owner->Textures[size_t(entry-owner->Jobs.Entries.data())]=std::move(texture);
            entry->Quality=quality;entry->InFlight=false;++entry->ImageRevision;
            entry->Status=certified?P::State::Ready:P::State::Provisional;
            if(certified)entry->Dependencies.Tracked=false;
            ++owner->Completed;owner->View=std::move(view);
            int backendWidth=0,backendHeight=0;
            if(api->GetSize)api->GetSize(owner->Textures[size_t(entry-owner->Jobs.Entries.data())]->Handle,&backendWidth,&backendHeight);
            LOG_INFO("MiniMap: general building sprite displayed (quality separately recorded): building=%u; session texture published %zux%zu; static reference only",key.Building,width,height);
            LOG_INFO("MiniMap: sprite publication draw view: key=%s ready=%zu requested=%zux%zu backend=%dx%d",
                MiniMapSpriteDiscoveryCore::KeyText(key).c_str(),owner->View->Ready.size(),width,height,backendWidth,backendHeight);
        }
        catch(...)
        {LOG_WARN("MiniMap: sprite publication failed: allocation or view construction; building=%u",key.Building);
         Record(key,P::State::Failed,"texture_publication_exception");}
    }
    DrawFrame BeginDraw()
    {
        std::scoped_lock lock(access);DrawFrame frame;
        if(owner)
        {
            frame.Generation=owner->Jobs.Generation;frame.View=owner->View;frame.ReportedReasons=owner->ReportedDrawReasons;
            if(owner->Completed&&owner->SpatialReports<8&&
                (owner->SpatialCompleted!=owner->Completed||owner->DiagnosticFrames==59||owner->DiagnosticFrames==299))
                try{frame.Spatial=std::make_unique<SpatialSnapshot>();}catch(...){} // Diagnostics cannot prevent drawing.
        }
        return frame;
    }
    void EndDraw(const DrawFrame& frame)
    {
        std::scoped_lock lock(access);if(!owner||owner->Jobs.Generation!=frame.Generation)return;
        owner->Hits+=frame.Hits;owner->Fallbacks+=frame.Fallbacks;
        for(size_t i=0;i<frame.Counts.size();++i)owner->DrawCounts[i]+=frame.Counts[i];
        if(frame.Spatial)
        {
            ++owner->SpatialReports;owner->SpatialCompleted=owner->Completed;
            bool ascendingZ=true;
            for(size_t i=1;i<frame.Spatial->Count;++i)
                ascendingZ&=frame.Spatial->Entries[i-1].Position[2]<=frame.Spatial->Entries[i].Position[2];
            LOG_INFO("MiniMap: sprite layering snapshot: generation=%llu frameAfterPublication=%zu submissions=%zu ascendingWorldZ=%d skipped=%zu lookupMisses=%zu projectionFailures=%zu viewportRejections=%zu fogRejections=%zu invalidHandles=%zu quadFailures=%zu details=%zu; order=world_Z_then_Y_then_X; spatial overlaps are screen AABBs, not opaque-pixel occlusion proof",
                (unsigned long long)frame.Generation,owner->DiagnosticFrames+1,frame.Hits,int(ascendingZ),
                frame.Counts[size_t(DrawCounter::Calls)]-frame.Hits,frame.Counts[size_t(DrawCounter::LookupMisses)],
                frame.Counts[size_t(DrawCounter::ProjectionFailure)],frame.Counts[size_t(DrawCounter::ViewportRejection)],
                frame.Counts[size_t(DrawCounter::FogRejection)],frame.Counts[size_t(DrawCounter::InvalidHandle)],frame.Counts[size_t(DrawCounter::QuadFailure)],
                (std::min)(frame.Spatial->Count,size_t(64)));
            for(size_t i=0;i<(std::min)(frame.Spatial->Count,size_t(64));++i)
            {
                const auto& spatial=frame.Spatial->Entries[i];
                size_t lowerOverlaps=0,equalOverlaps=0,higherOverlaps=0;
                for(size_t j=0;j<frame.Spatial->Count;++j)
                {
                    const auto& f=frame.Spatial->Entries[j];
                    if(j!=i&&spatial.ScreenBounds[0]<f.ScreenBounds[2]&&spatial.ScreenBounds[2]>f.ScreenBounds[0]&&
                        spatial.ScreenBounds[1]<f.ScreenBounds[3]&&spatial.ScreenBounds[3]>f.ScreenBounds[1])
                    {
                        if(f.Position[2]<spatial.Position[2])++lowerOverlaps;
                        else if(f.Position[2]>spatial.Position[2])++higherOverlaps;
                        else ++equalOverlaps;
                    }
                }
                int width=0,height=0;auto* self=GetSelf();auto* api=self&&self->hooks?self->hooks->ImGuiTextures:nullptr;
                if(api&&api->GetSize)api->GetSize(spatial.Handle,&width,&height);
                LOG_INFO("MiniMap: sprite layering submission: sequence=%zu entity=(%d,%d) building=%u worldCm=(%.6f,%.6f,%.6f) yawDegrees=%.6f worldSpriteCm=%.6fx%.6f screenAABB=[%.6f,%.6f]-[%.6f,%.6f] overlapsLowerEqualHigherZ=%zu,%zu,%zu texture=%dx%d; viewport intersection and FOW passed; capture foreground validated at publication, per-pixel overlap not measured",
                    i,spatial.Index,spatial.Serial,spatial.Building,
                    spatial.Position[0],spatial.Position[1],spatial.Position[2],spatial.Yaw,spatial.WorldWidth,spatial.WorldHeight,
                    spatial.ScreenBounds[0],spatial.ScreenBounds[1],spatial.ScreenBounds[2],spatial.ScreenBounds[3],lowerOverlaps,equalOverlaps,higherOverlaps,width,height);
            }
        }
        if(frame.SampleReason!=DrawCounter::Count&&!(owner->ReportedDrawReasons&(1u<<size_t(frame.SampleReason))))
        {
            owner->ReportedDrawReasons|=1u<<size_t(frame.SampleReason);
            int backendWidth=0,backendHeight=0;
            if(frame.View)
            {
                const auto found=frame.View->Ready.find(frame.SampleKey);
                auto* self=GetSelf();auto* api=self&&self->hooks?self->hooks->ImGuiTextures:nullptr;
                if(found!=frame.View->Ready.end()&&found->second&&api&&api->GetSize)
                {
                    api->GetSize(found->second->Handle,&backendWidth,&backendHeight);
                    ++owner->SampledHandleChecks;
                    owner->SampledInvalidHandles+=backendWidth<=0||backendHeight<=0;
                }
            }
            LOG_INFO("MiniMap: sprite draw sample: reason=%s key=%s representativePublishedKey=%s valid(definition,visual,transform)=%d,%d,%d backend=%dx%d position=(%.9g,%.9g,%.9g) scale=(%.9g,%.9g,%.9g) quaternion=(%.9g,%.9g,%.9g,%.9g) region=(%.9g,%.9g,%.9g,%.9g) geometry=%d projected=%d world=(%.9g,%.9g;%.9g,%.9g;%.9g,%.9g;%.9g,%.9g) screen=(%.9g,%.9g;%.9g,%.9g;%.9g,%.9g;%.9g,%.9g)",
                DrawLabel(frame.SampleReason),MiniMapSpriteDiscoveryCore::KeyText(frame.SampleKey).c_str(),
                MiniMapSpriteDiscoveryCore::KeyText(frame.SamplePublishedKey).c_str(),
                int(frame.SampleDefinitionValid),int(frame.SampleVisualValid),int(frame.SampleTransformValid),backendWidth,backendHeight,
                frame.SamplePosition[0],frame.SamplePosition[1],frame.SamplePosition[2],frame.SampleScale[0],frame.SampleScale[1],frame.SampleScale[2],
                frame.SampleRotation[0],frame.SampleRotation[1],frame.SampleRotation[2],frame.SampleRotation[3],
                frame.SampleRegion[0],frame.SampleRegion[1],frame.SampleRegion[2],frame.SampleRegion[3],int(frame.SampleGeometry),int(frame.SampleProjection),
                frame.SampleWorld[0][0],frame.SampleWorld[0][1],frame.SampleWorld[1][0],frame.SampleWorld[1][1],
                frame.SampleWorld[2][0],frame.SampleWorld[2][1],frame.SampleWorld[3][0],frame.SampleWorld[3][1],
                frame.SampleScreen[0][0],frame.SampleScreen[0][1],frame.SampleScreen[1][0],frame.SampleScreen[1][1],
                frame.SampleScreen[2][0],frame.SampleScreen[2][1],frame.SampleScreen[3][0],frame.SampleScreen[3][1]);
            if(frame.SampleProjection)LOG_INFO("MiniMap: sprite draw sample viewport: min=(%.9g,%.9g) max=(%.9g,%.9g)",
                frame.SampleViewport[0],frame.SampleViewport[1],frame.SampleViewport[2],frame.SampleViewport[3]);
        }
        // Six cumulative summaries after publication, independent of the game's frame rate.
        constexpr size_t reportFrames[]={1,60,300,1800,3600,7200};
        if(owner->Completed&&owner->DiagnosticReports<std::size(reportFrames)&&
            ++owner->DiagnosticFrames>=reportFrames[owner->DiagnosticReports])
        {
            ++owner->DiagnosticReports;const auto& c=owner->DrawCounts;
            LOG_INFO("MiniMap: sprite draw counters: generation=%llu framesAfterPublication=%zu ready=%zu calls=%zu noReadyView=%zu generationMismatch=%zu invalidInputs=%zu lookupAttempts=%zu lookupMisses=%zu lookupMatches=%zu invalidTransforms=%zu quadFailures=%zu projectionFailures=%zu viewportRejections=%zu budgetRejections=%zu fogRejections=%zu invalidHandles=%zu submissions=%zu sampledBackendHandleChecks=%zu sampledBackendInvalidHandles=%zu",
                (unsigned long long)frame.Generation,owner->DiagnosticFrames,frame.View?frame.View->Ready.size():0,
                c[0],c[1],c[2],c[3],c[4],c[5],c[6],c[7],c[8],c[9],c[10],c[11],c[12],c[13],c[14],owner->SampledHandleChecks,owner->SampledInvalidHandles);
        }
    }
    bool Draw(DrawFrame& frame,IModLoaderImGui* ui,PluginDrawList dl,const MiniMapMap::Transform& transform,const MiniMapBuildingCollector::Record& record)
    {
        auto count=[&](DrawCounter reason){++frame.Counts[size_t(reason)];};
        auto sample=[&](DrawCounter reason,const Texture* texture=nullptr,
            const std::array<std::array<double,2>,4>* quad=nullptr,
            const std::array<MiniMapMap::ScreenPoint,4>* screen=nullptr)
        {
            if(frame.SampleReason!=DrawCounter::Count||(frame.ReportedReasons&(1u<<size_t(reason))))return;
            frame.SampleReason=reason;const auto input=MiniMapSpriteCandidates::Copy(record);
            frame.SampleKey=input.Identity;frame.SampleDefinitionValid=input.DefinitionValid;
            frame.SampleVisualValid=input.VisualInputsValid;frame.SampleTransformValid=record.TransformValid;
            frame.SamplePosition=record.Position;frame.SampleScale=record.Scale;frame.SampleRotation=record.Rotation;
            frame.SampleViewport={transform.CenterX-transform.Width/2,transform.CenterY-transform.Height/2,
                transform.CenterX+transform.Width/2,transform.CenterY+transform.Height/2};
            if(frame.View&&!frame.View->Ready.empty())
            {
                frame.SamplePublishedKey=frame.View->Ready.begin()->first;
                for(const auto& [key,value]:frame.View->Ready)if(key.Building==record.BuildingId)
                {frame.SamplePublishedKey=key;break;}
            }
            if(texture)frame.SampleRegion=texture->Region;
            if(quad){frame.SampleWorld=*quad;frame.SampleGeometry=true;}
            if(screen){for(size_t i=0;i<4;++i)frame.SampleScreen[i]={(*screen)[i].X,(*screen)[i].Y};frame.SampleProjection=true;}
        };
        count(DrawCounter::Calls);
        if(record.Generation!=frame.Generation)
        {count(DrawCounter::GenerationMismatch);sample(DrawCounter::GenerationMismatch);return false;}
        // No native reads or key lookup for an empty immutable view.
        if(!frame.View||frame.View->Ready.empty())
        {count(DrawCounter::NoReadyView);sample(DrawCounter::NoReadyView);return false;}
        const auto copy=MiniMapSpriteCandidates::Copy(record);
        if(!copy.DefinitionValid||!copy.VisualInputsValid)
        {count(DrawCounter::InvalidInputs);sample(DrawCounter::InvalidInputs);return false;}
        count(DrawCounter::LookupAttempts);
        const auto found=frame.View->Ready.find(copy.Identity);
        if(found==frame.View->Ready.end())
        {count(DrawCounter::LookupMisses);sample(DrawCounter::LookupMisses);return false;}
        count(DrawCounter::LookupMatches);
        const auto& texture=found->second; // frame owns the texture through draw submission
        if(!texture||!texture->Handle)
        {count(DrawCounter::InvalidHandle);sample(DrawCounter::InvalidHandle,texture.get());return false;}
        std::array<std::array<double,2>,4> quad{};
        if(!record.TransformValid)
        {count(DrawCounter::InvalidTransform);sample(DrawCounter::InvalidTransform,texture.get());return false;}
        if(!P::Quad(record.Position,record.Rotation,record.Scale,texture->Bounds,quad,texture->Region))
        {count(DrawCounter::QuadFailure);sample(DrawCounter::QuadFailure,texture.get());return false;}
        // Require every FOW cell intersecting the entire sprite AABB revealed.
        // Fails closed at the mask-read budget; no geometry leaks through holes.
        std::array<MiniMapMap::ScreenPoint,4> p;
        for(size_t i=0;i<4;++i)if(!transform.WorldToScreen(quad[i][0],quad[i][1],p[i]))
        {count(DrawCounter::ProjectionFailure);sample(DrawCounter::ProjectionFailure,texture.get(),&quad);return false;}
        float sx=p[0].X,sy=p[0].Y,ex=sx,ey=sy;
        for(const auto& v:p){sx=(std::min)(sx,v.X);sy=(std::min)(sy,v.Y);ex=(std::max)(ex,v.X);ey=(std::max)(ey,v.Y);}
        if(ex<transform.CenterX-transform.Width/2||sx>transform.CenterX+transform.Width/2||
            ey<transform.CenterY-transform.Height/2||sy>transform.CenterY+transform.Height/2)
        {count(DrawCounter::ViewportRejection);sample(DrawCounter::ViewportRejection,texture.get(),&quad,&p);return false;}
        if(!frame.Budget)
        {count(DrawCounter::BudgetRejection);sample(DrawCounter::BudgetRejection,texture.get(),&quad,&p);return false;}--frame.Budget;
        double minX=quad[0][0],minY=quad[0][1],maxX=minX,maxY=minY;
        for(const auto& v:quad){minX=(std::min)(minX,v[0]);minY=(std::min)(minY,v[1]);maxX=(std::max)(maxX,v[0]);maxY=(std::max)(maxY,v[1]);}
        if(!MiniMapFogOfWar::IsNativeWorldRectangleRevealed(minX,minY,maxX,maxY))
        {count(DrawCounter::FogRejection);sample(DrawCounter::FogRejection,texture.get(),&quad,&p);return false;}
        ui->DL_AddImageQuad(dl,texture->Handle,p[0].X,p[0].Y,p[1].X,p[1].Y,p[2].X,p[2].Y,p[3].X,p[3].Y,0,0,1,0,1,1,0,1,0xffffffff);
        if(frame.Spatial&&frame.Spatial->Count<frame.Spatial->Entries.size())
        {
            auto& spatial=frame.Spatial->Entries[frame.Spatial->Count++];
            spatial.Index=record.Index;spatial.Serial=record.Serial;spatial.Building=record.BuildingId;
            spatial.Handle=texture->Handle;
            spatial.Position=record.Position;spatial.ScreenBounds={sx,sy,ex,ey};
            const auto& q=record.Rotation;double norm=0;for(double v:q)norm+=v*v;
            spatial.Yaw=std::atan2(2*(q[3]*q[2]+q[0]*q[1]),norm-2*(q[1]*q[1]+q[2]*q[2]))*180/3.14159265358979323846;
            spatial.WorldWidth=std::hypot(quad[1][0]-quad[0][0],quad[1][1]-quad[0][1]);
            spatial.WorldHeight=std::hypot(quad[3][0]-quad[0][0],quad[3][1]-quad[0][1]);
        }
        count(DrawCounter::Submissions);sample(DrawCounter::Submissions,texture.get(),&quad,&p);
        ++frame.Hits;return true;
    }
    void RecordTiming(Timing kind,double ms) noexcept
    {
        try {std::scoped_lock lock(access);if(owner&&size_t(kind)<owner->Timings.size())owner->Timings[size_t(kind)].Add(ms);}catch(...){}
    }
    std::string Diagnostics()
    {
        std::scoped_lock lock(access);if(!owner)return "{}";
        const char* names[]={"discovery_service","capture_service","texture_upload","widget_cpu","widget_interval","registry_scan","representation_audit","native_class_resolution","classification","representation_traversal","assembly_recipe","capture_admission","diagnostic_build","diagnostic_publish","readiness_service","readiness_sample"};
        std::string timings="{";
        for(size_t i=0;i<owner->Timings.size();++i)
        {
            const auto& t=owner->Timings[i];if(i)timings+=',';
            timings+=J::Quote(names[i])+":"+J::Object({{"calls",std::to_string(t.Calls)},
                {"last",J::Number(t.Last)},{"min",J::Number(t.Calls?t.Min:0)},{"total",J::Number(t.Total)},{"max",J::Number(t.Max)},{"mean",J::Number(t.Calls?t.Total/double(t.Calls):0)}});
        }
        timings+="}";
        return P::Json(owner->Jobs,owner->Hits,owner->Fallbacks,MiniMapSmelterCapture::CandidateServiceStatus(),timings);
    }
}
#endif

