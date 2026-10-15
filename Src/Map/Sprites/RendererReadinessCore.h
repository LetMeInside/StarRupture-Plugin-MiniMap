#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace MiniMapRendererReadiness
{
    enum class RenderReadiness { Ready, Pending, Unknown, Unsupported };
    enum class Route { Unknown, Conventional, Nanite, Other };
    enum class Condition : uint8_t
    {
        Representation, RouteSelection, CookedDetail, Initialization, Residency,
        EffectiveDetail, Materials, Shaders, PipelineState, Registration,
        ProxyGeneration, SceneIntegration, CaptureView, Count
    };
    enum class Proof : uint8_t { Unknown, Satisfied, Pending, Unsupported };
    enum class Wake : uint8_t { None, StreamingState, RenderState, ResourceChanged };
    struct Stamp
    {
        uint64_t World{},Resource{},RenderState{},View{};
        bool operator==(const Stamp&) const = default;
        bool Valid() const {return World&&Resource&&RenderState&&View;}
    };
    struct Component
    {
        Stamp Generation;
        uint64_t Identity{}; // CPU identity token, never a borrowed proxy pointer.
        Route Selected=Route::Unknown;
        std::array<Proof,size_t(Condition::Count)> Conditions{};
        std::array<Wake,size_t(Condition::Count)> Wakes{};
    };
    struct Result
    {
        RenderReadiness State=RenderReadiness::Unknown;
        uint32_t Unknown{},Pending{},Unsupported{};
        uint8_t Wakes{};
        size_t Components{};
        Stamp Generation;
        Stamp CapturedGeneration; // populated only by a verified capture-view observer
    };
    constexpr uint32_t Bit(Condition c){return uint32_t(1)<<unsigned(c);}
    inline const char* Label(RenderReadiness state)
    {
        switch(state){case RenderReadiness::Ready:return "Ready";case RenderReadiness::Pending:return "Pending";
        case RenderReadiness::Unsupported:return "Unsupported";default:return "Unknown";}
    }
    inline const char* Label(Condition c)
    {
        constexpr const char* labels[]={"representation","selected_rendering_route","cooked_intended_detail",
            "resource_initialization","required_residency","effective_detail_selection","intended_section_materials",
            "intended_shader_resources","required_capture_PSO","component_registration","proxy_generation",
            "scene_integration","capture_view"};
        return size_t(c)<size_t(Condition::Count)?labels[size_t(c)]:"invalid_condition";
    }
    inline Result Evaluate(const Component* components,size_t count,const Stamp& current)
    {
        Result r;r.Generation=current;r.Components=count;
        if(!components||!count||count>16||!current.World)return r;
        if(!current.Valid())r.Unknown|=Bit(Condition::ProxyGeneration)|Bit(Condition::CaptureView);
        for(size_t i=0;i<count;++i)
        {
            const auto& c=components[i];
            if(!c.Identity||c.Generation!=current)r.Unknown|=Bit(Condition::ProxyGeneration);
            if(c.Selected==Route::Unknown)r.Unknown|=Bit(Condition::RouteSelection);
            if(c.Selected==Route::Nanite||c.Selected==Route::Other)r.Unsupported|=Bit(Condition::RouteSelection);
            for(size_t n=0;n<c.Conditions.size();++n)
                switch(c.Conditions[n])
                {
                case Proof::Unknown:r.Unknown|=uint32_t(1)<<n;break;
                case Proof::Unsupported:r.Unsupported|=uint32_t(1)<<n;break;
                case Proof::Pending:
                    r.Pending|=uint32_t(1)<<n;
                    // A missing wake contract is an engineering gap, not a timer.
                    if(c.Wakes[n]==Wake::None)r.Unknown|=uint32_t(1)<<n;
                    else r.Wakes|=uint8_t(1)<<unsigned(c.Wakes[n]);
                    break;
                default:break;
                }
        }
        r.State=r.Unsupported?RenderReadiness::Unsupported:r.Unknown?RenderReadiness::Unknown:
            r.Pending?RenderReadiness::Pending:RenderReadiness::Ready;
        return r;
    }
    inline void PreserveVisualUncertainty(Result& result,bool incomplete)noexcept
    {
        if(!incomplete)return;
        result.Unknown|=Bit(Condition::Representation);
        if(!result.Unsupported)result.State=RenderReadiness::Unknown;
    }
    // Preliminary GT cache observation. No RHI or actual selected-route proof.
    struct MeshState
    {
        bool Observed{},HasRenderData{},Initialized{},ReadyForStreaming{},PendingUpdate{},NeverStream{};
        std::array<uint8_t,8> Cache{};
        uint64_t ResourceToken{}; // opaque comparison value, never a borrowed pointer
    };
    enum class NotificationAck { Recorded,AlreadyRecorded,Invalidated,Rejected,Observed };
    enum class ObservationMethod : uint8_t { Unavailable, BoundedPolling, Notification };
    struct ProviderObservation
    {
        uint32_t Provider{},Conditions{},Pending{},Unknown{},Completed{},Unsupported{};
        uint64_t World{},ResourceIdentity{},ResourceVersion{},ObservationRevision{};
        int32_t ObjectIndex=-1,NameIndex=-1;uint32_t NameNumber{}; // prepared SDK identity, no native pointer
        ObservationMethod Method=ObservationMethod::Unavailable;
        bool CanChange=false,RequiresEvaluation=false,RecaptureEligible=false;
        MeshState Mesh; // conventional provider payload; scheduler never interprets it
        RenderReadiness Disposition()const
        {return Unsupported?RenderReadiness::Unsupported:Unknown?RenderReadiness::Unknown:
            Pending?RenderReadiness::Pending:Conditions&&Completed==Conditions?RenderReadiness::Ready:RenderReadiness::Unknown;}
    };
    inline bool SameResource(const ProviderObservation& a,const ProviderObservation& b)
    {
        // Hash (including zero) is only an additional consistency check. Exact
        // SDK identity, provider and world establish which resource is compared.
        return a.ObjectIndex>=0&&a.NameIndex>=0&&b.ObjectIndex>=0&&b.NameIndex>=0&&
            a.Provider==b.Provider&&a.World==b.World&&a.ObjectIndex==b.ObjectIndex&&
            a.NameIndex==b.NameIndex&&a.NameNumber==b.NameNumber&&a.ResourceIdentity==b.ResourceIdentity;
    }
    // Persistent CPU-only dependency metadata. Native tickets are optional and
    // may retire while this record continues to describe an unresolved sprite.
    struct Dependencies
    {
        struct Resource {ProviderObservation Evidence,Advancement;uint64_t AdvancementRevision{},AdvancementSource{},Consumed{},Rejected{},Reserved{};bool Relevant=false;int32_t ObjectIndex=-1,NameIndex=-1;uint32_t NameNumber{};uint16_t Contributors{};uint64_t LastNotificationRevision{};};
        std::array<Resource,16> Resources{};size_t ResourceCount{};
        uint32_t Required=(uint32_t(1)<<unsigned(Condition::Count))-1,Missing=Required;
        // Definition-level acknowledgment maxima below are diagnostics only.
        uint64_t ProviderEpoch{},ObservationRevision{},AdvancementRevision{},ConsumedAdvancement{};
        // Exact CPU-owned recipe signature, not native pointers or a durable cache key.
        std::vector<uint64_t> Source;
        uint64_t SourceRevision{},ReservedAdvancement{},ReservedSource{},RejectedAdvancement{};
        uint64_t EvaluationRevision{},BlockedObservation{},ChangeRevision{};double BlockedPublication=-1;
        enum class Watch { None,Waiting,Active,Retiring };
        Watch ObservationWatch=Watch::None;
        bool Demand=false,EvaluationQueued=false,AttemptStarted=false,UpgradeReservation=false;
        bool Tracked=false,Reevaluate=false;
    };
    struct Streaming
    {
        Proof State=Proof::Unknown;
        int FirstResident=-1,FirstCachedLOD=-1;
        bool NonStreaming{},InitHint{},TransitionHint{};
    };
    inline Streaming InspectStreaming(const MeshState& s)
    {
        Streaming r;
        if(!s.Observed||!s.HasRenderData)return r;
        if(s.PendingUpdate||(s.Initialized&&!s.ReadyForStreaming))r.State=Proof::Pending;
        const auto& b=s.Cache;
        const unsigned nonstream=b[1],nonoptional=b[2],max=b[3],resident=b[5],requested=b[6];
        r.InitHint=(b[0]&4)!=0;r.TransitionHint=(b[0]&8)!=0;
        if(!max||!nonstream||nonstream>nonoptional||nonoptional>max||resident>max||requested>max||
            resident<nonstream||requested<nonstream||unsigned(b[4])+max>255)return r;
        r.FirstResident=int(b[4])+int(max)-int(resident);r.FirstCachedLOD=b[4];
        r.NonStreaming=(b[0]&1)==0;
        if(s.PendingUpdate||(s.Initialized&&!s.ReadyForStreaming))r.State=Proof::Pending;
        // Hints may be stale and are normally cleared by the engine's mutating
        // HasPendingInitOrStreaming. Equality can also follow cancellation.
        else if(s.Initialized&&s.ReadyForStreaming&&!r.InitHint&&!r.TransitionHint&&
            requested==resident&&resident==max)r.State=Proof::Satisfied;
        // Satisfied only describes this cache. It does not prove usable buffers,
        // optional cooked data, MinLOD/quality policy, shaders or selected LOD.
        return r;
    }
    enum class Stage { Discovered,AwaitingResources,AwaitingRenderState,ReadyForCapture,Unknown,Unsupported,Cancelled };
    struct Ticket
    {
        Stage Current=Stage::Discovered;Result Evidence;uint64_t Revision{},World{};
        bool Apply(const Result& r,uint64_t revision)
        {
            if(Current==Stage::Cancelled||!revision||revision<=Revision)return false;
            if(World&&r.Generation.World!=World)return false;
            World=r.Generation.World;Revision=revision;Evidence=r;
            Current=r.State==RenderReadiness::Ready?Stage::ReadyForCapture:
                r.State==RenderReadiness::Unknown?Stage::Unknown:
                r.State==RenderReadiness::Unsupported?Stage::Unsupported:
                (r.Wakes&(uint8_t(1)<<unsigned(Wake::StreamingState)))?Stage::AwaitingResources:Stage::AwaitingRenderState;
            return true;
        }
        bool ShouldObserve(Wake event)const
        {
            return (Current==Stage::AwaitingResources||Current==Stage::AwaitingRenderState||
                (Current==Stage::Unknown&&Evidence.Pending))&&
                event!=Wake::None&&(Evidence.Wakes&(uint8_t(1)<<unsigned(event)));
        }
        void Cancel(){Evidence={};++Revision;Current=Stage::Cancelled;}
    };
    inline bool Certifiable(const Result& r,uint64_t world)
    {
        return r.State==RenderReadiness::Ready&&r.Components&&r.Components<=16&&
            !r.Unknown&&!r.Pending&&!r.Unsupported&&r.Generation.Valid()&&r.Generation.World==world&&
            r.CapturedGeneration==r.Generation;
    }
    // An upgrade needs an observed change in engine resources, never image
    // comparison or time. Cancellation with unchanged residency is not progress.
    inline bool Improved(const MeshState& before,const MeshState& after)
    {
        const auto a=InspectStreaming(before),b=InspectStreaming(after);
        if(!before.Observed||!after.Observed||!after.HasRenderData)return false;
        if(after.PendingUpdate||before.ResourceToken!=after.ResourceToken)return false;
        // Changed bias/cooked range is a new detail domain, not proof of a
        // residency improvement relative to the previous request.
        for(size_t i=1;i<=4;++i)if(before.Cache[i]!=after.Cache[i])return false;
        if(b.FirstResident<0)return false;
        return (!before.ReadyForStreaming&&after.Initialized&&after.ReadyForStreaming)||
            (a.FirstResident>=0&&b.FirstResident>=0&&b.FirstResident<a.FirstResident);
    }
}
