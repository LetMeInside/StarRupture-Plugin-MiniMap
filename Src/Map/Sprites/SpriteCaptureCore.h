#pragma once
#include "SpriteDiscoveryCore.h"
#include "SpriteClassificationCore.h"
#include <optional>
#include "CaptureLeaseCore.h"
#include "StaticAssemblyCaptureCore.h"

namespace MiniMapBuildingSpriteCore { enum class Admission; }

namespace MiniMapSpriteCapture
{
    namespace D=MiniMapSpriteDiscoveryCore;
    namespace K=MiniMapSpriteClassification;
    struct Candidate
    {
        D::Key Identity; // Existing session/lifetime token; not a durable blueprint identity.
        std::shared_ptr<const std::string> CanonicalBlueprint;
        K::Policy Classification;
        int32_t ActorObject=-1,ActorName=-1;uint32_t ActorNumber{};
        bool ClassificationObserved=false,RepresentationVerified=false,Truncated=true,General=false;
        // Capture the authored Blueprint in the isolated private world.
        D::Tier Tier=D::Tier::Unknown;
    };
    enum class State { Rejected,NeedsRetention,Eligible };
    // Eligibility is not a receipt for an irreversible native handoff.
    enum class HandoffOutcome { NotOffered,Rejected,Deferred,ObservationOnly,Admitted,Uncertain };
    enum class Cause { None,Policy,SourceNotCurrent,EvidenceIncomplete,ClassificationUnverified,UnsupportedRepresentation,RecipeInvalid,LifecycleUnavailable,NativeOwnerBusy,BackendUnavailable,RetentionUnavailable,ResourcePublicationPending,OperationalException };
    struct Decision
    {
        State Status=State::Rejected;const char* Reason="not_examined";
        Cause Why=Cause::None;
        HandoffOutcome Handoff=HandoffOutcome::NotOffered;
        std::optional<MiniMapBuildingSpriteCore::Admission> Admission;
        std::optional<uint64_t> SourceRevision; // Identity-only session captures use revision zero.
        std::optional<MiniMapCaptureLeases::Token> NativeLease; // Exact accepted receipt when handoff returns normally.
        MiniMapStaticAssembly::RecipeValidation RecipeCheck;
        Decision WithHandoff(HandoffOutcome outcome)const noexcept{auto copy=*this;copy.Handoff=outcome;return copy;}
        Decision RejectedOffer()const noexcept
        {
            auto copy=*this;
            copy.Handoff=Why==Cause::LifecycleUnavailable||Why==Cause::NativeOwnerBusy||Why==Cause::BackendUnavailable||
                Why==Cause::RetentionUnavailable||Why==Cause::ResourcePublicationPending?HandoffOutcome::Deferred:
                Why==Cause::OperationalException?HandoffOutcome::Uncertain:HandoffOutcome::Rejected;
            return copy;
        }
        bool MayRetain()const{return Status!=State::Rejected;}
        bool Allowed()const{return Status==State::Eligible;}
    };
    inline const char* Label(HandoffOutcome o)
    {switch(o){case HandoffOutcome::Rejected:return "rejected";case HandoffOutcome::Deferred:return "deferred";
        case HandoffOutcome::ObservationOnly:return "observation_only";case HandoffOutcome::Admitted:return "admitted";
        case HandoffOutcome::Uncertain:return "uncertain";default:return "not_offered";}}
    inline std::string SessionVisualKey(const Candidate& c)
    {
        return D::KeyText(c.Identity);
    }
}
