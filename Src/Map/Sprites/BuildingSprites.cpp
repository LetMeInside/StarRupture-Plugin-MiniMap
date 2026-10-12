#if defined(MODLOADER_CLIENT_BUILD) && defined(MINIMAP_BUILDING_SPRITES)
#include "BuildingSprites.h"
#include "Map/FogOfWar/FogOfWar.h"
#include "SpriteCandidateIdentity.h"
#include "BuildingSpriteCapture.h"
#include "CapturePng.h"
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
            std::array<size_t,size_t(DrawCounter::Count)> DrawCounts{};
            size_t DiagnosticFrames{},DiagnosticReports{},SampledHandleChecks{},SampledInvalidHandles{};
            uint32_t ReportedDrawReasons{};
        };
        // Explicit lifetime through discovery; no owning static initializer.
        Owner* owner=nullptr;
        std::mutex access;
        const char* DrawLabel(DrawCounter reason)
        {
            const char* labels[]={"calls","no_ready_view","generation_mismatch","invalid_candidate_inputs",
                "lookup_attempts","lookup_misses","lookup_matches","invalid_instance_transform",
                "quad_validation_failure","projection_failure","viewport_rejection","budget_rejection",
                "fog_of_war_rejection","invalid_texture_handle","image_quad_submissions"};
            return size_t(reason)<size_t(DrawCounter::Count)?labels[size_t(reason)]:"none";
        }
        void PublishReadyView() // access lock held; only on ready/excluded transitions
        {
            auto view=std::make_shared<DrawState>();view->Ready.reserve(P::Capacity);
            for(size_t i=0;i<owner->Jobs.Count;++i)
                if(owner->Jobs.Entries[i].Status==P::State::Ready&&owner->Textures[i])
                    view->Ready.emplace(owner->Jobs.Entries[i].Key,owner->Textures[i]);
            owner->View=std::move(view);
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
         owner->SampledHandleChecks=owner->SampledInvalidHandles=0;owner->ReportedDrawReasons=0;
         owner->Timings={};owner->Jobs.Reset(generation);owner->Hits=owner->Fallbacks=owner->Completed=owner->Failed=0;}
        if(!generation){delete owner;owner=nullptr;}
    }
    void Record(const MiniMapSpriteDiscoveryCore::Key& key,P::State state,const std::string& reason)
    {
        std::scoped_lock lock(access);if(!owner)return;
        auto* e=owner->Jobs.Set(key,state,reason);if(!e)return;
        if(state==P::State::Excluded&&owner->Textures[size_t(e-owner->Jobs.Entries.data())])
        {owner->Textures[size_t(e-owner->Jobs.Entries.data())].reset();PublishReadyView();}
    }
    bool Queue(const MiniMapSpriteDiscoveryCore::Key& key)
    {std::scoped_lock lock(access);return owner&&owner->Jobs.QueueCapture(key);}
    void Complete(const MiniMapSpriteDiscoveryCore::Key& key,const MiniMapStaticAssembly::Footprint& bounds,const std::vector<uint8_t>& rgba)
    {
        TimingScope timing(Timing::TextureUpload);
        // Game-thread completion only; no image work or uploads occur in Draw.
        {
            std::scoped_lock lock(access);
            const auto* entry=owner?owner->Jobs.Find(key):nullptr;
            if(!entry||entry->Status!=P::State::Queued)return;
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
            const auto name="MiniMap_Building_R27_session_"+MiniMapSpriteDiscoveryCore::KeyText(key);
            auto texture=std::make_shared<Texture>();texture->Free=api->FreeTexture;texture->Bounds=bounds;texture->Region=region;
            try {texture->Handle=api->LoadFromRGBA(pixels->data(),int(width),int(height),name.c_str());}catch(...){texture->Handle=nullptr;}
            if(!texture->Handle&&useProcessed&&!processed.Passthrough)
            {
                // A failed processed upload may retry the original validated canvas once.
                texture->Region={0,0,1,1};width=height=256;
                LOG_WARN("MiniMap: processed sprite upload failed; original canvas fallback; building=%u",key.Building);
                try {texture->Handle=api->LoadFromRGBA(rgba.data(),256,256,name.c_str());}catch(...){texture->Handle=nullptr;}
            }
            std::scoped_lock lock(access);if(!owner)return;auto* entry=owner->Jobs.Find(key);
            if(!entry||entry->Status!=P::State::Queued)return;
            if(!texture->Handle)
            {entry->Status=P::State::Failed;entry->Reason="texture_upload_failed";++owner->Failed;
             LOG_WARN("MiniMap: sprite publication failed: upload; building=%u",key.Building);return;}
            // Construct the complete immutable view before committing either pixels or metadata.
            auto view=std::make_shared<DrawState>();view->Ready.reserve(P::Capacity);
            for(size_t i=0;i<owner->Jobs.Count;++i)
                if(owner->Jobs.Entries[i].Status==P::State::Ready&&owner->Textures[i])
                    view->Ready.emplace(owner->Jobs.Entries[i].Key,owner->Textures[i]);
            view->Ready.emplace(key,texture);
            entry->Reason+="; static_reference_only; live_equivalence_unverified";
            owner->Textures[size_t(entry-owner->Jobs.Entries.data())]=std::move(texture);
            entry->Status=P::State::Ready;++owner->Completed;owner->View=std::move(view);
            int backendWidth=0,backendHeight=0;
            if(api->GetSize)api->GetSize(owner->Textures[size_t(entry-owner->Jobs.Entries.data())]->Handle,&backendWidth,&backendHeight);
            LOG_INFO("MiniMap: general building sprite ready: building=%u; session texture published %zux%zu; static reference only",key.Building,width,height);
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
        if(owner){frame.Generation=owner->Jobs.Generation;frame.View=owner->View;frame.ReportedReasons=owner->ReportedDrawReasons;}return frame;
    }
    void EndDraw(const DrawFrame& frame)
    {
        std::scoped_lock lock(access);if(!owner||owner->Jobs.Generation!=frame.Generation)return;
        owner->Hits+=frame.Hits;owner->Fallbacks+=frame.Fallbacks;
        for(size_t i=0;i<frame.Counts.size();++i)owner->DrawCounts[i]+=frame.Counts[i];
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
        const char* names[]={"discovery_service","capture_service","texture_upload","widget_cpu","widget_interval","registry_scan","representation_audit","native_class_resolution","classification","representation_traversal","assembly_recipe","capture_admission","diagnostic_build","diagnostic_publish"};
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

