#pragma once
#include "RepresentationMetadataCore.h"
#include <array>

namespace MiniMapSpriteClassification
{
    namespace J=MiniMapRepresentationCore;
    inline constexpr size_t TagLimit=64;
    enum class Exclusion { Unknown,Exclude,Include };
    inline const char* Label(Exclusion e)
    {return e==Exclusion::Exclude?"Exclude":e==Exclusion::Include?"Include":"Unknown";}
    // Interior is an explicit sprite policy, independent of placement capability.
    // The remaining premises still require authoritative definition-wide evidence.
    struct Evidence
    {
        bool TypeInvariant=false,HabitatCapability=false,PermanentProtection=false,
            OutdoorOnly=false,PlayerManageable=false,Conflict=false,InteriorTag=false;
    };
    struct Policy
    {
        Exclusion ExcludeFromMiniMap=Exclusion::Unknown;
        std::string Reason="InsufficientEvidence",Provenance="native_policy_contract_unresolved";
    };
    inline Policy Decide(Evidence e)
    {
        Policy p;
        // A confirmed tag wins even over conflicting or positive placement inputs.
        if(e.InteriorTag)
        {p.ExcludeFromMiniMap=Exclusion::Exclude;p.Reason="InteriorTag";p.Provenance="UCrBuildingData.Tags; exact FName equality";return p;}
        if(e.Conflict){p.Reason="ConflictingEvidence";return p;}
        if(!e.TypeInvariant)return p;
        if(e.HabitatCapability||e.PermanentProtection)
        {p.ExcludeFromMiniMap=Exclusion::Exclude;p.Reason=e.HabitatCapability?"ConfirmedHabitatPlacementCapability":"ExplicitPermanentProtection";}
        else if(e.OutdoorOnly&&e.PlayerManageable)
        {p.ExcludeFromMiniMap=Exclusion::Include;p.Reason="ConfirmedOutdoorOnly";}
        if(p.ExcludeFromMiniMap!=Exclusion::Unknown)p.Provenance="authoritative_type_invariant_required";
        return p;
    }
    // Unknown preserves existing behavior; absence of Interior never implies Include.
    inline bool SpriteAllowed(const Policy& p){return p.ExcludeFromMiniMap!=Exclusion::Exclude;}
    struct Name {int32_t Index{};uint32_t Number{};};
    struct SoftState
    {
        std::string Presence="unverified",Weak="not_resolved_native_contract_omitted";
        Name Package,Asset;int32_t WeakIndex{},WeakSerial{},SubpathCount{};
        bool Read=false;
    };
    inline const char* WeakShape(int32_t index,int32_t serial)
    {return serial==0?"empty_serial_zero":index<0||serial<0?"invalid_shape":"populated_unresolved_serial_not_validated";}
    struct Observation
    {
        Policy CachedPolicy;
        std::string Status="not_attempted",Reason="candidate_not_audited",BuildingData="not_examined",
            CachedConfig="not_examined",Helper="null",Custom="null",TagsStatus="not_examined",
            InteriorTag="unknown",InteriorComparison="unavailable";
        bool Fixed=false,Subclass=false,Truncated=false;
        std::array<bool,7> Flags{};
        uint8_t UI{},Subtype{};bool Dynamic=false;
        std::array<Name,TagLimit> Tags{};size_t TagCount{},IdentityVisits{},TagVisits{};
        SoftState ConfigReference,ActorReference;
    };
    inline std::string SoftJson(const SoftState& s)
    {
        return J::Object({{"presence",J::Quote(s.Presence)},{"weak_cache_state",J::Quote(s.Weak)},
            {"cache_resolution",J::Quote("not_attempted_no_new_native_callable")},
            {"weak_index",s.Read?std::to_string(s.WeakIndex):"null"},{"weak_serial",s.Read?std::to_string(s.WeakSerial):"null"},
            {"package_fname_index",s.Read?std::to_string(s.Package.Index):"null"},
            {"package_fname_number",s.Read?std::to_string(s.Package.Number):"null"},
            {"asset_fname_index",s.Read?std::to_string(s.Asset.Index):"null"},
            {"asset_fname_number",s.Read?std::to_string(s.Asset.Number):"null"},
            {"subpath_count",s.Read?std::to_string(s.SubpathCount):"null"}});
    }
    inline std::string Json(const Observation& o)
    {
        std::string tags="[";
        for(size_t i=0;i<o.TagCount;++i)
        {if(i)tags+=',';tags+=J::Object({{"comparison_index",std::to_string(o.Tags[i].Index)},{"number",std::to_string(o.Tags[i].Number)}});}
        tags+=']';
        return J::Object({{"status",J::Quote(o.Status)},{"reason",J::Quote(o.Reason)},
            {"scope",J::Quote("definition_inputs; no instance origin inferred")},
            {"ExcludeFromMiniMap",J::Quote(Label(o.CachedPolicy.ExcludeFromMiniMap))},
            {"exclusion_reason",J::Quote(o.CachedPolicy.Reason)},{"policy_provenance",J::Quote(o.CachedPolicy.Provenance)},
            {"building_sprite_allowed",J::Bool(SpriteAllowed(o.CachedPolicy))},
            {"interior_tag",J::Quote(o.InteriorTag)},{"interior_tag_comparison",J::Quote(o.InteriorComparison)},
            {"policy_effective_version",J::Quote("R2.6b.6_InteriorTag")},{"policy_observational_only","true"},
            {"bCanBePlacedOnTerrain",o.Fixed?J::Bool(o.Flags[0]):"null"},
            {"bHasToBePlacedOnTerrain",o.Fixed?J::Bool(o.Flags[1]):"null"},
            {"RequireHabitat",o.Fixed?J::Bool(o.Flags[2]):"null"},
            {"RequiresBeingInsideHabitat",o.Fixed?J::Bool(o.Flags[3]):"null"},
            {"SplineBased",o.Fixed?J::Bool(o.Flags[4]):"null"},
            {"CheckBaseCoreArea",o.Fixed?J::Bool(o.Flags[5]):"null"},
            {"CheckOutsideBaseCoreArea",o.Fixed?J::Bool(o.Flags[6]):"null"},
            {"helper_class",o.Helper},{"custom_placement_method",o.Custom},
            {"building_data_status",J::Quote(o.BuildingData)},
            {"ui_category",o.Subclass?std::to_string(o.UI):"null"},{"ui_subtype",o.Subclass?std::to_string(o.Subtype):"null"},
            {"CanBeDynamic",o.Subclass?J::Bool(o.Dynamic):"null"},
            {"definition_tags_status",J::Quote(o.TagsStatus)},{"definition_tags",tags},
            {"authored_interior_classification",J::Quote(o.InteriorTag=="present"?"Interior":o.InteriorTag=="absent"?"no_Interior_tag; outdoor_capability_unknown":"unknown")},
            {"cached_entity_config",J::Quote(o.CachedConfig)},
            {"authored_configuration",SoftJson(o.ConfigReference)},{"placement_actor_class",SoftJson(o.ActorReference)},
            {"supports_habitat_interior",J::Quote("unknown_contextual_policy_unresolved")},
            {"live_actor_status",J::Quote("unverified_actor_relationship_not_attempted")},
            {"NonDeconstructible",J::Quote("unverified")},{"instance_interior_tag",J::Quote("unverified_producer_and_anchor_contract")},
            {"permanent_origin",J::Quote("unknown")},{"deconstruction_permission","null"},
            {"confidence",J::Quote("verified_stored_inputs_only; no capability_or_origin_inference")},
            {"identity_visits",std::to_string(o.IdentityVisits)},{"tag_visits",std::to_string(o.TagVisits)},
            {"truncated",J::Bool(o.Truncated)}});
    }
}
