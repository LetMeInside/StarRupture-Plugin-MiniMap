#if defined(MODLOADER_CLIENT_BUILD) && defined(MINIMAP_BUILDING_SPRITES)
#include "BuildingSprites.h"
#include "BuildingSpriteCapture.h"
#include "CapturePng.h"
#include "plugin_helpers.h"
#include <mutex>
#include <limits>
#include "SessionSpritePublicationCore.h"
#include <set>

namespace MiniMapBuildingSprites
{
    namespace P=MiniMapBuildingSpriteCore;namespace J=MiniMapRepresentationCore;
    struct Texture
    {
        PluginTextureHandle Handle{};void(*Free)(PluginTextureHandle){};
        size_t RawBytes{};
        MiniMapStaticAssembly::Footprint Bounds;
        std::array<double,4> Region{0,0,1,1}; // Original capture coordinates, normalized; includes rounding margins.
        ~Texture(){if(Handle&&Free)Free(Handle);} // v70 deferred texture retirement.
    };
    using RenderGeneration=MiniMapSessionPublication::Store<Texture>;
    namespace
    {
        struct TimingStats
        {
            size_t Calls{};double Last{},Min=(std::numeric_limits<double>::max)(),Max{},Total{};
            void Add(double ms){if(std::isfinite(ms)&&ms>=0){++Calls;Last=ms;Min=(std::min)(Min,ms);Max=(std::max)(Max,ms);Total+=ms;}}
        };
        struct Owner
        {
            std::shared_ptr<RenderGeneration> Generation;
            RenderGeneration& Jobs;
            explicit Owner(uint64_t world):Generation(std::make_shared<RenderGeneration>(world)),Jobs(*Generation){}

            std::array<TimingStats,size_t(Timing::Count)> Timings{};
            size_t Hits{},Fallbacks{},Completed{},Failed{},SuccessfulTypes{},UploadFailures{};
            uint64_t UploadSequence{};
        };
        // Explicit lifetime through discovery; no owning static initializer.
        std::unique_ptr<Owner> owner;
        MiniMapSessionPublication::ResetResult resetResult=MiniMapSessionPublication::ResetResult::WorldEnded;
        uint64_t requestedGeneration{};size_t generationAllocationFailures{};
        std::mutex access;
        std::shared_ptr<RenderGeneration> retired;
        RenderGeneration* retiredTail=nullptr;
        // The access lock serializes policy mutation. Image versions are synchronized by Generation::Mutex.
        struct AccessLock
        {
            std::unique_lock<std::mutex> Lock{access};
            ~AccessLock()noexcept{if(owner)try{owner->Jobs.FlushDemand();}catch(...){std::terminate();}}
        };
        // The native request, not the renderer generation, owns this ledger.
        // Reset(0) must not make an old quarantined request look retired.
        MiniMapCaptureLeases::Ledger leases;
        bool SourceCurrent(const MiniMapSpriteDiscoveryCore::Key& key) // access held
        {
            return owner&&owner->Jobs.SourceCurrent(key);
        }

    }
    void Reset(uint64_t generation)
    {
        std::unique_ptr<Owner> previous;
        MiniMapSessionPublication::ResetResult result;
        {
            AccessLock lock;requestedGeneration=generation;
            result=MiniMapSessionPublication::TransitionOwner(owner,generation,
                [](uint64_t world){return std::make_unique<Owner>(world);},
                [](Owner& old){
                    old.Jobs.FlushDemand();old.Generation->Invalidate();
                    if(!retired)retiredTail=old.Generation.get();
                    old.Generation->NextRetired=std::move(retired);retired=old.Generation;
                },previous);
            resetResult=result;
            if(result==MiniMapSessionPublication::ResetResult::AllocationFailed)++generationAllocationFailures;
        }
        // previous is destroyed outside access/publication locks. Retired state
        // preserves frame/CPU ownership; the native ledger is never reset here.
        if(result==MiniMapSessionPublication::ResetResult::AllocationFailed)
            LOG_WARN("MiniMap: renderer generation allocation FAILED: requested=%llu; new-world admission disabled; old CPU state retired, native lease unchanged",
                (unsigned long long)generation);
    }
    void ServiceMaintenance()
    {
        const auto start=std::chrono::steady_clock::now();
        std::shared_ptr<RenderGeneration> current,old;
        {AccessLock lock;if(owner)current=owner->Generation;old=retired;}
        size_t remaining=16;
        if(current)remaining-=current->Maintain(old?8:remaining,2.0,false,false);
        // One retired owner per opportunity. Rotation prevents a pinned old frame
        // from blocking later owners; chain manipulation itself is allocation-free.
        if(old&&remaining&&std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<2.0)
            old->Maintain(remaining,2.0-std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
        std::shared_ptr<RenderGeneration> detached;
        {AccessLock lock;if(old&&retired==old){
            if(old->Empty()){detached=std::move(retired);retired=std::move(detached->NextRetired);if(!retired)retiredTail=nullptr;}
            else if(old->NextRetired){auto head=std::move(retired);retired=std::move(head->NextRetired);
                retiredTail->NextRetired=std::move(head);retiredTail=old.get();}
        }}
        // Empty-owner destruction outside all publication/owner-access locks.
    }
    void Shutdown()
    {
        Reset(0);
        std::shared_ptr<RenderGeneration> owners;
        {AccessLock lock;owners=std::move(retired);retiredTail=nullptr;}
        // Final CPU ownership release is distinct from world reset. Existing host
        // shutdown contract is still required: unregister does not drain callbacks.
        while(owners){auto next=std::move(owners->NextRetired);
            owners->DrainShutdown();owners=std::move(next);}
        // Native lease ledger remains reachable for certified native shutdown.
    }
    static void RecordLocked(const MiniMapSpriteDiscoveryCore::Key& key,P::State state,const std::string& reason)
    {
        if(!owner)return;
        auto* slot=owner->Jobs.Observe(key);if(!slot){
            const auto failures=owner->Jobs.AllocationFailures;
            if(failures&&(failures&(failures-1))==0)LOG_WARN("MiniMap: renderer slot allocation failed; failures=%zu; candidate retained upstream",failures);
            return;}
        if(state==P::State::Excluded&&!owner->Jobs.Exclude(*slot))
        {LOG_WARN("MiniMap: renderer exclusion revision failure; generation=%llu",(unsigned long long)key.Generation);return;}
        owner->Jobs.Set(key,state,reason);
    }
    void Record(const MiniMapSpriteDiscoveryCore::Key& key,P::State state,const std::string& reason)
    {AccessLock lock;RecordLocked(key,state,reason);}
    bool ServiceInitialization(double budgetMs)
    {AccessLock lock;return owner&&owner->Jobs.ServiceInitialization(budgetMs);}
    void RecordForLease(const MiniMapCaptureLeases::Token& token,P::State state,const std::string& reason)
    {
        AccessLock lock;auto* e=owner?owner->Jobs.Find(token.Candidate):nullptr;
        if(e&&P::Queue::MatchesLease(*e,token,leases))RecordLocked(token.Candidate,state,reason);
    }
    bool ReturnLease(const MiniMapCaptureLeases::Token& token,const MiniMapCaptureLeases::RetirementReceipt& receipt) noexcept
    {try{AccessLock lock;return leases.Return(token,receipt);}catch(...){return false;}}
    MiniMapCaptureDispatch::RendererEvidence InspectDispatch(const MiniMapSpriteDiscoveryCore::Key& key,
        const std::shared_ptr<const MiniMapSpriteDiscoveryCore::Registry>& latest) noexcept
    {
        try{
            // Raw lock: observational queries must not FlushDemand or mutate policy.
            std::scoped_lock lock(access);MiniMapCaptureDispatch::RendererEvidence out;
            if(owner)out=owner->Jobs.InspectDispatch(key,latest);
            out.LeaseHeld=leases.ActiveCount()!=0;return out;
        }catch(...){return {};}
    }
    MiniMapCaptureLeases::Inspection InspectLease(const MiniMapCaptureLeases::Token& token) noexcept
    {try{std::scoped_lock lock(access);return leases.Inspect(token);}catch(...){return MiniMapCaptureLeases::Inspection::Unknown;}}
    MiniMapCaptureLeases::Snapshot ObserveLeases() noexcept
    {try{std::scoped_lock lock(access);return leases.Observe();}catch(...){return {};}}
    MiniMapCaptureLeases::ArmResult ArmRetirementObservation(const MiniMapCaptureLeases::Snapshot& before,const MiniMapSpriteDiscoveryCore::Key& key) noexcept
    {try{std::scoped_lock lock(access);return leases.ArmRetirement(before,key);}catch(...){return {};}}
    void SealRetirementObservation(const MiniMapCaptureLeases::ObservationTicket& ticket) noexcept
    {try{std::scoped_lock lock(access);leases.SealObservation(ticket);}catch(...){/* Unsealed evidence stays quarantined by the guarded reconciliation contract. */}}
    MiniMapCaptureLeases::RetirementObservation ObserveRetirement(const MiniMapCaptureLeases::ObservationTicket& ticket) noexcept
    {try{std::scoped_lock lock(access);return leases.ObserveRetirement(ticket);}catch(...){return {};}}
    bool AcknowledgeRetirement(const MiniMapCaptureLeases::ObservationTicket& ticket,const MiniMapCaptureLeases::Token& token) noexcept
    {try{std::scoped_lock lock(access);return leases.AcknowledgeRetirement(ticket,token);}catch(...){return false;}}
    bool CancelUnadmittedObservation(const MiniMapCaptureLeases::ObservationTicket& ticket) noexcept
    {try{std::scoped_lock lock(access);return leases.CancelUnadmittedObservation(ticket);}catch(...){return false;}}
    size_t ActiveLeaseCount(){AccessLock lock;return leases.ActiveCount();}
    bool AcquireObservationLease(const MiniMapSpriteDiscoveryCore::Key& key,uint64_t sourceRevision,MiniMapCaptureLeases::Token& token)
    {
        // Called only by the guarded native owner after source validation and
        // request allocation. Does not grant a renderer admission or consume proof.
        AccessLock lock;return leases.Acquire(key,sourceRevision,token);
    }
    P::Admission Queue(const MiniMapSpriteDiscoveryCore::Key& key,MiniMapCaptureLeases::Token& token)
    {
        AccessLock lock;
        if(!owner||key.Generation!=owner->Jobs.Generation)return P::Admission::InvalidGeneration;
        if(!SourceCurrent(key))return P::Admission::SourceUnavailable;
        const auto result=owner->Jobs.QueueCapture(key,leases,token);
        if(auto* e=owner->Jobs.Find(key))e->LastAdmission=result;return result;
    }
    bool ReservationCurrent(const MiniMapCaptureLeases::Token& token)
    {AccessLock lock;auto* e=SourceCurrent(token.Candidate)?owner->Jobs.Find(token.Candidate):nullptr;return e&&P::Queue::MatchesLease(*e,token,leases)&&owner->Jobs.ReservationCurrent(*e);}
    bool StartCapture(const MiniMapCaptureLeases::Token& token)
    {AccessLock lock;auto* e=SourceCurrent(token.Candidate)?owner->Jobs.Find(token.Candidate):nullptr;return e&&P::Queue::MatchesLease(*e,token,leases)&&owner->Jobs.StartCapture(*e);}
    void AbandonCapture(const MiniMapCaptureLeases::Token& token,bool transient) noexcept
    {
        try{AccessLock lock;if(owner)if(auto* e=owner->Jobs.Find(token.Candidate);e&&P::Queue::MatchesLease(*e,token,leases)){
            owner->Jobs.ReleaseCapture(*e,transient);
        }}catch(...){}
    }
    bool ObserveRegistry(std::shared_ptr<const MiniMapSpriteDiscoveryCore::Registry> registry)
    {
        // Declared before the guard: potentially final publication ownership
        // releases only after both owner-access and publication locks are gone.
        RenderGeneration::RegistryPublication displaced;
        bool accepted=false;
        {AccessLock lock;if(owner)accepted=owner->Jobs.ObserveRegistry(registry,displaced);}
        return accepted;
    }
    void Complete(const MiniMapCaptureLeases::Token& token,const MiniMapStaticAssembly::Footprint& bounds,const std::vector<uint8_t>& rgba)
    {
        const auto& key=token.Candidate;
        TimingScope timing(Timing::TextureUpload);
        struct Finish {const MiniMapCaptureLeases::Token& Token;~Finish(){AbandonCapture(Token);}} finish{token};
        uint64_t uploadSequence=0,imageRevision=0;std::shared_ptr<RenderGeneration> target;
        RenderGeneration::ScaleObservation scaleSample;
        // Game-thread completion only; no image work or uploads occur in Draw.
        {
            AccessLock lock;
            const auto* entry=SourceCurrent(key)?owner->Jobs.Find(key):nullptr;
            if(!entry||!entry->InFlight||!P::Queue::PublicationCurrent(*entry,token,leases))return;
            if(owner->UploadSequence==(std::numeric_limits<uint64_t>::max)())
            {++owner->Jobs.PublicationFailures;LOG_WARN("MiniMap: upload serial exhausted; capture image not uploaded");return;}
            uploadSequence=++owner->UploadSequence;target=owner->Generation;imageRevision=entry->ImageRevision;
            scaleSample=target->CurrentScale(key);
        }
        if(!P::HasCoverage(rgba))
        {LOG_WARN("MiniMap: sprite processing rejected: empty or invalid 256x256 capture; building=%u",key.Building);
         RecordForLease(token,P::State::Failed,"empty_or_invalid_sprite_coverage; footprint_preserved");return;}
        const auto start=std::chrono::steady_clock::now();
        // Complete immutable CURRENT membership supplies this exact-key maximum.
        // The scale source is the accepted discovery publication, not a partial
        // scan or a newer raw collector snapshot that has not been adopted.
        const double maxScale=scaleSample.Values.Maximum;
        const bool scaleKnown=scaleSample.Authoritative&&scaleSample.Values.Known;
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
        {{AccessLock lock;if(owner)++owner->UploadFailures;}
         LOG_WARN("MiniMap: sprite publication failed: texture upload unavailable or capacity; building=%u",key.Building);
         RecordForLease(token,P::State::Failed,"texture_upload_unavailable_or_capacity");return;}
        bool preparing=false;
        try
        {
            // Loader names are a deduplication key: the first image wins. Give
            // each upload its own name while preserving the logical sprite key.
            const auto name="MiniMap_Building_session_"+MiniMapSpriteDiscoveryCore::KeyText(key)+
                "_Image_"+std::to_string(uploadSequence);
            auto texture=std::make_shared<Texture>();texture->Free=api->FreeTexture;texture->Bounds=bounds;texture->Region=region;
            preparing=true;auto prepared=target->Prepare(texture);preparing=false;
            std::string reason="private_blueprint_session_sprite; visual_quality_uncertified";
            try {texture->Handle=api->LoadFromRGBA(pixels->data(),int(width),int(height),name.c_str());}catch(...){texture->Handle=nullptr;}
            if(!texture->Handle){AccessLock lock;if(owner&&owner->Generation==target)++owner->UploadFailures;}
            if(!texture->Handle&&useProcessed&&!processed.Passthrough)
            {
                // A failed processed upload may retry the original validated canvas once.
                texture->Region={0,0,1,1};width=height=256;
                LOG_WARN("MiniMap: processed sprite upload failed; original canvas fallback; building=%u",key.Building);
                try {texture->Handle=api->LoadFromRGBA(rgba.data(),256,256,name.c_str());}catch(...){texture->Handle=nullptr;}
                if(!texture->Handle){AccessLock lock;if(owner&&owner->Generation==target)++owner->UploadFailures;}
            }
            texture->RawBytes=size_t(width)*size_t(height)*4;
            bool accepted=false;size_t cached=0,bytes=0,active=0;
            {
                AccessLock lock;
                auto* entry=owner&&owner->Generation==target?target->Find(key):nullptr;
                const bool current=owner&&owner->Generation==target&&SourceCurrent(key)&&entry&&entry->InFlight&&
                    P::Queue::PublicationCurrent(*entry,token,leases)&&target->ScaleCurrent(key,scaleSample);
                if(!texture->Handle)
                {
                    if(current){++owner->Failed;entry->InFlight=false;
                        if(!entry->Cached)entry->Status=P::State::Failed;}
                }
                else if(!current||(entry->Cached&&(entry->Cached->Bounds.Min!=bounds.Min||entry->Cached->Bounds.Max!=bounds.Max)))
                {
                    // Uploaded handles always have a preallocated owned disposition.
                    if(entry)target->RetainUnselected(*entry,prepared);
                }
                else if(target->Commit(*entry,prepared,token.SourceRevision,imageRevision))
                {
                    entry->Reason.swap(reason);
                    if(!entry->ImageRevision)++owner->SuccessfulTypes;
                    entry->InFlight=false;++entry->ImageRevision;
                    entry->Status=P::State::Provisional;
                    ++owner->Completed;accepted=true;
                    cached=target->SelectedCount;bytes=target->SelectedBytes;active=leases.ActiveCount();
                }
                else target->RetainUnselected(*entry,prepared);
            }
            // Backend/UI/formatting and final prepared references are outside publication locks.
            if(accepted)
            {
                int backendWidth=0,backendHeight=0;if(api->GetSize)api->GetSize(texture->Handle,&backendWidth,&backendHeight);
                LOG_INFO("MiniMap: session sprite cache: retained=%zu rawRGBABytes=%zu activeNativeLeases=%zu",cached,bytes,active);
                LOG_INFO("MiniMap: sprite publication draw view: key=%s requested=%zux%zu backend=%dx%d",
                    MiniMapSpriteDiscoveryCore::KeyText(key).c_str(),width,height,backendWidth,backendHeight);
            }
        }
        catch(const std::bad_alloc&)
        {if(!preparing)target->AllocationFailed();
         LOG_WARN("MiniMap: sprite publication allocation failed; building=%u",key.Building);
         try{RecordForLease(token,P::State::Failed,"texture_publication_allocation_failure");}catch(...){} }
        catch(...)
        {LOG_WARN("MiniMap: sprite publication exception; building=%u",key.Building);
         try{RecordForLease(token,P::State::Failed,"texture_publication_exception");}catch(...){} }
    }
    void RecordTiming(Timing kind,double ms) noexcept
    {
        try {AccessLock lock;if(owner&&size_t(kind)<owner->Timings.size())owner->Timings[size_t(kind)].Add(ms);}catch(...){}
    }
    std::string Diagnostics()
    {
        AccessLock lock;
        const auto lifecycle=J::Object({{"requested_renderer_generation",std::to_string(requestedGeneration)},
            {"renderer_reset_result",std::to_string(unsigned(resetResult))},
            {"renderer_generation_allocation_failures",std::to_string(generationAllocationFailures)}});
        if(!owner)return lifecycle;
        const char* names[]={"discovery_service","capture_service","texture_upload","widget_cpu","widget_interval","registry_scan","representation_audit","native_class_resolution","classification","representation_traversal","assembly_recipe","capture_admission","diagnostic_build","diagnostic_publish","readiness_service","readiness_sample"};
        std::string timings="{";
        for(size_t i=0;i<owner->Timings.size();++i)
        {
            const auto& t=owner->Timings[i];if(i)timings+=',';
            timings+=J::Quote(names[i])+":"+J::Object({{"calls",std::to_string(t.Calls)},
                {"last",J::Number(t.Last)},{"min",J::Number(t.Calls?t.Min:0)},{"total",J::Number(t.Total)},{"max",J::Number(t.Max)},{"mean",J::Number(t.Calls?t.Total/double(t.Calls):0)}});
        }
        timings+="}";
        const auto metrics=owner->Jobs.Metrics();
        auto json=P::Json(owner->Jobs,owner->Hits,owner->Fallbacks,MiniMapBuildingCapture::CandidateServiceStatus(),timings,
            leases.ActiveCount(),MiniMapSessionTextures::Memory{owner->Jobs.SelectedCount,owner->Jobs.SelectedBytes},owner->SuccessfulTypes,owner->UploadFailures);
        json.pop_back();
        const char* metricNames[]={"active_frame_pins","active_pinned_revisions","prior_version_nodes_awaiting_reclamation","uploaded_unselected_textures",
            "uploaded_unselected_raw_rgba_bytes","renderer_allocation_failures","renderer_publication_failures","renderer_counter_failures","historical_renderer_slots"};
        for(size_t i=0;i<metrics.size();++i)json+=","+J::Quote(metricNames[i])+":"+std::to_string(metrics[i]);
        json+=",\"renderer_lifecycle\":"+lifecycle+"}";return json;
    }
}
#endif

