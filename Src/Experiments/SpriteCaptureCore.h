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
            (c.General?"development_static_template_unknown_classification":"experimental_static_reference_unknown_policy_override"):"validated_candidate"};
    }
    namespace Experiment
    {
        // Temporary R2.7a validation target, NOT a production whitelist.
        inline bool Enabled(uint32_t building){return building==336;}
        inline Decision Evaluate(const Candidate& c,const Evidence& e)
        {
            if(c.Classification.ExcludeFromMiniMap==K::Exclusion::Exclude||c.Classification.Reason=="InteriorTag")return {State::Rejected,"classification_excluded"};
            if(!Enabled(c.Identity.Building))return {State::Rejected,"experimental_target_not_enabled"};
            return Decide(c,e,true);
        }
    }
    inline std::string SessionVisualKey(const Candidate& c)
    {
        return D::KeyText(c.Identity)+":actor"+std::to_string(c.ActorObject)+":"+std::to_string(c.ActorName)+":"+
            std::to_string(c.ActorNumber)+":tier"+std::to_string(unsigned(c.Tier))+":static_reference_v1";
    }
    inline std::string Json(const Candidate& c,Decision d)
    {
        namespace J=MiniMapRepresentationCore;
        return J::Object({{"stage",J::Quote("R2.7a")},{"session_local_candidate_key",J::Quote(D::KeyText(c.Identity))},
            {"session_local_visual_key",J::Quote(SessionVisualKey(c))},
            {"selected_class_object_index",std::to_string(c.ActorObject)},{"selected_class_fname_index",std::to_string(c.ActorName)},
            {"selected_class_fname_number",std::to_string(c.ActorNumber)},
            {"classification",J::Quote(K::Label(c.Classification.ExcludeFromMiniMap))},{"classification_reason",J::Quote(c.Classification.Reason)},
            {"classification_scope",J::Quote("assembly_audit_input; capture_owner_rechecks_before_submission")},
            {"representation_verified",J::Bool(c.RepresentationVerified)},{"evidence_truncated",J::Bool(c.Truncated)},
            {"capture_accepted",J::Bool(d.Allowed())},{"eligibility_reason",J::Quote(d.Reason)},
            {"mode",J::Quote("experimental_authored_static_reference; skeletal_omitted; no_live_actor_equivalence")},
            {"persistent_cache_key_available","false"},{"visual_equivalence",J::Quote("provisional; template_reference_only")},
            {"output_scope",J::Quote("experimental_diagnostics; session association only; not an atlas entry")},
            {"color_output",Experiment::Enabled(c.Identity.Building)?J::Quote("Smelter_StaticAssembly.png"):"null"},
            {"alpha_output",Experiment::Enabled(c.Identity.Building)?J::Quote("Smelter_StaticAssembly_Alpha.png"):"null"},
            {"capture_result",J::Quote(d.Allowed()?"pending; completion_and_export_reported_by_existing_capture_owner":"not_started")}});
    }
}
