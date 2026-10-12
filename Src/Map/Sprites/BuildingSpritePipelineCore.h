#pragma once
#include "SpriteCaptureCore.h"
#include "StaticAssemblyCaptureCore.h"

namespace MiniMapBuildingSpriteCore
{
    constexpr size_t Capacity=32; // 8 MiB of 256x256 RGBA textures, plus loader overhead.
    struct KeyHash
    {
        size_t operator()(const MiniMapSpriteDiscoveryCore::Key& k)const noexcept
        {
            uint64_t h=1469598103934665603ull;
            for(uint64_t v:{k.Generation,uint64_t(k.Building),uint64_t(k.DefinitionObject),uint64_t(k.NameIndex),uint64_t(k.NameNumber),
                uint64_t(k.VisualClass),uint64_t(k.MeshObject),uint64_t(k.MeshName),uint64_t(k.MeshNumber),uint64_t(k.Representation),uint64_t(k.Spline)})
            {h^=v;h*=1099511628211ull;}
            for(auto v:k.TintBits){h^=v;h*=1099511628211ull;}return size_t(h);
        }
    };
    inline bool HasCoverage(const std::vector<uint8_t>& rgba,size_t width=256,size_t height=256)
    {if(!width||!height||width>256||height>256||rgba.size()!=width*height*4)return false;
     for(size_t i=3;i<rgba.size();i+=4)if(rgba[i])return true;return false;}
    enum class State { Awaiting,Excluded,Unavailable,Unsupported,Queued,Ready,Failed };
    inline const char* Label(State s)
    {switch(s){case State::Excluded:return "excluded";case State::Unavailable:return "temporarily_unavailable";
    case State::Unsupported:return "unsupported";case State::Queued:return "queued_or_capturing";
    case State::Ready:return "ready_session_cache";case State::Failed:return "failed";default:return "awaiting_validation";}}
    struct Entry { MiniMapSpriteDiscoveryCore::Key Key;State Status=State::Awaiting;std::string Reason; };
    struct Queue
    {
        std::array<Entry,MiniMapSpriteDiscoveryCore::Capacity> Entries{};size_t Count{},Captures{};uint64_t Generation{};
        void Reset(uint64_t generation){*this={};Generation=generation;}
        Entry* Find(const MiniMapSpriteDiscoveryCore::Key& key)
        {for(size_t i=0;i<Count;++i)if(Entries[i].Key==key)return &Entries[i];return nullptr;}
        Entry* Observe(const MiniMapSpriteDiscoveryCore::Key& key)
        {if(!key.Generation||key.Generation!=Generation)return nullptr;if(auto* e=Find(key))return e;
        if(Count==Entries.size())return nullptr;auto& e=Entries[Count++];e.Key=key;return &e;}
        Entry* Set(const MiniMapSpriteDiscoveryCore::Key& key,State state,const std::string& reason)
        {auto* e=Observe(key);if(!e)return nullptr;
        if(state==State::Awaiting&&e->Status!=State::Awaiting)return nullptr;
        if(e->Status==State::Ready&&state!=State::Excluded)return nullptr;
        if(e->Status==State::Failed&&state==State::Failed)return nullptr;
        e->Status=state;e->Reason=reason;return e;}
        bool QueueCapture(const MiniMapSpriteDiscoveryCore::Key& key)
        {auto* e=Observe(key);if(!e||e->Status!=State::Awaiting||Captures>=Capacity)return false;
        e->Status=State::Queued;++Captures;return true;}
    };
    inline std::string Json(const Queue& q,size_t hits=0,size_t fallbacks=0,const char* service="not_reported",const std::string& timings="{}")
    {
        namespace J=MiniMapRepresentationCore;J::JsonArray rows;rows.Limit=200*1024;
        std::array<size_t,7> counts{};
        for(size_t i=0;i<q.Count;++i){const auto& e=q.Entries[i];++counts[size_t(e.Status)];
            rows.Add(J::Object({{"session_candidate",J::Quote(MiniMapSpriteDiscoveryCore::KeyText(e.Key))},
                {"building_id",std::to_string(e.Key.Building)},{"state",J::Quote(Label(e.Status))},{"reason",J::Quote(e.Reason)}}));}
        return J::Object({{"schema_version","1"},{"stage",J::Quote("R2.7b")},{"generation",std::to_string(q.Generation)},
            {"capture_service",J::Quote(service)},
            {"discovered_candidate_keys",std::to_string(q.Count)},{"excluded",std::to_string(counts[1])},
            {"awaiting",std::to_string(counts[0])},{"queued",std::to_string(counts[4])},
            {"unsupported",std::to_string(counts[3])},{"unavailable",std::to_string(counts[2])},
            {"completed",std::to_string(counts[5])},{"failed",std::to_string(counts[6])},
            {"session_hits",std::to_string(hits)},{"footprint_fallbacks",std::to_string(fallbacks)},
            {"counter_scope",J::Quote("cumulative within generation; hits and fallbacks count actual draw submissions, not distinct entities")},
            {"timings_ms",timings},
            {"persistent_cache_hits","0"},{"persistent_cache",J::Quote("unsupported; no verified durable recipe identity")},
            {"visual_equivalence",J::Quote("shared authored static reference only; runtime/customization variants unsupported")},
            {"live_building_complete","false"},{"capture_limit",std::to_string(Capacity)},
            {"capture_attempts",std::to_string(q.Captures)},
            {"output_truncated",J::Bool(rows.Truncated)},{"candidates",rows.String()}});
    }
    template<class Read> bool Revealed(double minX,double minY,double maxX,double maxY,
        double pivotX,double pivotY,double scale,int logical,size_t limit,Read read)
    {
        if(!std::isfinite(minX)||!std::isfinite(minY)||!std::isfinite(maxX)||!std::isfinite(maxY)||
            !std::isfinite(scale)||scale<=0||minX>maxX||minY>maxY)return false;
        const double a=std::floor((minX-pivotX)*scale),b=std::floor((minY-pivotY)*scale);
        const double c=std::floor((maxX-pivotX)*scale),d=std::floor((maxY-pivotY)*scale);
        if(!std::isfinite(a)||!std::isfinite(b)||!std::isfinite(c)||!std::isfinite(d)||
            a<0||b<0||c>=logical||d>=logical||(c-a+1)*(d-b+1)>double(limit))return false;
        for(int y=int(b);y<=int(d);++y)for(int x=int(a);x<=int(c);++x)if(!read(x,y))return false;
        return true;
    }
    // PNG camera right=+X, up=-Y, hence image top-left is local (-X,-Y).
    // Only upright, positive-scale instances can reuse this reference projection.
    inline bool Quad(const std::array<double,3>& position,const std::array<double,4>& q,
        const std::array<double,3>& scale,const MiniMapStaticAssembly::Footprint& bounds,
        std::array<std::array<double,2>,4>& out,const std::array<double,4>& region={0,0,1,1})
    {
        for(double v:region)if(!std::isfinite(v))return false;
        if(region[0]<0||region[1]<0||region[2]<=region[0]||region[3]<=region[1])return false;
        if(!bounds.Initialized||!MiniMapAssemblyRecipe::Finite(position)||!MiniMapAssemblyRecipe::Finite(scale))return false;
        double norm=0;for(double v:q){if(!std::isfinite(v))return false;norm+=v*v;}
        if(std::abs(norm-1)>0.001||std::abs(q[0])>1e-6||std::abs(q[1])>1e-6)return false;
        for(double v:scale)if(v<=0||v>1000)return false;
        const double half=MiniMapStaticAssembly::OrthoWidth(bounds)/2;
        const double cx=(bounds.Min[0]+bounds.Max[0])/2,cy=(bounds.Min[1]+bounds.Max[1])/2;
        if(!std::isfinite(half)||half<=0)return false;
        const double c=1-2*q[2]*q[2],s=2*q[3]*q[2];
        for(size_t i=0;i<4;++i){const double u=region[(i==1||i==2)?2:0],v=region[i>=2?3:1];
            const double x=(cx+(2*u-1)*half)*scale[0];
            const double y=(cy+(2*v-1)*half)*scale[1];out[i]={position[0]+c*x-s*y,position[1]+s*x+c*y};
            if(!std::isfinite(out[i][0])||!std::isfinite(out[i][1]))return false;}
        return true;
    }
}
