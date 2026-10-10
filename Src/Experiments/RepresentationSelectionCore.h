#pragma once
#include "RepresentationMetadataCore.h"
#include <array>
#include <set>
#include <tuple>

namespace MiniMapRepresentationSelection
{
    inline constexpr size_t RecordLimit=4096,DefinitionLimit=16,ProbeLimit=8,InstanceLimit=2,NativeWorkLimit=32768;
    inline constexpr uint32_t PreferredBuilding=336;
    struct Definition
    {
        int32_t ObjectIndex{},NameIndex{};uint32_t NameNumber{};
        auto Key()const{return std::tuple(ObjectIndex,NameIndex,NameNumber);}
        bool operator<(const Definition& b)const{return Key()<b.Key();}
        bool operator==(const Definition& b)const{return Key()==b.Key();}
    };
    struct Record
    {
        Definition Def;uint32_t Building{};int32_t Index{},Serial{};uint64_t Generation{};
        bool Valid{};size_t SnapshotIndex{};
        auto Order()const{return std::tuple(Building!=PreferredBuilding,Building,Def.Key(),uint32_t(Serial),uint32_t(Index));}
    };
    struct Candidate {Definition Def;std::vector<Record> Instances;};
    struct Plan
    {
        size_t SnapshotRecords{},Examined{},ValidRecords{},DistinctDefinitions{};
        bool ScanComplete{},SmelterFound{},DefinitionsTruncated{};
        std::vector<Candidate> Candidates;
    };
    // Only copied CPU metadata is examined. All containers have fixed ceilings;
    // the top definitions are maintained during scanning, never an unbounded sort.
    template<class GetRecord> Plan Scan(size_t total,GetRecord get)
    {
        Plan p;p.SnapshotRecords=total;p.Examined=(std::min)(total,RecordLimit);p.ScanComplete=total<=RecordLimit;
        std::set<Definition> seen;
        for(size_t i=0;i<p.Examined;++i)
        {
            auto r=get(i);r.SnapshotIndex=i;if(!r.Valid)continue;
            ++p.ValidRecords;p.SmelterFound|=r.Building==PreferredBuilding;seen.insert(r.Def);
            auto found=std::find_if(p.Candidates.begin(),p.Candidates.end(),[&](const auto& c){return c.Def==r.Def;});
            if(found==p.Candidates.end())p.Candidates.push_back({r.Def,{r}});
            else
            {
                auto& instances=found->Instances;
                if(std::none_of(instances.begin(),instances.end(),[&](const auto& x){return x.Index==r.Index&&x.Serial==r.Serial;}))instances.push_back(r);
                std::sort(instances.begin(),instances.end(),[](const auto& a,const auto& b){return a.Order()<b.Order();});
                if(instances.size()>InstanceLimit)instances.resize(InstanceLimit);
            }
            std::sort(p.Candidates.begin(),p.Candidates.end(),[](const auto& a,const auto& b){return a.Instances[0].Order()<b.Instances[0].Order();});
            if(p.Candidates.size()>DefinitionLimit){p.Candidates.resize(DefinitionLimit);p.DefinitionsTruncated=true;}
        }
        p.DistinctDefinitions=seen.size();return p;
    }
    enum class Route {PlacementActor=0,ConfigurationActor=1,Mass=2,None=3};
    inline const char* RouteName(Route r)
    {
        switch(r){case Route::PlacementActor:return "placement.ActorClass";case Route::ConfigurationActor:return "configuration_actor";
            case Route::Mass:return "configuration_mass_ism";default:return "none";}
    }
    inline Route EligibleRoute(bool placementActor,bool configurationActor,bool mass)
    {return placementActor?Route::PlacementActor:configurationActor?Route::ConfigurationActor:mass?Route::Mass:Route::None;}
    inline std::string UnavailableReference(const char* reason)
    {return MiniMapRepresentationCore::Object({{"status",MiniMapRepresentationCore::Quote("unavailable")},
        {"reason",MiniMapRepresentationCore::Quote(reason)}});}
    struct Probe {Route EligibleRoute=Route::None;bool Stale=false,WorkExhausted=false;};
    struct Decision
    {
        size_t Probes{},DefinitionsProbed{},Eligible{},Rejected{},SelectedAttempt=SIZE_MAX;
        Record Selected{};Route SelectedRoute=Route::None;
        bool ProbeTruncated=false,WorkTruncated=false,PreferenceSatisfied=false;
        bool HasSelection()const{return SelectedAttempt!=SIZE_MAX;}
    };
    inline int Rank(const Record& r,Route route){return (r.Building==PreferredBuilding?0:3)+int(route);}
    template<class ProbeRecord> Decision Choose(const Plan& plan,ProbeRecord probe,Route bestSupported=Route::PlacementActor)
    {
        Decision d;int best=100;
        for(size_t ci=0;ci<plan.Candidates.size();++ci)
        {
            const auto& candidate=plan.Candidates[ci];
            if(d.HasSelection()&&d.Selected.Building==PreferredBuilding&&candidate.Instances[0].Building!=PreferredBuilding)
            {d.PreferenceSatisfied=true;return d;} // A fallback cannot outrank an eligible Smelter.
            for(size_t instance=0;instance<candidate.Instances.size();++instance)
            {
                if(d.Probes==ProbeLimit){d.ProbeTruncated=true;return d;}
                if(!instance)++d.DefinitionsProbed;
                const auto& r=candidate.Instances[instance];const auto attempt=d.Probes++;
                const auto result=probe(r,attempt);
                if(result.EligibleRoute!=Route::None)
                {
                    ++d.Eligible;const auto rank=Rank(r,result.EligibleRoute);
                    if(rank<best){best=rank;d.SelectedAttempt=attempt;d.Selected=r;d.SelectedRoute=result.EligibleRoute;}
                }
                else ++d.Rejected;
                if(result.WorkExhausted){d.WorkTruncated=true;return d;}
                if(d.HasSelection()&&best==int(bestSupported)+(d.Selected.Building==PreferredBuilding?0:3))
                {d.PreferenceSatisfied=true;return d;}
                // Only a stale Mass identity warrants one alternate instance.
                if(!result.Stale)break;
            }
        }
        return d;
    }
    struct WorkBudget
    {
        size_t Remaining=NativeWorkLimit,SelectionVisits=0,RepresentationVisits=0;bool Exhausted=false;
        bool Spend(bool representation)
        {
            if(!Remaining){Exhausted=true;return false;}--Remaining;
            if(representation)++RepresentationVisits;else ++SelectionVisits;return true;
        }
    };
    inline std::string CoverageJson(const Plan& p,const Decision& d)
    {
        namespace C=MiniMapRepresentationCore;
        return C::Object({{"snapshot_records",std::to_string(p.SnapshotRecords)},{"copied_records_examined",std::to_string(p.Examined)},
            {"copied_scan_complete",C::Bool(p.ScanComplete)},{"definition_valid_records",std::to_string(p.ValidRecords)},
            {"distinct_definitions_seen",std::to_string(p.DistinctDefinitions)},{"candidate_definitions_retained",std::to_string(p.Candidates.size())},
            {"smelter_found_in_examined_records",C::Bool(p.SmelterFound)},
            {"smelter_snapshot_presence",C::Quote(p.SmelterFound?"present":p.ScanComplete?"absent_from_current_spatial_snapshot":"unknown_scan_incomplete")},
            {"native_candidates_probed",std::to_string(d.Probes)},{"eligible_candidates",std::to_string(d.Eligible)},
            {"native_definition_candidates_probed",std::to_string(d.DefinitionsProbed)},
            {"preference_satisfied",MiniMapRepresentationCore::Bool(d.PreferenceSatisfied)},
            {"rejected_attempts",std::to_string(d.Rejected)},
            {"selected_building_id",d.HasSelection()?std::to_string(d.Selected.Building):"null"},
            {"selected_route",d.HasSelection()?C::Quote(RouteName(d.SelectedRoute)):"null"},
            {"outcome",C::Quote(d.HasSelection()?"selected":"unavailable")}});
    }
    inline std::string TruncationJson(const Plan& p,const Decision& d,bool nativeTraversal,bool output)
    {
        namespace C=MiniMapRepresentationCore;
        return C::Object({{"copied_record_limit",C::Bool(!p.ScanComplete)},
            {"definition_candidate_limit",C::Bool(p.DefinitionsTruncated)},
            {"native_probe_limit",C::Bool(d.ProbeTruncated)},
            {"native_traversal_limits",C::Bool(nativeTraversal)},
            {"work_budget_exhausted",C::Bool(d.WorkTruncated)},{"attempt_output_limit",C::Bool(output)}});
    }
    inline std::string BoundDocument(std::string document,size_t limit=MiniMapRepresentationCore::OutputLimit)
    {
        if(document.size()<=limit)return document;
        const std::string fallback="{\"schema_version\":2,\"stage\":\"R2.4b\",\"status\":\"truncated\",\"reason\":\"output_byte_limit\",\"selection\":{\"outcome\":\"unavailable_output_truncated\"},\"representation\":{\"status\":\"truncated\"},\"complete_visual_building\":false}";
        return fallback.size()<=limit?fallback:"{}";
    }
}
