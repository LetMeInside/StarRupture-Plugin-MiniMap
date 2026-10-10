#include "../Src/Experiments/RepresentationSelectionCore.h"
#include <cassert>
#include <fstream>
#include <iostream>

namespace S=MiniMapRepresentationSelection;
namespace C=MiniMapRepresentationCore;
S::Record Record(int definition,uint32_t building,int instance)
{return {{definition,definition+100,0},building,instance,1,9,true,0};}
S::Plan Scan(const std::vector<S::Record>& records)
{return S::Scan(records.size(),[&](size_t i){return records[i];});}

int main(int argc,char** argv)
{
    // Smelter beyond the old first-64 window and after the retained-definition
    // pool fills must displace a fallback without sorting the entire snapshot.
    std::vector<S::Record> records;
    for(int i=0;i<100;++i)records.push_back(Record(i+1,uint32_t(i+1),i+1));
    records.push_back(Record(500,336,900));
    auto plan=Scan(records);
    assert(plan.Examined==101&&plan.ScanComplete&&plan.SmelterFound);
    assert(plan.DistinctDefinitions==101&&plan.Candidates.size()==16&&plan.DefinitionsTruncated);
    assert(plan.Candidates[0].Instances[0].Building==336);
    auto result=S::Choose(plan,[](const auto&,size_t){return S::Probe{S::Route::ConfigurationActor};});
    assert(result.HasSelection()&&result.Selected.Building==336&&result.Probes==1&&result.PreferenceSatisfied&&!result.ProbeTruncated);
    auto optimal=S::Choose(plan,[](const auto&,size_t){return S::Probe{S::Route::ConfigurationActor};},S::Route::ConfigurationActor);
    assert(optimal.Selected.Building==336&&optimal.Probes==1&&optimal.PreferenceSatisfied&&!optimal.ProbeTruncated);

    // Deterministic definition and instance order does not depend on incoming
    // record ordering or raw native addresses.
    std::reverse(records.begin(),records.end());auto reverse=Scan(records);
    for(size_t i=0;i<plan.Candidates.size();++i)
        assert(plan.Candidates[i].Def==reverse.Candidates[i].Def);

    records={Record(8,7,5),Record(8,7,3),Record(8,7,4),Record(9,7,6)};
    plan=Scan(records);
    assert(plan.DistinctDefinitions==2&&plan.Candidates.size()==2);
    assert(plan.Candidates[0].Instances.size()==2&&plan.Candidates[0].Instances[0].Index==3&&plan.Candidates[0].Instances[1].Index==4);
    size_t stale=0;
    result=S::Choose(plan,[&](const auto& r,size_t)
    {
        if(r.Index==3){++stale;return S::Probe{S::Route::None,true};}
        return S::Probe{S::Route::Mass};
    });
    assert(stale==1&&result.Probes==3&&result.DefinitionsProbed==2&&result.Selected.Index==4);
    size_t notStaleAttempts=0;
    result=S::Choose(plan,[&](const auto&,size_t){++notStaleAttempts;return S::Probe{};});
    assert(notStaleAttempts==2&&!result.HasSelection()); // No alternate for configuration absence.

    // Every requested preference group, including policy support for an optional
    // resident placement actor route (runtime access remains explicitly omitted).
    records={Record(1,1,1),Record(2,336,2),Record(3,336,3)};plan=Scan(records);
    result=S::Choose(plan,[](const auto& r,size_t)
    {return S::Probe{r.Def.ObjectIndex==2?S::Route::Mass:S::Route::PlacementActor};});
    assert(result.Selected.Def.ObjectIndex==3&&result.SelectedRoute==S::Route::PlacementActor);
    records={Record(1,1,1),Record(2,336,2)};plan=Scan(records);
    result=S::Choose(plan,[](const auto& r,size_t)
    {return S::Probe{r.Building==336?S::Route::Mass:S::Route::PlacementActor};});
    assert(result.Selected.Building==336&&result.SelectedRoute==S::Route::Mass);
    records={Record(1,1,1),Record(2,2,2),Record(3,3,3)};plan=Scan(records);
    result=S::Choose(plan,[](const auto& r,size_t)
    {return S::Probe{r.Building==1?S::Route::Mass:r.Building==2?S::Route::ConfigurationActor:S::Route::PlacementActor};});
    assert(result.Selected.Building==3);
    assert(S::EligibleRoute(false,false,true)==S::Route::Mass);
    assert(S::EligibleRoute(false,true,true)==S::Route::ConfigurationActor);
    assert(S::EligibleRoute(false,false,false)==S::Route::None);

    // A null transient cache does not establish authored-reference absence.
    const auto unknown=S::UnavailableReference("soft-reference state/read contract not verified");
    const auto nullConfig=C::Object({{"cached_entity_config",C::Quote("null")},{"authored_entity_config",unknown}});
    assert(nullConfig.find("unavailable")!=std::string::npos&&nullConfig.find("absent")==std::string::npos);

    size_t reads=0;
    plan=S::Scan(S::RecordLimit+1,[&](size_t i){++reads;return Record(int(i)+1,7,int(i)+1);});
    assert(reads==S::RecordLimit&&!plan.ScanComplete&&!plan.SmelterFound);
    // No access to the element after the CPU cap, even if it is Smelter.
    assert(S::CoverageJson(plan,{}).find("unknown_scan_incomplete")!=std::string::npos);
    result=S::Choose(plan,[](const auto&,size_t){return S::Probe{};});
    assert(result.Probes==8&&result.ProbeTruncated&&!result.HasSelection());

    S::WorkBudget budget;budget.Remaining=2;
    records={Record(1,1,1),Record(2,2,2),Record(3,3,3)};plan=Scan(records);
    result=S::Choose(plan,[&](const auto&,size_t)
    {return S::Probe{S::Route::None,false,!budget.Spend(false)};});
    assert(result.WorkTruncated&&result.Probes==3&&budget.SelectionVisits==2);
    assert(!budget.Spend(true)&&budget.RepresentationVisits==0);
    S::WorkBudget shared;assert(shared.Spend(false)&&shared.Spend(true));
    assert(shared.SelectionVisits==1&&shared.RepresentationVisits==1&&shared.Remaining==S::NativeWorkLimit-2);

    plan=Scan({Record(1,7,1)});result=S::Choose(plan,[](const auto&,size_t){return S::Probe{};});
    assert(!result.HasSelection()&&result.Probes==1&&result.Rejected==1);
    const auto document=C::Object({{"schema_version","2"},{"stage",C::Quote("R2.4b")},
        {"selection",S::CoverageJson(plan,result)},{"null_cached_configuration",nullConfig},
        {"selection_truncation",S::TruncationJson(plan,result,false,false)},
        {"representation_truncation",C::Object({{"truncated","false"}})},
        {"components","[]"},{"selected","null"}});
    assert(document.find("\"selected_building_id\":null")!=std::string::npos);
    auto capped=plan;capped.ScanComplete=false;auto limited=result;limited.ProbeTruncated=true;
    const auto finalDocument=C::Object({{"no_selection",document},{"limited_selection",S::TruncationJson(capped,limited,true,false)},
        {"overflow",S::BoundDocument(std::string(C::OutputLimit+1,'x'))}});
    assert(S::BoundDocument("1234",1)=="{}");
    if(argc>1){std::ofstream out(argv[1],std::ios::binary);out<<finalDocument;out.close();assert(out);}
    std::cout<<"Representation selection: coverage, preference, ordering, deduplication, stale alternate, eligibility, budgets and JSON tests passed\n";
}
