#pragma once
#include "SpriteDiscoveryCore.h"
#include "SpriteClassificationCore.h"

namespace MiniMapSpriteValidation
{
    namespace D=MiniMapSpriteDiscoveryCore;
    namespace J=MiniMapRepresentationCore;
    inline constexpr size_t DetailLimit=16384,OutputLimit=256*1024;
    enum class Outcome { Eligible,Conditional,Unsupported,NoResident,Invalid };
    inline const char* Label(Outcome o)
    {switch(o){case Outcome::Eligible:return "generic_static_template_eligible";case Outcome::Conditional:return "conditional";
    case Outcome::Unsupported:return "unsupported_current_method";case Outcome::NoResident:return "no_validated_resident_representation";default:return "invalid_or_stale";}}
    struct Result
    {
        MiniMapSpriteClassification::Observation Classification;
        Outcome Status=Outcome::Invalid;
        std::string Reason="not_attempted",Route="none",High="not_examined",Low="not_examined",Mass="not_examined",
            Config="not_examined",Selected="null",Details="{}";
        size_t Static{},UniqueMeshes{},Skeletal{},Scene{},NonScene{},Nodes{},MaterialSlots{},OverrideSlots{},
            IdentityVisits{},GraphVisits{},AuditVisits{},Probes{};
        bool Root=false,Transforms=false,Materials=false,Resident=false,Truncated=false,Inherited=false,Child=false,OtherSkinned=false,DerivedStatic=false;
    };
    inline Outcome Classify(const Result& r,bool spline)
    {
        if(spline)return Outcome::Unsupported;
        if(!r.Resident)return Outcome::NoResident;
        if(r.Route=="mass_ism"||(!r.Static&&!r.Truncated))return Outcome::Unsupported;
        if(r.Truncated||!r.Root||!r.Transforms||!r.Materials||r.Inherited||r.Child||r.OtherSkinned||r.DerivedStatic)return Outcome::Conditional;
        return Outcome::Eligible;
    }
    struct Attempt
    {
        D::Candidate Candidate;
        Result Validation;
        std::array<size_t,2> Indices{};size_t InstanceCount{};
        bool Done=false,Current=true;
    };
    template<class Snapshot> struct Scheduler
    {
        std::array<Attempt,D::Capacity> Entries{};size_t Count{},Cursor{},CpuVisits{},NativeAudits{},LastCpuVisits{},
            QueueDeferred{};
        uint64_t Generation{};double SourcePublication{};
        bool Preparing=false,QueueTruncated=false,ScanTruncated=false;
        std::shared_ptr<const Snapshot> Held;
        void Clear(){*this={};}
        void Observe(const D::Registry& registry)
        {
            if(registry.Generation!=Generation){Clear();Generation=registry.Generation;}
            for(size_t i=0;i<Count;++i)Entries[i].Current=false;
            QueueDeferred=0;
            for(size_t i=0;i<registry.Count;++i)
            {
                const auto& c=registry.Entries[i];size_t n=0;
                for(;n<Count;++n)if(Entries[n].Candidate.Identity==c.Identity)break;
                if(n==Count)
                {
                    if(Count==D::Capacity){++QueueDeferred;QueueTruncated=true;continue;}
                    ++Count;
                    if(Held){Preparing=true;Cursor=0;for(size_t j=0;j<Count;++j)if(!Entries[j].Done)Entries[j].InstanceCount=0;}
                }
                Entries[n].Candidate=c;Entries[n].Current=true;
            }
        }
        bool Complete()const
        {for(size_t i=0;i<Count;++i)if(Entries[i].Current&&!Entries[i].Done)return false;return !QueueDeferred;}
        size_t Processed()const
        {size_t n=0;for(size_t i=0;i<Count;++i)n+=Entries[i].Current&&Entries[i].Done;return n;}
        template<class Get,class Audit> bool Service(const D::Registry& registry,std::shared_ptr<const Snapshot> latest,Get get,Audit audit)
        {
            const auto sliceStart=std::chrono::steady_clock::now();
            LastCpuVisits=0;
            if(!Generation||Generation!=registry.Generation||!registry.Complete()||!latest||
                latest->Generation!=Generation||!latest->Complete||Complete())return false;
            if(Held&&Held->Generation!=latest->Generation){Held.reset();return false;}
            if(!Held)
            {
                Held=std::move(latest);Preparing=true;Cursor=0;SourcePublication=Held->PublishedAtSeconds;
                for(size_t i=0;i<Count;++i)if(!Entries[i].Done)Entries[i].InstanceCount=0;
            }
            if(Preparing)
            {
                const auto end=(std::min)(Held->Records.size(),D::RecordLimit);
                while(Cursor<end&&LastCpuVisits<D::VisitsPerTick)
                {
                    if(LastCpuVisits&&std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-sliceStart).count()>=2.0)return false;
                    const auto r=get(Held->Records[Cursor]);
                    if(r.DefinitionValid&&r.VisualInputsValid)
                        for(size_t i=0;i<Count;++i)
                        {
                            auto& e=Entries[i];if(!e.Done&&e.Current&&e.InstanceCount<2&&e.Candidate.Identity==r.Identity)
                                e.Indices[e.InstanceCount++]=Cursor;
                        }
                    ++Cursor;++LastCpuVisits;++CpuVisits;
                }
                // Large source snapshots are not retained across service ticks.
                if(Held->Records.size()>D::RecordLimit){ScanTruncated=true;Preparing=false;}
                else Preparing=Cursor<end;
                if(Preparing)return false;
            }
            if(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-sliceStart).count()>=2.0)return false;
            for(size_t i=0;i<Count;++i)
            {
                auto& e=Entries[i];if(e.Done||!e.Current)continue;
                if(!e.InstanceCount){e.Validation.Reason=ScanTruncated?"representative_scan_truncated":"no_current_matching_instance";}
                else
                {
                    ++NativeAudits;
                    try {e.Validation=audit(Held->Records[e.Indices[0]],e.InstanceCount>1?&Held->Records[e.Indices[1]]:nullptr);}
                    catch(...) {e.Validation={};e.Validation.Reason="candidate_audit_exception";}
                }
                e.Done=true;
                if(Held->Records.size()>D::RecordLimit||Complete())Held.reset();
                return true; // at most one synchronous native audit opportunity per tick
            }
            Held.reset();return false;
        }
    };
    inline std::string CandidateJson(const Attempt& e)
    {
        const auto& c=e.Candidate;const auto& r=e.Validation;
        auto details=r.Details.size()<=DetailLimit?r.Details:"{\"truncated\":true}";
        return J::Object({{"session_local_candidate_key",J::Quote(D::KeyText(c.Identity))},
            {"building_id",std::to_string(c.Identity.Building)},{"observed_instance_count",std::to_string(c.Instances)},
            {"definition_object_index",std::to_string(c.Identity.DefinitionObject)},
            {"definition_fname_index",std::to_string(c.Identity.NameIndex)},{"definition_fname_number",std::to_string(c.Identity.NameNumber)},
            {"key_current",J::Bool(e.Current)},{"processed",J::Bool(e.Done)},
            {"human_readable_name","null"},{"name_provenance",J::Quote("no verified bounded runtime name conversion; offline enum labels are not asset display names")},
            {"classification",J::Quote("unknown")},{"classification_provenance",J::Quote("collector membership does not establish ownership or spawn origin")},
            {"experimental_eligibility",e.Done?J::Quote(Label(r.Status)):J::Quote("deferred")},
            {"reason",J::Quote(r.Reason)}, {"selected_visual_route",J::Quote(r.Route)},
            {"high_detail_actor_status",J::Quote(r.High)},{"low_detail_actor_status",J::Quote(r.Low)},
            {"mass_alternative_status",J::Quote(r.Mass)},{"cached_entity_config",J::Quote(r.Config)},
            {"authored_entity_config",J::Quote("not_examined_soft_reference_contract_unverified")},
            {"placement_actor_class",J::Quote("not_examined_resident_soft_class_contract_unverified")},
            {"selected_class",r.Selected},{"authored_static_instances",std::to_string(r.Static)},
            {"unique_static_meshes",std::to_string(r.UniqueMeshes)},{"skeletal_components",std::to_string(r.Skeletal)},
            {"nonmesh_scene_components",std::to_string(r.Scene)},{"non_scene_components",std::to_string(r.NonScene)},
            {"scs_nodes",std::to_string(r.Nodes)},{"material_slots",std::to_string(r.MaterialSlots)},{"override_slots",std::to_string(r.OverrideSlots)},
            {"native_root_verified",J::Bool(r.Root)},{"effective_transforms_verified",J::Bool(r.Transforms)},
            {"stored_material_mapping_verified",J::Bool(r.Materials)},{"resident_representation_validated",J::Bool(r.Resident)},
            {"inherited_selection_unverified",J::Bool(r.Inherited)},{"child_contributions_unresolved",J::Bool(r.Child)},
            {"other_skinned_contribution_unresolved",J::Bool(r.OtherSkinned)},
            {"derived_static_component_semantics_unverified",J::Bool(r.DerivedStatic)},
            {"instance_specific_spline",J::Bool(c.Identity.Spline)},
            {"identity_visits",std::to_string(r.IdentityVisits)},{"graph_visits",std::to_string(r.GraphVisits)},
            {"audit_visits",std::to_string(r.AuditVisits)},{"representative_probes",std::to_string(r.Probes)},
            {"truncated",J::Bool(r.Truncated||r.Details.size()>DetailLimit)},{"verified_safe_to_capture","false"},
            {"authored_audit",details}});
    }
    template<class Snapshot> std::string Json(const Scheduler<Snapshot>& s,const D::Registry& registry,size_t limit=OutputLimit)
    {
        limit=(std::min)(limit,OutputLimit);std::string records;size_t emitted=0;std::array<size_t,5> outcomes{};
        if(limit<4096)return "{\"stage\":\"R2.6b\",\"output_truncated\":true,\"new_sprites_generated\":0}";
        size_t work=0,graphs=0,audits=0;
        for(size_t i=0;i<s.Count;++i)
        {
            const auto& e=s.Entries[i];if(e.Current&&e.Done)++outcomes[size_t(e.Validation.Status)];
            work+=e.Validation.IdentityVisits;graphs+=e.Validation.GraphVisits;audits+=e.Validation.AuditVisits;
            const auto record=CandidateJson(e);
            if(records.size()+record.size()+4096>limit)continue;
            if(emitted)records+=',';records+=record;++emitted;
        }
        return J::Object({{"schema_version","1"},{"stage",J::Quote("R2.6b")},{"game_build",J::Quote("CL-127004")},
            {"world_generation",std::to_string(s.Generation)},{"source_publication_steady_seconds",J::Number(s.SourcePublication)},
            {"discovered_candidates",std::to_string(registry.Count)},{"processed",std::to_string(s.Processed())},
            {"validated",std::to_string(outcomes[0])},{"conditional",std::to_string(outcomes[1])},
            {"unsupported",std::to_string(outcomes[2])},{"no_resident_representation",std::to_string(outcomes[3])},
            {"invalid_or_stale",std::to_string(outcomes[4])},
            {"remaining_or_deferred",std::to_string(registry.Count-(std::min)(registry.Count,s.Processed()))},
            {"completion_status",J::Quote(s.Generation&&s.Complete()&&registry.Complete()?"complete":"partial_or_invalidated")},
            {"source_registry_complete",J::Bool(registry.Complete())},{"queue_truncated",J::Bool(s.QueueTruncated)},
            {"representative_scan_truncated",J::Bool(s.ScanTruncated)},{"output_truncated",J::Bool(emitted<s.Count)},
            {"candidate_records_emitted",std::to_string(emitted)},{"cpu_record_visits",std::to_string(s.CpuVisits)},
            {"native_candidate_audits",std::to_string(s.NativeAudits)}, {"identity_visits",std::to_string(work)},
            {"graph_visits",std::to_string(graphs)},{"audit_visits",std::to_string(audits)},
            {"new_sprites_generated","0"},{"capture_enabled","false"},{"candidates","["+records+"]"}});
    }
    template<class Snapshot> std::string ClassificationJson(const Scheduler<Snapshot>& s,const D::Registry& registry,size_t limit=OutputLimit)
    {
        limit=(std::min)(limit,OutputLimit);
        if(limit<4096)return "{\"stage\":\"R2.6b.4\",\"output_truncated\":true,\"capture_enabled\":false}";
        std::string rows;size_t emitted=0,unknown=0,include=0,exclude=0,invalid=0,visits=0,tags=0,invalidSource=0;
        bool truncated=s.QueueTruncated||s.ScanTruncated;
        for(size_t i=0;i<s.Count;++i)
        {
            const auto& e=s.Entries[i];if(!e.Current)continue;
            const auto& o=e.Validation.Classification;
            if(e.Done)
            {
                using E=MiniMapSpriteClassification::Exclusion;
                unknown+=o.CachedPolicy.ExcludeFromMiniMap==E::Unknown;include+=o.CachedPolicy.ExcludeFromMiniMap==E::Include;
                exclude+=o.CachedPolicy.ExcludeFromMiniMap==E::Exclude;invalid+=!o.Fixed;
                invalidSource+=e.Validation.Status==Outcome::Invalid;
            }
            truncated|=o.Truncated;visits+=o.IdentityVisits;tags+=o.TagVisits;
            const auto row=J::Object({{"session_local_candidate_key",J::Quote(D::KeyText(e.Candidate.Identity))},
                {"building_id",std::to_string(e.Candidate.Identity.Building)},
                {"definition_object_index",std::to_string(e.Candidate.Identity.DefinitionObject)},
                {"definition_fname_index",std::to_string(e.Candidate.Identity.NameIndex)},
                {"definition_fname_number",std::to_string(e.Candidate.Identity.NameNumber)},
                {"observed_instance_count",std::to_string(e.Candidate.Instances)},{"processed",J::Bool(e.Done)},
                {"observation",MiniMapSpriteClassification::Json(o)}});
            if(rows.size()+row.size()+4096>limit)continue;
            if(emitted++)rows+=',';rows+=row;
        }
        return J::Object({{"schema_version","1"},{"stage",J::Quote("R2.6b.4")},{"game_build",J::Quote("CL-127004")},
            {"world_generation",std::to_string(s.Generation)},{"source_publication_steady_seconds",J::Number(s.SourcePublication)},
            {"candidates_discovered",std::to_string(registry.Count)},{"candidates_processed",std::to_string(s.Processed())},
            {"candidates_remaining",std::to_string(registry.Count-(std::min)(registry.Count,s.Processed()))},
            {"exclude",std::to_string(exclude)},{"include",std::to_string(include)},{"unknown",std::to_string(unknown)},
            {"invalid_or_unavailable",std::to_string(invalid)},{"identity_visits",std::to_string(visits)},
            {"invalid_or_stale_source",std::to_string(invalidSource)},{"tag_visits",std::to_string(tags)},
            {"native_candidate_audits",std::to_string(s.NativeAudits)},{"cpu_record_visits",std::to_string(s.CpuVisits)},
            {"completion_status",J::Quote(s.Generation&&s.Complete()&&registry.Complete()?"complete":"partial_or_invalidated")},
            {"inputs_truncated",J::Bool(truncated)},{"output_truncated",J::Bool(emitted<registry.Count)},
            {"source_registry_complete",J::Bool(registry.Complete())},
            {"observation_refresh",J::Quote("once_per_definition_key_per_world_generation; mutable inputs may change; no trusted policy cached")},
            {"candidates_emitted",std::to_string(emitted)},{"policy_observational_only","true"},
            {"new_sprites_generated","0"},{"capture_enabled","false"},{"candidates","["+rows+"]"}});
    }
}
