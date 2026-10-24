#pragma once
#include "SpriteCaptureCore.h"
#include "StaticAssemblyCaptureCore.h"
#include "CaptureLeaseCore.h"
#include "SessionTextureAccountingCore.h"
#include <map>

namespace MiniMapBuildingSpriteCore
{
    constexpr size_t Capacity=32; // Native resource ceiling; the capture owner remains serialized at one.
    struct KeyHash
    {
        size_t operator()(const MiniMapSpriteDiscoveryCore::Key& k)const noexcept
        {
            uint64_t h=1469598103934665603ull;
            for(uint64_t v:{k.Generation,uint64_t(k.Building),uint64_t(k.DefinitionObject),uint64_t(k.NameIndex),uint64_t(k.NameNumber),
                uint64_t(k.VisualClass),uint64_t(k.MeshObject),uint64_t(k.MeshName),uint64_t(k.MeshNumber),uint64_t(k.Representation),uint64_t(k.Spline)})
            {h^=v;h*=1099511628211ull;}
            return size_t(h);
        }
    };
    inline bool HasCoverage(const std::vector<uint8_t>& rgba,size_t width=256,size_t height=256)
    {if(!width||!height||width>256||height>256||rgba.size()!=width*height*4)return false;
     for(size_t i=3;i<rgba.size();i+=4)if(rgba[i])return true;return false;}
    enum class State { Awaiting,Excluded,Unavailable,Unsupported,Queued,Ready,Failed,Provisional };
    inline const char* Label(State s)
    {switch(s){case State::Excluded:return "excluded";case State::Unavailable:return "temporarily_unavailable";
    case State::Unsupported:return "unsupported";case State::Queued:return "queued_or_capturing";
    case State::Provisional:return "provisional_session_sprite";case State::Ready:return "quality_certified_session_sprite";case State::Failed:return "failed";default:return "awaiting_validation";}}
    enum class Admission { Accepted,AlreadyInFlight,SourceUnavailable,InvalidGeneration,Terminal,NoAdvancement,AdvancementConsumed,StaleAdvancement,SetupRejected,Capacity };
    struct Entry { MiniMapSpriteDiscoveryCore::Key Key;State Status=State::Awaiting;std::string Reason;
        Admission LastAdmission=Admission::NoAdvancement;
        MiniMapCaptureLeases::Token Lease;
        uint64_t ImageRevision{};bool InFlight=false,InitialAdmission=false,AttemptStarted=false; };
    struct Policy
    {
        size_t Captures{},InitialAdmissions{};uint64_t Generation{};
        void Reset(uint64_t generation){Captures=InitialAdmissions=0;Generation=generation;}
        Entry* SetEntry(Entry& entry,State state,const std::string& reason)
        {
            auto* e=&entry;
            if(state==State::Awaiting&&e->Status!=State::Awaiting)return nullptr;
            if(state==State::Queued&&!e->InFlight)return nullptr;
            if((e->Status==State::Ready||e->Status==State::Provisional)&&state!=State::Excluded)return nullptr;
            if(state==State::Failed||state==State::Excluded||state==State::Unsupported)ReleaseCapture(*e);
            e->Reason=reason;e->Status=state;return e;
        }
        Admission Admit(Entry& entry,MiniMapCaptureLeases::Ledger& leases,MiniMapCaptureLeases::Token& token)
        {
            auto* e=&entry;
            if(!e->Key.Generation||e->Key.Generation!=Generation)return Admission::InvalidGeneration;
            if(e->InFlight)return Admission::AlreadyInFlight;
            if(e->InitialAdmission||e->Status==State::Excluded||e->Status==State::Unsupported)return Admission::Terminal;
            MiniMapCaptureLeases::Token acquired;
            // Candidate identity is the complete session source. No resource revision gate.
            if(!leases.Acquire(e->Key,0,acquired))return Admission::Capacity;
            e->InitialAdmission=true;++InitialAdmissions;e->Lease=acquired;token=acquired;
            e->AttemptStarted=false;e->InFlight=true;e->Status=State::Queued;
            return Admission::Accepted;
        }
        bool ReservationCurrent(const Entry& e)const
        {return e.InFlight&&!e.AttemptStarted&&e.Key.Generation==Generation;}
        static bool MatchesLease(const Entry& e,const MiniMapCaptureLeases::Token& token,const MiniMapCaptureLeases::Ledger& leases) noexcept
        {return e.Lease==token&&leases.Holds(token);}
        static bool PublicationCurrent(const Entry& e,const MiniMapCaptureLeases::Token& token,const MiniMapCaptureLeases::Ledger& leases) noexcept
        {return MatchesLease(e,token,leases);}
        bool StartCapture(Entry& e)
        {if(!ReservationCurrent(e))return false;e.AttemptStarted=true;++Captures;return true;}
        void ReleaseCapture(Entry& e,bool=false)
        {e.InFlight=false;if(e.Status==State::Queued)e.Status=State::Failed;}
    };
    // Compatibility storage for portable policy clients. Production renderer uses
    // co-located image/policy slots instead. No distinct-identity capacity ceiling.
    struct KeyLess {bool operator()(const MiniMapSpriteDiscoveryCore::Key& a,const MiniMapSpriteDiscoveryCore::Key& b)const noexcept{return a.Values()<b.Values();}};
    struct Queue : Policy
    {
        std::map<MiniMapSpriteDiscoveryCore::Key,Entry,KeyLess> Entries;
        size_t Count{};
        auto Counts()const{std::array<size_t,8> counts{};for(const auto& [key,e]:Entries)++counts[size_t(e.Status)];return counts;}
        void Reset(uint64_t generation){Entries.clear();Count=0;Policy::Reset(generation);}
        Entry* Find(const MiniMapSpriteDiscoveryCore::Key& key){auto it=Entries.find(key);return it==Entries.end()?nullptr:&it->second;}
        Entry* Observe(const MiniMapSpriteDiscoveryCore::Key& key){if(!key.Generation||key.Generation!=Generation)return nullptr;
            auto [it,added]=Entries.try_emplace(key);if(added){it->second.Key=key;++Count;}return &it->second;}
        Entry* Set(const MiniMapSpriteDiscoveryCore::Key& key,State state,const std::string& reason){auto* e=Observe(key);return e?SetEntry(*e,state,reason):nullptr;}
        Admission QueueCapture(const MiniMapSpriteDiscoveryCore::Key& key,MiniMapCaptureLeases::Ledger& leases,MiniMapCaptureLeases::Token& token)
        {auto* e=Observe(key);return e?Admit(*e,leases,token):Admission::InvalidGeneration;}
    };
    template<class Q> inline std::string Json(const Q& q,size_t hits=0,size_t fallbacks=0,const char* service="not_reported",const std::string& timings="{}",
        size_t activeLeases=0,MiniMapSessionTextures::Memory textureMemory={},size_t successfulTypes=0,size_t uploadFailures=0)
    {
        namespace J=MiniMapRepresentationCore;J::JsonArray rows;rows.Limit=200*1024;
        const auto counts=q.Counts();
        for(const auto& [key,e]:q.Entries){if(rows.Truncated)break;
            rows.Add(J::Object({{"session_candidate",J::Quote(MiniMapSpriteDiscoveryCore::KeyText(e.Key))},
                {"image_revision",std::to_string(e.ImageRevision)},
                {"visual_quality",J::Quote("uncertified")},
                {"last_capture_admission",std::to_string(unsigned(e.LastAdmission))},
                {"capture_in_flight",J::Bool(e.InFlight)},{"building_id",std::to_string(e.Key.Building)},{"state",J::Quote(Label(e.Status))},{"reason",J::Quote(e.Reason)}}));}
        return J::Object({{"schema_version","3"},{"stage",J::Quote("6.8")},{"generation",std::to_string(q.Generation)},
            {"capture_service",J::Quote(service)},
            {"discovered_candidate_keys",std::to_string(q.Count)},{"excluded",std::to_string(counts[1])},
            {"awaiting",std::to_string(counts[0])},{"queued",std::to_string(counts[4])},
            {"unsupported",std::to_string(counts[3])},{"unavailable",std::to_string(counts[2])},
            {"provisional",std::to_string(counts[7])},{"quality_certified",std::to_string(counts[5])},{"completed",std::to_string(counts[5]+counts[7])},{"failed",std::to_string(counts[6])},
            {"session_hits",std::to_string(hits)},{"footprint_fallbacks",std::to_string(fallbacks)},
            {"counter_scope",J::Quote("cumulative within generation; hits and fallbacks count actual draw submissions, not distinct entities")},
            {"timings_ms",timings},
            {"persistent_cache_hits","0"},{"persistent_cache",J::Quote("session cache only")},
            {"visual_equivalence",J::Quote("private Blueprint capture; gameplay visual equivalence not certified")},
            {"live_building_complete","false"},{"capture_limit",std::to_string(Capacity)},
            {"capture_attempts",std::to_string(q.Captures)},
            {"historical_initial_admissions",std::to_string(q.InitialAdmissions)},
            {"active_native_capture_leases",std::to_string(activeLeases)},
            {"retained_texture_count",std::to_string(textureMemory.Count)},
            {"retained_texture_raw_rgba_bytes",std::to_string(textureMemory.RawBytes)},
            {"historical_successfully_captured_types",std::to_string(successfulTypes)},
            {"loader_upload_failures",std::to_string(uploadFailures)},
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
    // A sufficient opacity proof for the overlay's coarse cells. Unknown samples,
    // native interpolation edges and local awareness must all remain drawable.
    template<class Read> bool OpaqueFogCell(int x,int y,bool localFeather,Read read)
    {
        if(localFeather)return false;
        for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx)
        {uint8_t value=0;if(!read(x+dx,y+dy,value)||value!=0)return false;}
        return true;
    }
    template<class Opaque> bool FullyObscured(double minX,double minY,double maxX,double maxY,
        double pivotX,double pivotY,double scale,int logical,size_t limit,Opaque opaque)
    {
        // Reuse the bounded inclusive cell walk; false means opacity was not proven.
        return Revealed(minX,minY,maxX,maxY,pivotX,pivotY,scale,logical,limit,opaque);
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
