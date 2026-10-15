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
        // Supported mode is an authored static reference template, never a live actor.
        D::Tier Tier=D::Tier::Unknown;
    };
    struct Evidence
    {
        bool SourceCurrent=false,SourceValidated=false,RecipeVerified=false,
            LifecycleReady=false,SourcesRetained=false;
        MiniMapStaticAssembly::RecipeValidation RecipeCheck;
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
        std::optional<uint64_t> SourceRevision; // Verified renderer recipe identity, not a publication counter.
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
    inline Decision Decide(const Candidate& c,const Evidence& e,bool explicitUnknownExperiment=false)
    {
        // Policy veto is unconditional, including conflicting/invalid other inputs.
        if(c.Classification.ExcludeFromMiniMap==K::Exclusion::Exclude||c.Classification.Reason=="InteriorTag")return {State::Rejected,"classification_excluded",Cause::Policy};
        if(!c.Identity.Generation||c.Identity.DefinitionObject<0||c.Identity.NameIndex<0||!e.SourceCurrent||!e.SourceValidated)return {State::Rejected,"invalid_or_stale_source",Cause::SourceNotCurrent};
        if(c.Truncated)return {State::Rejected,"truncated_evidence",Cause::EvidenceIncomplete};
        if(!c.ClassificationObserved)return {State::Rejected,"classification_not_verified",Cause::ClassificationUnverified};
        if(!c.RepresentationVerified||c.ActorObject<0||c.ActorName<0||(c.Tier!=D::Tier::High&&!(c.General&&c.Tier==D::Tier::Low)))return {State::Rejected,"unsupported_or_unverified_representation",Cause::UnsupportedRepresentation};
        if(c.Identity.Spline)return {State::Rejected,"unsupported_spline_variant",Cause::UnsupportedRepresentation};
        // Instance tint/customization is not applied by the authored-template path.
        if(c.Identity.TintBits!=D::Key{}.TintBits)return {State::Rejected,"unsupported_customization_variant",Cause::UnsupportedRepresentation};
        if(!e.RecipeVerified){Decision rejected{State::Rejected,"static_recipe_not_verified",Cause::RecipeInvalid};
            rejected.RecipeCheck=e.RecipeCheck;if(const auto* reason=MiniMapStaticAssembly::Label(e.RecipeCheck.Failure))rejected.Reason=reason;return rejected;}
        if(c.Classification.ExcludeFromMiniMap!=K::Exclusion::Include&&!explicitUnknownExperiment)return {State::Rejected,"classification_unknown",Cause::ClassificationUnverified};
        if(!e.LifecycleReady)return {State::Rejected,"capture_lifecycle_unavailable",Cause::LifecycleUnavailable};
        if(!e.SourcesRetained)return {State::NeedsRetention,"source_retention_required",Cause::RetentionUnavailable};
        return {State::Eligible,explicitUnknownExperiment&&c.Classification.ExcludeFromMiniMap==K::Exclusion::Unknown?
            "development_static_template_unknown_classification":"validated_candidate"};
    }
    inline std::string SessionVisualKey(const Candidate& c)
    {
        return D::KeyText(c.Identity)+":actor"+std::to_string(c.ActorObject)+":"+std::to_string(c.ActorName)+":"+
            std::to_string(c.ActorNumber)+":tier"+std::to_string(unsigned(c.Tier))+":static_reference_v1";
    }
}
