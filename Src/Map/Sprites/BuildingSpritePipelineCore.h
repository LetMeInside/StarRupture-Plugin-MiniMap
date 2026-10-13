#pragma once
#include "SpriteCaptureCore.h"
#include "RendererReadinessCore.h"
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
    enum class State { Awaiting,Excluded,Unavailable,Unsupported,Queued,Ready,Failed,Provisional };
    inline const char* Label(State s)
    {switch(s){case State::Excluded:return "excluded";case State::Unavailable:return "temporarily_unavailable";
    case State::Unsupported:return "unsupported";case State::Queued:return "queued_or_capturing";
    case State::Provisional:return "provisional_session_sprite";case State::Ready:return "quality_certified_session_sprite";case State::Failed:return "failed";default:return "awaiting_validation";}}
    enum class Admission { Accepted,AlreadyInFlight,SourceUnavailable,InvalidGeneration,Terminal,NoAdvancement,AdvancementConsumed,StaleAdvancement,SetupRejected,Capacity };
    struct Entry { MiniMapSpriteDiscoveryCore::Key Key;State Status=State::Awaiting;std::string Reason;
        MiniMapRendererReadiness::Result Quality,Resources;
        MiniMapRendererReadiness::Dependencies Dependencies;
        Admission LastAdmission=Admission::NoAdvancement;
        uint64_t ImageRevision{};bool InFlight=false,Reserved=false; };
    struct Queue
    {
        std::array<Entry,MiniMapSpriteDiscoveryCore::Capacity> Entries{};size_t Count{},Captures{},Reservations{};uint64_t Generation{};
        void Reset(uint64_t generation){for(auto& e:Entries)e=Entry{};
            Count=Captures=Reservations=0;Generation=generation;} // no multi-megabyte stack temporary
        Entry* Find(const MiniMapSpriteDiscoveryCore::Key& key)
        {for(size_t i=0;i<Count;++i)if(Entries[i].Key==key)return &Entries[i];return nullptr;}
        Entry* Observe(const MiniMapSpriteDiscoveryCore::Key& key)
        {if(!key.Generation||key.Generation!=Generation)return nullptr;if(auto* e=Find(key))return e;
        if(Count==Entries.size())return nullptr;auto& e=Entries[Count++];e.Key=key;return &e;}
        Entry* Set(const MiniMapSpriteDiscoveryCore::Key& key,State state,const std::string& reason)
        {auto* e=Observe(key);if(!e)return nullptr;
        if(state==State::Awaiting&&e->Status!=State::Awaiting)return nullptr;
        if(state==State::Queued&&!e->InFlight)return nullptr; // source evaluation is not capture reservation
        if((e->Status==State::Ready||e->Status==State::Provisional)&&state!=State::Excluded)
        {if(state==State::Failed){ReleaseCapture(*e);e->Reason=reason+"; previous_sprite_retained";}
         if(state==State::Unsupported){e->InFlight=false;e->Dependencies={};e->Resources.State=MiniMapRendererReadiness::RenderReadiness::Unsupported;}
         return nullptr;}
        if(e->Status==State::Failed&&state==State::Failed){ReleaseCapture(*e);return nullptr;}
        if(state==State::Failed||state==State::Excluded)ReleaseCapture(*e);
        e->Reason=reason;e->Status=state;
        if(state==State::Excluded||state==State::Unsupported)e->Dependencies={};return e;}
        // One authority: provider proof, exact recipe generation, unconsumed
        // revision. Source reevaluation alone never grants rendering permission.
        bool EligibleProof(const MiniMapRendererReadiness::Dependencies& d,
            const MiniMapRendererReadiness::Dependencies::Resource& r)const
        {
            return r.AdvancementRevision&&r.AdvancementRevision!=r.Consumed&&r.AdvancementRevision!=r.Rejected&&
                r.AdvancementSource==d.SourceRevision&&r.Advancement.World==Generation&&r.Evidence.World==Generation&&
                r.Relevant&&MiniMapRendererReadiness::SameResource(r.Advancement,r.Evidence)&&
                r.ObjectIndex==r.Advancement.ObjectIndex&&r.NameIndex==r.Advancement.NameIndex&&r.NameNumber==r.Advancement.NameNumber&&
                r.Advancement.ResourceVersion==r.Evidence.ResourceVersion;
        }
        uint64_t EligibleAdvancement(const Entry& e)const
        {
            const auto& d=e.Dependencies;uint64_t revision=0;
            for(size_t i=0;i<d.ResourceCount;++i)if(EligibleProof(d,d.Resources[i]))
                revision=(std::max)(revision,d.Resources[i].AdvancementRevision);
            return revision;
        }
        void CoverCurrentProofs(Entry& e)
        {
            auto& d=e.Dependencies;d.ReservedAdvancement=0;
            for(size_t i=0;i<d.ResourceCount;++i){auto& r=d.Resources[i];
                r.Reserved=EligibleProof(d,r)?r.AdvancementRevision:0;
                d.ReservedAdvancement=(std::max)(d.ReservedAdvancement,r.Reserved);}
        }
        Admission QueueCapture(const MiniMapSpriteDiscoveryCore::Key& key)
        {
            if(!key.Generation||key.Generation!=Generation)return Admission::InvalidGeneration;
            auto* e=Observe(key);if(!e)return Admission::Capacity;
            if(e->InFlight)return Admission::AlreadyInFlight;
            if(e->Status==State::Excluded||e->Status==State::Unsupported)return Admission::Terminal;
            const auto advance=EligibleAdvancement(*e);
            e->Dependencies.UpgradeReservation=e->Reserved;
            if(e->Reserved)
            {
                if(!advance){const auto& d=e->Dependencies;
                    bool rejected=false,consumed=false;
                    for(size_t i=0;i<d.ResourceCount;++i){const auto& r=d.Resources[i];
                        rejected|=r.AdvancementRevision&&r.Rejected==r.AdvancementRevision;
                        consumed|=r.AdvancementRevision&&r.Consumed==r.AdvancementRevision;}
                    if(rejected)return Admission::SetupRejected;if(consumed)return Admission::AdvancementConsumed;
                    return d.AdvancementRevision?Admission::StaleAdvancement:Admission::NoAdvancement;}
            }
            else
            {
                if(Reservations>=Capacity)return Admission::Capacity;
                e->Reserved=true;++Reservations;
            }
            CoverCurrentProofs(*e);
            e->Dependencies.ReservedSource=e->Dependencies.SourceRevision;
            e->Dependencies.AttemptStarted=false;e->InFlight=true;
            if(!e->ImageRevision)e->Status=State::Queued;
            return Admission::Accepted;
        }
        bool ReservationCurrent(const Entry& e)const
        {
            const auto& d=e.Dependencies;
            if(!e.InFlight||d.AttemptStarted||d.ReservedSource!=d.SourceRevision)return false;
            // A later valid advancement in the same source generation can
            // supersede the reserved proof before dispatch. A stale proof alone cannot.
            if(d.UpgradeReservation&&!EligibleAdvancement(e))return false;
            return true;
        }
        bool StartCapture(Entry& e)
        {
            if(!ReservationCurrent(e))return false;
            auto& d=e.Dependencies;
            // Reconcile the exact current proof set immediately before dispatch.
            // Temporarily irrelevant proofs are uncovered and remain outstanding.
            CoverCurrentProofs(e);
            for(size_t i=0;i<d.ResourceCount;++i){auto& r=d.Resources[i];if(r.Reserved)r.Consumed=r.Reserved;}
            d.AttemptStarted=true;d.ConsumedAdvancement=(std::max)(d.ConsumedAdvancement,d.ReservedAdvancement); // diagnostic maximum
            ++Captures;return true;
        }
        void ReleaseCapture(Entry& e,bool transient=false)
        {
            auto& d=e.Dependencies;
            // Definitive setup failure suppresses this revision without claiming
            // a GPU attempt. Temporary owner/source contention does neither.
            for(size_t i=0;i<d.ResourceCount;++i){auto& r=d.Resources[i];
                if(e.InFlight&&!d.AttemptStarted&&!transient&&d.ReservedSource==d.SourceRevision&&r.Reserved)
                {r.Rejected=r.Reserved;d.RejectedAdvancement=(std::max)(d.RejectedAdvancement,r.Reserved);} // diagnostic maximum
                r.Reserved=0;}
            e.InFlight=false;d.ReservedAdvancement=d.ReservedSource=0;d.AttemptStarted=d.UpgradeReservation=false;
            if(e.Status==State::Queued)e.Status=State::Failed;
        }
    };
    inline std::string Json(const Queue& q,size_t hits=0,size_t fallbacks=0,const char* service="not_reported",const std::string& timings="{}")
    {
        namespace J=MiniMapRepresentationCore;J::JsonArray rows;rows.Limit=200*1024;
        std::array<size_t,8> counts{};
        for(size_t i=0;i<q.Count;++i){const auto& e=q.Entries[i];++counts[size_t(e.Status)];
            rows.Add(J::Object({{"session_candidate",J::Quote(MiniMapSpriteDiscoveryCore::KeyText(e.Key))},
                {"image_revision",std::to_string(e.ImageRevision)},
                {"image_quality_readiness",J::Quote(MiniMapRendererReadiness::Label(e.Quality.State))},
                {"current_resource_readiness",J::Quote(MiniMapRendererReadiness::Label(e.Resources.State))},{"quality_unknown_mask",std::to_string(e.Quality.Unknown)},
                {"quality_pending_mask",std::to_string(e.Quality.Pending)},{"quality_unsupported_mask",std::to_string(e.Quality.Unsupported)},
                {"current_pending_mask",std::to_string(e.Resources.Pending)},{"current_unknown_mask",std::to_string(e.Resources.Unknown)},
                {"completion_wake_mask",std::to_string(e.Resources.Wakes)},
                {"dependency_tracking",J::Bool(e.Dependencies.Tracked)},{"missing_prerequisites",std::to_string(e.Dependencies.Missing)},
                {"dependency_revision",std::to_string(e.Dependencies.ObservationRevision)},
                {"resource_advancement_revision",std::to_string(e.Dependencies.AdvancementRevision)},
                {"consumed_advancement_max_diagnostic",std::to_string(e.Dependencies.ConsumedAdvancement)},
                {"reserved_advancement_revision",std::to_string(e.Dependencies.ReservedAdvancement)},
                {"setup_rejected_advancement_max_diagnostic",std::to_string(e.Dependencies.RejectedAdvancement)},
                {"source_revision",std::to_string(e.Dependencies.SourceRevision)},
                {"observable_demand",J::Bool(e.Dependencies.Demand)},
                {"watch_state",std::to_string(unsigned(e.Dependencies.ObservationWatch))},
                {"source_evaluation_queued",J::Bool(e.Dependencies.EvaluationQueued)},
                {"last_capture_admission",std::to_string(unsigned(e.LastAdmission))},
                {"reevaluation_requested",J::Bool(e.Dependencies.Reevaluate)},
                {"capture_in_flight",J::Bool(e.InFlight)},{"building_id",std::to_string(e.Key.Building)},{"state",J::Quote(Label(e.Status))},{"reason",J::Quote(e.Reason)}}));}
        J::JsonArray conditions;
        for(size_t n=0;n<size_t(MiniMapRendererReadiness::Condition::Count);++n)
            conditions.Add(J::Object({{"bit",std::to_string(n)},
                {"condition",J::Quote(MiniMapRendererReadiness::Label(MiniMapRendererReadiness::Condition(n)))}}));
        return J::Object({{"schema_version","2"},{"stage",J::Quote("A.4.9d")},{"generation",std::to_string(q.Generation)},
            {"readiness_conditions",conditions.String()},
            {"capture_service",J::Quote(service)},
            {"discovered_candidate_keys",std::to_string(q.Count)},{"excluded",std::to_string(counts[1])},
            {"awaiting",std::to_string(counts[0])},{"queued",std::to_string(counts[4])},
            {"unsupported",std::to_string(counts[3])},{"unavailable",std::to_string(counts[2])},
            {"provisional",std::to_string(counts[7])},{"quality_certified",std::to_string(counts[5])},{"completed",std::to_string(counts[5]+counts[7])},{"failed",std::to_string(counts[6])},
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
