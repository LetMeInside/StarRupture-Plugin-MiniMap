#if defined(MODLOADER_CLIENT_BUILD) && defined(MINIMAP_BUILDING_SPRITES)
#include "BuildingSprites.h"
#include "Map/FogOfWar/FogOfWar.h"
#include "SpriteCandidateIdentity.h"
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
    struct DrawState
    {
        std::shared_ptr<const RenderGeneration::Pin> Frame;
        std::shared_ptr<Texture> Resolve(const MiniMapSpriteDiscoveryCore::Key& key)const
        {return Frame?Frame->Owner->Resolve(*Frame,key):nullptr;}
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
            std::shared_ptr<RenderGeneration> Generation;
            RenderGeneration& Jobs;
            explicit Owner(uint64_t world):Generation(std::make_shared<RenderGeneration>(world)),Jobs(*Generation){}

            std::array<TimingStats,size_t(Timing::Count)> Timings{};
            size_t Hits{},Fallbacks{},Completed{},Failed{},SuccessfulTypes{},UploadFailures{};
            uint64_t UploadSequence{};
            std::array<size_t,size_t(DrawCounter::Count)> DrawCounts{};
            size_t DiagnosticFrames{},DiagnosticReports{},SampledHandleChecks{},SampledInvalidHandles{};
            size_t SpatialReports{},SpatialCompleted{};
            uint32_t ReportedDrawReasons{};
        };
        // Explicit lifetime through discovery; no owning static initializer.
        std::unique_ptr<Owner> owner;
        MiniMapSessionPublication::ResetResult resetResult=MiniMapSessionPublication::ResetResult::WorldEnded;
        uint64_t requestedGeneration{};size_t generationAllocationFailures{};
        std::mutex access;
        std::shared_ptr<RenderGeneration> retired;
        RenderGeneration* retiredTail=nullptr;
        // The access lock serializes policy mutation. Draw only reads immutable
        // image versions under Generation::Mutex, never policy Entry fields.
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
        const char* DrawLabel(DrawCounter reason)
        {
            const char* labels[]={"calls","no_ready_view","generation_mismatch","invalid_candidate_inputs",
                "lookup_attempts","lookup_misses","lookup_matches","invalid_instance_transform",
                "quad_validation_failure","projection_failure","viewport_rejection",
                "fog_of_war_rejection","invalid_texture_handle","image_quad_submissions"};
            return size_t(reason)<size_t(DrawCounter::Count)?labels[size_t(reason)]:"none";
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
    DrawFrame BeginDraw()
    {
        AccessLock lock;DrawFrame frame;
        if(owner)
        {
            frame.Generation=owner->Jobs.Generation;frame.ReportedReasons=owner->ReportedDrawReasons;
            try{auto view=std::make_shared<DrawState>();view->Frame=owner->Generation->Begin();if(view->Frame)frame.View=std::move(view);}catch(...){owner->Jobs.AllocationFailed();}
            if(owner->Completed&&owner->SpatialReports<8&&
                (owner->SpatialCompleted!=owner->Completed||owner->DiagnosticFrames==59||owner->DiagnosticFrames==299))
                try{frame.Spatial=std::make_unique<SpatialSnapshot>();}catch(...){} // Diagnostics cannot prevent drawing.
        }
        return frame;
    }
    void EndDraw(const DrawFrame& frame)
    {
        AccessLock lock;if(!owner||owner->Jobs.Generation!=frame.Generation||
            (frame.View&&frame.View->Frame->Owner!=owner->Generation))return;
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
                auto image=frame.View->Resolve(frame.SampleKey);
                auto* self=GetSelf();auto* api=self&&self->hooks?self->hooks->ImGuiTextures:nullptr;
                if(image&&api&&api->GetSize)
                {
                    api->GetSize(image->Handle,&backendWidth,&backendHeight);
                    ++owner->SampledHandleChecks;owner->SampledInvalidHandles+=backendWidth<=0||backendHeight<=0;
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
            LOG_INFO("MiniMap: sprite draw counters: generation=%llu framesAfterPublication=%zu ready=%zu calls=%zu noReadyView=%zu generationMismatch=%zu invalidInputs=%zu lookupAttempts=%zu lookupMisses=%zu lookupMatches=%zu invalidTransforms=%zu quadFailures=%zu projectionFailures=%zu viewportRejections=%zu fogRejections=%zu invalidHandles=%zu submissions=%zu sampledBackendHandleChecks=%zu sampledBackendInvalidHandles=%zu",
                (unsigned long long)frame.Generation,owner->DiagnosticFrames,owner->Jobs.SelectedCount,
                c[0],c[1],c[2],c[3],c[4],c[5],c[6],c[7],c[8],c[9],c[10],c[11],c[12],c[13],owner->SampledHandleChecks,owner->SampledInvalidHandles);
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
            frame.SamplePublishedKey=frame.SampleKey; // Exact queried key, never a history scan.
            if(texture)frame.SampleRegion=texture->Region;
            if(quad){frame.SampleWorld=*quad;frame.SampleGeometry=true;}
            if(screen){for(size_t i=0;i<4;++i)frame.SampleScreen[i]={(*screen)[i].X,(*screen)[i].Y};frame.SampleProjection=true;}
        };
        count(DrawCounter::Calls);
        if(record.Generation!=frame.Generation)
        {count(DrawCounter::GenerationMismatch);sample(DrawCounter::GenerationMismatch);return false;}
        // No native reads or key lookup for an empty immutable view.
        if(!frame.View)
        {count(DrawCounter::NoReadyView);sample(DrawCounter::NoReadyView);return false;}
        const auto copy=MiniMapSpriteCandidates::Copy(record);
        if(!copy.DefinitionValid||!copy.VisualInputsValid)
        {count(DrawCounter::InvalidInputs);sample(DrawCounter::InvalidInputs);return false;}
        count(DrawCounter::LookupAttempts);
        auto texture=frame.View->Resolve(copy.Identity);
        if(!texture)
        {count(DrawCounter::LookupMisses);sample(DrawCounter::LookupMisses);return false;}
        count(DrawCounter::LookupMatches);
        if(!texture||!texture->Handle)
        {count(DrawCounter::InvalidHandle);sample(DrawCounter::InvalidHandle,texture.get());return false;}
        std::array<std::array<double,2>,4> quad{};
        if(!record.TransformValid)
        {count(DrawCounter::InvalidTransform);sample(DrawCounter::InvalidTransform,texture.get());return false;}
        if(!P::Quad(record.Position,record.Rotation,record.Scale,texture->Bounds,quad,texture->Region))
        {count(DrawCounter::QuadFailure);sample(DrawCounter::QuadFailure,texture.get());return false;}
        // Cull only proven opaque coverage; partial/unknown fog stays drawable
        // beneath the overlay, including the local-awareness feather.
        std::array<MiniMapMap::ScreenPoint,4> p;
        for(size_t i=0;i<4;++i)if(!transform.WorldToScreen(quad[i][0],quad[i][1],p[i]))
        {count(DrawCounter::ProjectionFailure);sample(DrawCounter::ProjectionFailure,texture.get(),&quad);return false;}
        float sx=p[0].X,sy=p[0].Y,ex=sx,ey=sy;
        for(const auto& v:p){sx=(std::min)(sx,v.X);sy=(std::min)(sy,v.Y);ex=(std::max)(ex,v.X);ey=(std::max)(ey,v.Y);}
        if(ex<transform.CenterX-transform.Width/2||sx>transform.CenterX+transform.Width/2||
            ey<transform.CenterY-transform.Height/2||sy>transform.CenterY+transform.Height/2)
        {count(DrawCounter::ViewportRejection);sample(DrawCounter::ViewportRejection,texture.get(),&quad,&p);return false;}
        double minX=quad[0][0],minY=quad[0][1],maxX=minX,maxY=minY;
        for(const auto& v:quad){minX=(std::min)(minX,v[0]);minY=(std::min)(minY,v[1]);maxX=(std::max)(maxX,v[0]);maxY=(std::max)(maxY,v[1]);}
        if(MiniMapFogOfWar::IsWorldRectangleFullyObscured(minX,minY,maxX,maxY,transform))
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

