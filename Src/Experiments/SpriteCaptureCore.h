#pragma once
#include "SpriteDiscoveryCore.h"
#include "SpriteClassificationCore.h"

namespace MiniMapSpriteCapture
{
    namespace D=MiniMapSpriteDiscoveryCore;
    namespace K=MiniMapSpriteClassification;
    struct Candidate
    {
        D::Key Identity;
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
    };
    enum class State { Rejected,NeedsRetention,Eligible };
    struct Decision
    {
        State Status=State::Rejected;const char* Reason="not_examined";
        bool MayRetain()const{return Status!=State::Rejected;}
        bool Allowed()const{return Status==State::Eligible;}
    };
    inline Decision Decide(const Candidate& c,const Evidence& e,bool explicitUnknownExperiment=false)
    {
        // Policy veto is unconditional, including conflicting/invalid other inputs.
        if(c.Classification.ExcludeFromMiniMap==K::Exclusion::Exclude||c.Classification.Reason=="InteriorTag")return {State::Rejected,"classification_excluded"};
        if(!c.Identity.Generation||c.Identity.DefinitionObject<0||c.Identity.NameIndex<0||!e.SourceCurrent||!e.SourceValidated)return {State::Rejected,"invalid_or_stale_source"};
        if(c.Truncated)return {State::Rejected,"truncated_evidence"};
        if(!c.ClassificationObserved)return {State::Rejected,"classification_not_verified"};
        if(!c.RepresentationVerified||c.ActorObject<0||c.ActorName<0||(c.Tier!=D::Tier::High&&!(c.General&&c.Tier==D::Tier::Low)))return {State::Rejected,"unsupported_or_unverified_representation"};
        if(c.Identity.Spline)return {State::Rejected,"unsupported_spline_variant"};
        // Instance tint/customization is not applied by the authored-template path.
        if(c.Identity.TintBits!=D::Key{}.TintBits)return {State::Rejected,"unsupported_customization_variant"};
        if(!e.RecipeVerified)return {State::Rejected,"static_recipe_not_verified"};
        if(c.Classification.ExcludeFromMiniMap!=K::Exclusion::Include&&!explicitUnknownExperiment)return {State::Rejected,"classification_unknown"};
        if(!e.LifecycleReady)return {State::Rejected,"capture_lifecycle_unavailable"};
        if(!e.SourcesRetained)return {State::NeedsRetention,"source_retention_required"};
        return {State::Eligible,explicitUnknownExperiment&&c.Classification.ExcludeFromMiniMap==K::Exclusion::Unknown?
            "development_static_template_unknown_classification":"validated_candidate"};
    }
    inline std::string SessionVisualKey(const Candidate& c)
    {
        return D::KeyText(c.Identity)+":actor"+std::to_string(c.ActorObject)+":"+std::to_string(c.ActorName)+":"+
            std::to_string(c.ActorNumber)+":tier"+std::to_string(unsigned(c.Tier))+":static_reference_v1";
    }
}
