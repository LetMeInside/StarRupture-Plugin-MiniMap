#pragma once
#include "SpriteDiscoveryCore.h"
#include "SpriteClassificationCore.h"
#include <type_traits>
#include <stdexcept>

namespace MiniMapSpriteValidation
{
    namespace D=MiniMapSpriteDiscoveryCore;
    namespace J=MiniMapRepresentationCore;
    inline constexpr size_t DetailLimit=16384,OutputLimit=256*1024;
    enum class Outcome { Eligible,Conditional,Unsupported,NoResident,Invalid };
    enum class AuditDisposition { Completed,Interrupted };
    inline const char* Label(Outcome o)
    {switch(o){case Outcome::Eligible:return "generic_static_template_eligible";case Outcome::Conditional:return "conditional";
    case Outcome::Unsupported:return "unsupported_current_method";case Outcome::NoResident:return "no_validated_resident_representation";default:return "invalid_or_stale";}}
    struct Result
    {
        MiniMapSpriteClassification::Observation Classification;
        Outcome Status=Outcome::Invalid;
        AuditDisposition Disposition=AuditDisposition::Completed;
        std::string Reason="not_attempted",Route="none",High="not_examined",Low="not_examined",Mass="not_examined",
            Config="not_examined",AuthoredClass="not_examined",Selected="null",Details="{}",Inspection="{}",Profile="{}",
            CookedGeometry="{\"status\":\"not_attempted\"}",SkeletalMetadata="{\"status\":\"not_attempted\"}";
        size_t Static{},UniqueMeshes{},Skeletal{},Scene{},NonScene{},Nodes{},MaterialSlots{},OverrideSlots{},
            IdentityVisits{},GraphVisits{},AuditVisits{},Probes{};
        bool Root=false,Transforms=false,Materials=false,Resident=false,Truncated=false,Inherited=false,Child=false,OtherSkinned=false,DerivedStatic=false;
        // Allocation-free even when the operational failure is bad_alloc.
        void MarkInterrupted()noexcept{Disposition=AuditDisposition::Interrupted;Status=Outcome::Invalid;}
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
        D::Candidate Candidate;Result Validation;
        std::array<size_t,2> Indices{};size_t InstanceCount{};
        uint64_t MembershipEpoch{},RepresentativeEpoch{};
        double RecoveryPublication{};
        bool Done=false,MissingRepresentative=false,RecoveryRequired=false;
    };
    // Stable ordinal cells plus exact-key lookup, detached in O(1) on reset.
    // No native request ever owns an Attempt pointer.
    class AttemptStorage
    {
        struct State {std::map<size_t,Attempt> Cells;std::map<D::Key,size_t,D::KeyLess> Index;State* Next{};};
        State* Active{};State* Head{};State* Tail{};size_t Borrows{};
    public:
        AttemptStorage()=default;AttemptStorage(const AttemptStorage&)=delete;
        struct Borrow {AttemptStorage& Owner;explicit Borrow(AttemptStorage& owner):Owner(owner){++Owner.Borrows;}~Borrow(){--Owner.Borrows;}Borrow(const Borrow&)=delete;};
        size_t size()const noexcept{return Active?Active->Cells.size():0;}
        Attempt& operator[](size_t i){return Active->Cells.at(i);}
        const Attempt& operator[](size_t i)const{return Active->Cells.at(i);}
        Attempt* Find(const D::Key& key)
        {if(!Active)return nullptr;auto it=Active->Index.find(key);return it==Active->Index.end()?nullptr:&Active->Cells.at(it->second);}
        Attempt& Insert(const D::Candidate& candidate)
        {
            if(!Active)Active=new State;
            const auto ordinal=Active->Cells.size();
            Attempt prepared;prepared.Candidate=candidate;
            auto [cell,added]=Active->Cells.emplace(ordinal,std::move(prepared));
            if(!added)throw std::logic_error("validation ordinal collision");
            try{auto [index,inserted]=Active->Index.emplace(candidate.Identity,ordinal);
                if(!inserted)throw std::logic_error("duplicate validation identity");}
            catch(...){Active->Cells.erase(cell);throw;}
            return cell->second;
        }
        void Retire()noexcept
        {if(!Active)return;if(Tail)Tail->Next=Active;else Head=Active;Tail=Active;Active=nullptr;}
        size_t Maintain(size_t limit=16,double budgetMs=2.0)
        {
            if(Borrows)return 0;
            const auto start=std::chrono::steady_clock::now();size_t units=0;
            while(Head&&units<limit)
            {
                auto* state=Head;
                if(!state->Cells.empty()){auto it=state->Cells.begin();state->Index.erase(it->second.Candidate.Identity);state->Cells.erase(it);}
                if(state->Cells.empty()){Head=state->Next;if(!Head)Tail=nullptr;delete state;}
                ++units;if(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()>=budgetMs)break;
            }
            return units;
        }
        ~AttemptStorage(){Retire();while(Maintain(16,(std::numeric_limits<double>::max)())){}} // Final shutdown only.
    };
    template<class Snapshot> struct Scheduler
    {
        AttemptStorage Entries;size_t Count{},Cursor{},CpuVisits{},NativeAudits{},LastCpuVisits{},QueueDeferred{};
        uint64_t Generation{};double SourcePublication{};
        bool Preparing=false,QueueTruncated=false,ScanTruncated=false;
        D::Key LastCompletedKey;std::shared_ptr<const Snapshot> Held;
        using RegistryPublication=std::shared_ptr<const D::Registry>;
        // One pinned work plan, one latest target, one committed result view.
        // Supersession coalesces the target; it never restarts in-flight adoption.
        RegistryPublication PendingRegistry,AdoptedRegistry,LatestRegistry;
        size_t AdoptionCursor{},LastAdoptionVisits{},LastAdoptionNew{},AdoptionFailures{},CompletedCurrent{},AdoptionDone{},AuditCursor{},LastSelectionVisits{},ScanFailures{};
        uint64_t NextEpoch{},PendingEpoch{},CurrentEpoch{},ScanEpoch{};
        double AdoptionPublication{},BlockedSourcePublication{};
        bool Adopting=false,AdoptionBlocked=false,AdoptionAllocationBlocked=false,HasAdoptionPublication=false,WaitingForSource=false,ScanAllocationBlocked=false;
        std::array<size_t,5> Outcomes{},AdoptionOutcomes{};
        size_t TotalIdentityVisits{},TotalGraphVisits{},TotalAuditVisits{};
        size_t MaintainRetired(size_t limit=16,double budgetMs=2.0){return Entries.Maintain(limit,budgetMs);}
        void Clear()noexcept
        {
            Entries.Retire();Count=Cursor=CpuVisits=NativeAudits=LastCpuVisits=QueueDeferred=0;
            Generation=0;SourcePublication=0;Preparing=QueueTruncated=ScanTruncated=false;
            LastCompletedKey={};Held.reset();PendingRegistry.reset();AdoptedRegistry.reset();LatestRegistry.reset();
            AdoptionCursor=LastAdoptionVisits=LastAdoptionNew=AdoptionFailures=CompletedCurrent=AdoptionDone=AuditCursor=LastSelectionVisits=ScanFailures=0;
            NextEpoch=PendingEpoch=CurrentEpoch=ScanEpoch=0;AdoptionPublication=BlockedSourcePublication=0;
            Adopting=AdoptionBlocked=AdoptionAllocationBlocked=HasAdoptionPublication=WaitingForSource=ScanAllocationBlocked=false;
            Outcomes={};AdoptionOutcomes={};TotalIdentityVisits=TotalGraphVisits=TotalAuditVisits=0;
        }
        bool ReadyFor(const D::Registry& r)const noexcept
        {return !Adopting&&!AdoptionBlocked&&AdoptedRegistry==LatestRegistry&&AdoptedRegistry.get()==&r&&r.Complete()&&Generation==r.Generation;}
        bool IsCurrent(const Attempt& e)const noexcept{return CurrentEpoch&&e.MembershipEpoch==CurrentEpoch;}
        bool RequestReevaluation(const D::Key& key)
        {
            if(Adopting||AdoptionBlocked||!AdoptedRegistry||key.Generation!=Generation)return false;
            auto* e=Entries.Find(key);if(!e||!IsCurrent(*e))return false;
            if(e->Done){if(!CompletedCurrent||!Outcomes[size_t(e->Validation.Status)])throw std::logic_error("validation completion counter underflow");--CompletedCurrent;--Outcomes[size_t(e->Validation.Status)];e->Done=false;}
            e->RecoveryRequired=false;e->MissingRepresentative=false;Held.reset();Preparing=false;WaitingForSource=false;return true;
        }
        bool BeginAdoption()
        {
            if(NextEpoch==UINT64_MAX){AdoptionBlocked=true;return false;}
            PendingEpoch=++NextEpoch;PendingRegistry=LatestRegistry;Adopting=true;
            AdoptionAllocationBlocked=false;AdoptionCursor=0;AdoptionDone=0;AdoptionOutcomes={};return true;
        }
        bool Observe(const RegistryPublication& r,uint64_t currentGeneration)
        {
            if(!r||!currentGeneration||r->Generation!=currentGeneration||!r->Complete()||
                !std::get_deleter<D::RetireRegistry>(r)||!std::isfinite(r->PublicationSeconds))
            {AdoptionBlocked=true;return false;}
            if(Generation!=currentGeneration){Clear();Generation=currentGeneration;}
            if(HasAdoptionPublication&&r->PublicationSeconds<AdoptionPublication){AdoptionBlocked=true;return false;}
            AdoptionBlocked=false;if(r==LatestRegistry)return Adopting||AdoptedRegistry==r?true:BeginAdoption();
            LatestRegistry=r;AdoptionPublication=r->PublicationSeconds;HasAdoptionPublication=true;
            Held.reset();Preparing=false;WaitingForSource=false;Cursor=0;
            return Adopting?true:BeginAdoption();
        }
        bool ServiceAdoption(double budgetMs=2.0)
        {
            const auto start=std::chrono::steady_clock::now();LastAdoptionVisits=LastAdoptionNew=0;
            if(!Adopting||AdoptionBlocked||!PendingRegistry)return false;AdoptionAllocationBlocked=false;
            while(AdoptionCursor<PendingRegistry->Count&&LastAdoptionVisits<D::VisitsPerTick&&LastAdoptionNew<D::NewPerTick&&
                std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<budgetMs)
            {
                const auto& c=PendingRegistry->Entries[AdoptionCursor];auto* e=Entries.Find(c.Identity);
                if(!e){try{e=&Entries.Insert(c);Count=Entries.size();++LastAdoptionNew;}
                    catch(const std::bad_alloc&){++AdoptionFailures;AdoptionAllocationBlocked=true;return false;}}
                e->Candidate=c;e->MembershipEpoch=PendingEpoch;
                if(e->Done){++AdoptionDone;++AdoptionOutcomes[size_t(e->Validation.Status)];}
                ++AdoptionCursor;++LastAdoptionVisits;
            }
            if(AdoptionCursor<PendingRegistry->Count)return false;
            AdoptedRegistry.swap(PendingRegistry);PendingRegistry.reset();Adopting=false;
            CurrentEpoch=PendingEpoch;CompletedCurrent=AdoptionDone;Outcomes=AdoptionOutcomes;AuditCursor=0;
            if(AdoptedRegistry!=LatestRegistry){BeginAdoption();return false;}
            return true;
        }
        bool Complete()const{return !Adopting&&!AdoptionBlocked&&AdoptedRegistry&&AdoptedRegistry==LatestRegistry&&CompletedCurrent==AdoptedRegistry->Count;}
        size_t Processed()const{return CompletedCurrent;}
        template<class Get,class Audit> bool Service(const D::Registry& registry,std::shared_ptr<const Snapshot> latest,Get get,Audit audit,double budgetMs=2.0)
        {
            const auto start=std::chrono::steady_clock::now();LastCpuVisits=LastSelectionVisits=0;ScanAllocationBlocked=false;
            if(!ReadyFor(registry)||!latest||latest->Generation!=Generation||!latest->Complete||
                latest->PublishedAtSeconds<registry.PublicationSeconds||Complete())return false;
            if(!Held)
            {
                if(WaitingForSource&&latest->PublishedAtSeconds<=BlockedSourcePublication)return false;
                if(ScanEpoch==UINT64_MAX){AdoptionBlocked=true;return false;}
                ++ScanEpoch;Held=std::move(latest);Preparing=true;WaitingForSource=false;Cursor=AuditCursor=0;SourcePublication=Held->PublishedAtSeconds;
            }
            if(Preparing)
            {
                while(Cursor<Held->Records.size()&&LastCpuVisits<D::VisitsPerTick&&
                    std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<budgetMs)
                {
                    try{
                        const auto r=get(Held->Records[Cursor]);
                        if(r.DefinitionValid&&r.VisualInputsValid)if(auto* e=Entries.Find(r.Identity);e&&IsCurrent(*e)&&!e->Done&&
                            (!e->RecoveryRequired||SourcePublication>e->RecoveryPublication))
                        {
                            // A new complete source wakes interrupted work even
                            // before renderer dependency tracking exists. The
                            // unchanged source never causes automatic reaudits.
                            e->RecoveryRequired=false;
                            if(e->RepresentativeEpoch!=ScanEpoch){e->RepresentativeEpoch=ScanEpoch;e->InstanceCount=0;}
                            if(e->InstanceCount<2)e->Indices[e->InstanceCount++]=Cursor;
                        }
                    }catch(const std::bad_alloc&){ScanAllocationBlocked=true;++ScanFailures;return false;}
                    ++Cursor;++LastCpuVisits;++CpuVisits;
                }
                Preparing=Cursor<Held->Records.size();if(Preparing)return false;
            }
            while(AuditCursor<registry.Count&&LastSelectionVisits<D::VisitsPerTick&&
                std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<budgetMs)
            {
                auto* e=Entries.Find(registry.Entries[AuditCursor++].Identity);++LastSelectionVisits;
                if(!e||!IsCurrent(*e)||e->Done||e->RecoveryRequired)continue;
                if(e->RepresentativeEpoch!=ScanEpoch||!e->InstanceCount){e->MissingRepresentative=true;continue;}
                e->MissingRepresentative=false;++NativeAudits;
                // Native calls may reenter a world/source transition. Pin CPU
                // retirement and source ownership for the synchronous borrow,
                // then revalidate before mutating completion state.
                AttemptStorage::Borrow borrow(Entries);const auto source=Held;
                const auto publication=AdoptedRegistry;const auto world=Generation;const auto epoch=CurrentEpoch;const auto key=e->Candidate.Identity;
                auto current=[&]{return Generation==world&&CurrentEpoch==epoch&&AdoptedRegistry==publication&&ReadyFor(registry)&&Entries.Find(key)==e;};
                try {auto result=audit(source->Records[e->Indices[0]],e->InstanceCount>1?&source->Records[e->Indices[1]]:nullptr);
                    if(!current())return false;e->Validation=std::move(result);}
                catch(...){if(!current())return false;e->Validation.MarkInterrupted();}
                if(e->Validation.Disposition==AuditDisposition::Interrupted)
                {e->RecoveryRequired=true;e->RecoveryPublication=SourcePublication;++ScanFailures;LastCompletedKey=e->Candidate.Identity;return true;}
                e->Done=true;++CompletedCurrent;++Outcomes[size_t(e->Validation.Status)];LastCompletedKey=e->Candidate.Identity;
                TotalIdentityVisits+=e->Validation.IdentityVisits;TotalGraphVisits+=e->Validation.GraphVisits;TotalAuditVisits+=e->Validation.AuditVisits;
                if(Complete())Held.reset();return true;
            }
            // Conditional P2 progress limitation: missing representatives may
            // wait for a newer complete source or explicit reevaluation. Avoiding
            // unchanged-input rescans means recovery need not be immediate; if
            // no trigger arrives, progress may stall without corrupting prior results.
            if(AuditCursor==registry.Count){BlockedSourcePublication=SourcePublication;WaitingForSource=true;Held.reset();}
            return false;
        }
    };
    template<class Snapshot> std::string AdoptionStatusJson(const Scheduler<Snapshot>& s,const D::Registry& registry,const char* stage="R2.6b")
    {
        return J::Object({{"stage",J::Quote(stage)},
            {"world_generation",std::to_string(s.Generation)},
            {"source_registry_adopted",J::Bool(s.ReadyFor(registry))},
            {"completion_status",J::Quote("consumer_execution_deferred")},
            {"adoption_pending",J::Bool(s.Adopting)},
            {"adoption_blocked",J::Bool(s.AdoptionBlocked)},
            {"adoption_cursor",std::to_string(s.AdoptionCursor)},
            {"adoption_allocation_blocked",J::Bool(s.AdoptionAllocationBlocked)},
            {"adoption_allocation_failures",std::to_string(s.AdoptionFailures)},
            {"retained_attempts",std::to_string(s.Count)},
            {"capacity_deferred",std::to_string(s.QueueDeferred)},
            {"representative_record_limit_deferred","false"},
            {"representative_scan_incomplete",J::Bool(s.Preparing)},
            {"scan_allocation_blocked",J::Bool(s.ScanAllocationBlocked)},
            {"waiting_for_source_advance",J::Bool(s.WaitingForSource)},
            {"scan_failures",std::to_string(s.ScanFailures)}});
    }
    inline std::string CandidateJson(const Attempt& e,bool current=false)
    {
        const auto& c=e.Candidate;const auto& r=e.Validation;
        auto details=r.Details.size()<=DetailLimit?r.Details:"{\"truncated\":true}";
        return J::Object({{"session_local_candidate_key",J::Quote(D::KeyText(c.Identity))},
            {"building_id",std::to_string(c.Identity.Building)},{"observed_instance_count",std::to_string(c.Instances)},
            {"definition_object_index",std::to_string(c.Identity.DefinitionObject)},
            {"definition_fname_index",std::to_string(c.Identity.NameIndex)},{"definition_fname_number",std::to_string(c.Identity.NameNumber)},
            {"key_current",J::Bool(current)},{"processed",J::Bool(e.Done)},{"representative_unavailable",J::Bool(e.MissingRepresentative)},{"recovery_required",J::Bool(e.RecoveryRequired)},
            {"audit_disposition",J::Quote(r.Disposition==AuditDisposition::Interrupted?"interrupted":e.Done?"completed":"pending")},
            {"recovery_trigger",J::Quote(e.RecoveryRequired?"newer_complete_source_or_explicit_reevaluation":"none")},
            {"recovery_source_publication",J::Number(e.RecoveryPublication)},
            {"human_readable_name","null"},{"name_provenance",J::Quote("no verified bounded runtime name conversion; offline enum labels are not asset display names")},
            {"classification",J::Quote("unknown")},{"classification_provenance",J::Quote("collector membership does not establish ownership or spawn origin")},
            {"experimental_eligibility",e.Done?J::Quote(Label(r.Status)):J::Quote("deferred")},
            {"reason",J::Quote(r.Reason)}, {"selected_visual_route",J::Quote(r.Route)},
            {"high_detail_actor_status",J::Quote(r.High)},{"low_detail_actor_status",J::Quote(r.Low)},
            {"mass_alternative_status",J::Quote(r.Mass)},{"cached_entity_config",J::Quote(r.Config)},
            {"authored_entity_config",J::Quote("not_examined_soft_reference_contract_unverified")},
            {"placement_actor_class",J::Quote(r.AuthoredClass)},
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
            {"authored_audit",details},{"static_component_inspection",r.Inspection},{"validation_profile",r.Profile},
            {"cooked_geometry",r.CookedGeometry},{"skeletal_source_metadata",r.SkeletalMetadata}});
    }
    template<class Snapshot> std::string Json(const Scheduler<Snapshot>& s,const D::Registry& registry,size_t limit=OutputLimit)
    {
        limit=(std::min)(limit,OutputLimit);std::string records;size_t emitted=0;auto outcomes=s.Outcomes;
        if(limit<4096)return "{\"stage\":\"R2.6b\",\"output_truncated\":true,\"new_sprites_generated\":0}";
        size_t work=s.TotalIdentityVisits,graphs=s.TotalGraphVisits,audits=s.TotalAuditVisits;
        for(size_t i=0;i<s.Count&&i<D::VisitsPerTick;++i)
        {
            const auto& e=s.Entries[i];

            const auto record=CandidateJson(e,s.IsCurrent(e));
            if(records.size()+record.size()+4096>limit)break;
            if(emitted)records+=',';records+=record;++emitted;
        }
        return J::Object({{"schema_version","1"},{"stage",J::Quote("R2.6b")},{"game_build",J::Quote("CL-127004")},
            {"world_generation",std::to_string(s.Generation)},{"source_publication_steady_seconds",J::Number(s.SourcePublication)},
            {"discovered_candidates",std::to_string(registry.Count)},{"processed",std::to_string(s.Processed())},
            {"validated",std::to_string(outcomes[0])},{"conditional",std::to_string(outcomes[1])},
            {"unsupported",std::to_string(outcomes[2])},{"no_resident_representation",std::to_string(outcomes[3])},
            {"invalid_or_stale",std::to_string(outcomes[4])},
            {"remaining_or_deferred",std::to_string(registry.Count-(std::min)(registry.Count,s.Processed()))},
            {"completion_status",J::Quote(s.Generation&&s.ReadyFor(registry)&&s.Complete()?"complete":"partial_or_invalidated")},
            {"source_registry_complete",J::Bool(registry.Complete())},
            {"source_registry_adopted",J::Bool(s.ReadyFor(registry))},
            {"adoption_pending",J::Bool(s.Adopting)},
            {"adoption_cursor",std::to_string(s.AdoptionCursor)},
            {"adoption_allocation_blocked",J::Bool(s.AdoptionAllocationBlocked)},
            {"adoption_allocation_failures",std::to_string(s.AdoptionFailures)},{"queue_truncated",J::Bool(s.QueueTruncated)},
            {"representative_scan_truncated",J::Bool(s.ScanTruncated)},{"output_truncated",J::Bool(emitted<s.Count)},
            {"diagnostic_record_visit_limit",std::to_string(D::VisitsPerTick)},{"retained_attempts",std::to_string(s.Count)},{"work_counter_scope",J::Quote("returned_native_results_generation_cumulative")},{"candidate_records_emitted",std::to_string(emitted)},{"cpu_record_visits",std::to_string(s.CpuVisits)},
            {"native_candidate_audits",std::to_string(s.NativeAudits)}, {"identity_visits",std::to_string(work)},
            {"graph_visits",std::to_string(graphs)},{"audit_visits",std::to_string(audits)},
            {"new_sprites_generated","0"},{"capture_enabled","false"},{"candidates","["+records+"]"}});
    }
    template<class Snapshot> std::string ClassificationJson(const Scheduler<Snapshot>& s,const D::Registry& registry,size_t limit=OutputLimit)
    {
        limit=(std::min)(limit,OutputLimit);
        if(limit<4096)return "{\"stage\":\"R2.6b.4\",\"output_truncated\":true,\"capture_enabled\":false}";
        std::string rows;size_t emitted=0,unknown=0,include=0,exclude=0,invalid=0,visits=0,tags=0,invalidSource=0;
        // Output detail limits are independent of production/input completeness.
        bool truncated=s.QueueTruncated||s.ScanTruncated;
        for(size_t i=0;i<s.Count&&i<D::VisitsPerTick;++i)
        {
            const auto& e=s.Entries[i];if(!s.IsCurrent(e))continue;
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
                {"observed_instance_count",std::to_string(e.Candidate.Instances)},{"processed",J::Bool(e.Done)},{"representative_unavailable",J::Bool(e.MissingRepresentative)},{"recovery_required",J::Bool(e.RecoveryRequired)},
                {"audit_disposition",J::Quote(e.Validation.Disposition==AuditDisposition::Interrupted?"interrupted":e.Done?"completed":"pending")},
                {"recovery_trigger",J::Quote(e.RecoveryRequired?"newer_complete_source_or_explicit_reevaluation":"none")},
                {"observation",MiniMapSpriteClassification::Json(o)}});
            if(rows.size()+row.size()+4096>limit)break;
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
            {"completion_status",J::Quote(s.Generation&&s.ReadyFor(registry)&&s.Complete()?"complete":"partial_or_invalidated")},
            {"classification_summary_scope",J::Quote("bounded_history_prefix_only")},{"inputs_truncated",J::Bool(truncated)},{"output_truncated",J::Bool(emitted<registry.Count||s.Count>D::VisitsPerTick)},
            {"source_registry_complete",J::Bool(registry.Complete())},
            {"source_registry_adopted",J::Bool(s.ReadyFor(registry))},
            {"adoption_pending",J::Bool(s.Adopting)},
            {"adoption_cursor",std::to_string(s.AdoptionCursor)},
            {"adoption_allocation_blocked",J::Bool(s.AdoptionAllocationBlocked)},
            {"adoption_allocation_failures",std::to_string(s.AdoptionFailures)},
            {"observation_refresh",J::Quote("once_per_definition_key_per_world_generation; mutable inputs may change; no trusted policy cached")},
            {"candidates_emitted",std::to_string(emitted)},{"policy_observational_only","true"},
            {"new_sprites_generated","0"},{"capture_enabled","false"},{"candidates","["+rows+"]"}});
    }
}
