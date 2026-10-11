#if defined(MODLOADER_CLIENT_BUILD) && defined(MINIMAP_BUILDING_SPRITES)
#include "BuildingSprites.h"
#include "FogOfWar.h"
#include "../Experiments/SpriteCandidateIdentity.h"
#include "../Experiments/SmelterCaptureExperiment.h"
#include "../plugin_helpers.h"
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
        };
        // Explicit lifetime through discovery; no owning static initializer.
        Owner* owner=nullptr;
        std::mutex access;
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
        {owner->View.reset();owner->Textures={};
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
        // Game-thread only. Public v70 upload makes one bounded blocking copy;
        // never performed by Draw, and never while holding the render-facing lock.
        auto* self=GetSelf();auto* api=self&&self->hooks?self->hooks->ImGuiTextures:nullptr;
        if(!P::HasCoverage(rgba)){Record(key,P::State::Failed,"empty_or_invalid_sprite_coverage; footprint_preserved");return;}
        if(!api||!api->LoadFromRGBA||!api->FreeTexture||!api->GetFreeSlotCount||!api->GetFreeSlotCount()||rgba.size()!=256*256*4)
        {Record(key,P::State::Failed,"texture_upload_unavailable_or_capacity");return;}
        const auto name="MiniMap_Building_R27_session_"+MiniMapSpriteDiscoveryCore::KeyText(key);
        auto texture=std::make_shared<Texture>();texture->Free=api->FreeTexture;texture->Bounds=bounds;
        try {texture->Handle=api->LoadFromRGBA(rgba.data(),256,256,name.c_str());}catch(...){texture->Handle=nullptr;}
        std::scoped_lock lock(access);if(!owner)return;auto* e=owner->Jobs.Find(key);
        if(!e||e->Status!=P::State::Queued)return;
        if(!texture->Handle){e->Status=P::State::Failed;e->Reason="texture_upload_failed";++owner->Failed;return;}
        owner->Textures[size_t(e-owner->Jobs.Entries.data())]=std::move(texture);
        e->Status=P::State::Ready;e->Reason+="; static_reference_only; live_equivalence_unverified";++owner->Completed;
        PublishReadyView();
        LOG_INFO("MiniMap: general building sprite ready: building=%u; session texture published; static reference only",key.Building);
    }
    DrawFrame BeginDraw()
    {
        std::scoped_lock lock(access);DrawFrame frame;
        if(owner){frame.Generation=owner->Jobs.Generation;frame.View=owner->View;}return frame;
    }
    void EndDraw(const DrawFrame& frame)
    {
        std::scoped_lock lock(access);if(!owner||owner->Jobs.Generation!=frame.Generation)return;
        owner->Hits+=frame.Hits;owner->Fallbacks+=frame.Fallbacks;
    }
    bool Draw(DrawFrame& frame,IModLoaderImGui* ui,PluginDrawList dl,const MiniMapMap::Transform& transform,const MiniMapBuildingCollector::Record& record)
    {
        // Excluded/unavailable/unsupported candidates never enter this immutable
        // view. With no ready sprites, no per-instance key construction or locking.
        if(!frame.View||frame.View->Ready.empty()||record.Generation!=frame.Generation)return false;
        const auto copy=MiniMapSpriteCandidates::Copy(record);
        if(!copy.DefinitionValid||!copy.VisualInputsValid)return false;
        const auto found=frame.View->Ready.find(copy.Identity);if(found==frame.View->Ready.end())return false;
        const auto& texture=found->second; // frame owns the texture through draw submission
        std::array<std::array<double,2>,4> quad{};
        if(!record.TransformValid||!P::Quad(record.Position,record.Rotation,record.Scale,texture->Bounds,quad))return false;
        // Require every FOW cell intersecting the entire sprite AABB revealed.
        // Fails closed at the mask-read budget; no geometry leaks through holes.
        std::array<MiniMapMap::ScreenPoint,4> p;
        for(size_t i=0;i<4;++i)if(!transform.WorldToScreen(quad[i][0],quad[i][1],p[i]))return false;
        float sx=p[0].X,sy=p[0].Y,ex=sx,ey=sy;
        for(const auto& v:p){sx=(std::min)(sx,v.X);sy=(std::min)(sy,v.Y);ex=(std::max)(ex,v.X);ey=(std::max)(ey,v.Y);}
        if(ex<transform.CenterX-transform.Width/2||sx>transform.CenterX+transform.Width/2||
            ey<transform.CenterY-transform.Height/2||sy>transform.CenterY+transform.Height/2)return false;
        if(!frame.Budget)return false;--frame.Budget;
        double minX=quad[0][0],minY=quad[0][1],maxX=minX,maxY=minY;
        for(const auto& v:quad){minX=(std::min)(minX,v[0]);minY=(std::min)(minY,v[1]);maxX=(std::max)(maxX,v[0]);maxY=(std::max)(maxY,v[1]);}
        if(!MiniMapFogOfWar::IsNativeWorldRectangleRevealed(minX,minY,maxX,maxY))return false;
        ui->DL_AddImageQuad(dl,texture->Handle,p[0].X,p[0].Y,p[1].X,p[1].Y,p[2].X,p[2].Y,p[3].X,p[3].Y,0,0,1,0,1,1,0,1,0xffffffff);
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

