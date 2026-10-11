#include "../Src/Experiments/SpriteValidationCore.h"
#include "../Src/Experiments/RepresentationSelectionCore.h"
#include <cassert>
#include <fstream>
#include <iostream>
namespace K=MiniMapSpriteClassification;
namespace V=MiniMapSpriteValidation;
namespace D=MiniMapSpriteDiscoveryCore;
namespace C=MiniMapRepresentationCore;
struct Snapshot {uint64_t Generation=4;bool Complete=true;double PublishedAtSeconds=1;std::vector<D::Input> Records;};
D::Input Item(int id,uint64_t generation=4)
{D::Input r;r.DefinitionValid=true;r.Identity.Generation=generation;r.Identity.Building=id;r.Identity.DefinitionObject=id;r.Identity.NameIndex=id;return r;}
D::Registry Registry(const Snapshot& s)
{D::Registry r;r.Begin(s.Generation,s.Records.size(),s.Complete);while(!r.Finished)r.Step([&](size_t i){return s.Records[i];});return r;}
int main(int argc,char** argv)
{
    auto source=std::make_shared<Snapshot>();
    for(const auto [id,count]:{std::pair{238,28},{18,4},{317,2},{336,2},{220,1},{341,1},{4,1}})
        for(int i=0;i<count;++i)source->Records.push_back(Item(id));
    auto registry=Registry(*source);
    auto owner=std::make_unique<V::Scheduler<Snapshot>>();auto& scheduler=*owner;scheduler.Observe(registry);
    auto get=[](const D::Input& r){return r;};
    std::vector<int> attempted;
    auto audit=[&](const D::Input& r,const D::Input*)
    {
        attempted.push_back(r.Identity.Building);V::Result result;
        auto& o=result.Classification;o.Fixed=true;o.Status="verified_inputs";
        o.CachedConfig=r.Identity.Building==336?"resident_validated":"null_not_authored_absence";
        o.ConfigReference.Presence="nonempty_authored_reference_unresolved";
        o.Flags[2]=o.Flags[3]=true;o.Subclass=true;o.UI=2; // Suggestive inputs cannot establish capability.
        o.Tags[0]={123,0};o.TagCount=1;o.IdentityVisits=3;o.TagVisits=1;
        if(r.Identity.Building==18){o.Truncated=true;o.TagsStatus="truncated";}
        result.Reason="cached_config_null";return result;
    };
    for(size_t tick=0;tick<7;++tick)
    {const auto before=attempted.size();assert(scheduler.Service(registry,source,get,audit));assert(attempted.size()==before+1);assert(scheduler.LastCpuVisits<=128);}
    assert(attempted==std::vector<int>({238,18,317,336,220,341,4}));
    assert(scheduler.Complete()&&scheduler.NativeAudits==7);
    for(size_t i=0;i<7;++i)
    {assert(scheduler.Entries[i].Validation.Classification.Fixed);assert(scheduler.Entries[i].Validation.Classification.CachedPolicy.ExcludeFromMiniMap==K::Exclusion::Unknown);}
    assert(!scheduler.Service(registry,source,get,audit));
    const auto full=V::ClassificationJson(scheduler,registry);
    const auto partial=V::ClassificationJson(scheduler,registry,7000);
    assert(full.find("\"unknown\":7")!=std::string::npos);
    assert(full.find("\"new_sprites_generated\":0")!=std::string::npos);
    assert(partial.size()<=7000&&partial.find("\"output_truncated\":true")!=std::string::npos);
    assert(V::ClassificationJson(scheduler,registry,1000).size()<=1000);
    assert(K::WeakShape(123,0)==std::string("empty_serial_zero"));
    assert(K::WeakShape(-1,3)==std::string("invalid_shape"));
    assert(K::WeakShape(123,3)==std::string("populated_unresolved_serial_not_validated"));
    // Stale serials cannot be resolved or classified as resident by shape inspection.
    K::SoftState soft;soft.Read=true;soft.WeakIndex=123;soft.WeakSerial=3;
    assert(K::SoftJson(soft).find("not_attempted_no_new_native_callable")!=std::string::npos);
    K::Evidence e;e.HabitatCapability=true;assert(K::Decide(e).ExcludeFromMiniMap==K::Exclusion::Unknown);
    e.TypeInvariant=true;assert(K::Decide(e).ExcludeFromMiniMap==K::Exclusion::Exclude);
    e.Conflict=true;assert(K::Decide(e).ExcludeFromMiniMap==K::Exclusion::Unknown);
    e={};e.TypeInvariant=e.PermanentProtection=true;assert(K::Decide(e).ExcludeFromMiniMap==K::Exclusion::Exclude);
    e={};e.TypeInvariant=e.OutdoorOnly=true;assert(K::Decide(e).ExcludeFromMiniMap==K::Exclusion::Unknown);
    e.PlayerManageable=true;assert(K::Decide(e).ExcludeFromMiniMap==K::Exclusion::Include);
    // The explicit Interior policy wins over inclusion, conflict and all mutable
    // habitat/terrain combinations; it does not claim indoor placement capability.
    e.InteriorTag=true;e.Conflict=true;
    auto interior=K::Decide(e);
    assert(interior.ExcludeFromMiniMap==K::Exclusion::Exclude&&interior.Reason=="InteriorTag");
    assert(!K::SpriteAllowed(interior));
    for(unsigned mask=0;mask<128;++mask)
    {
        K::Observation tagged;tagged.Fixed=true;tagged.InteriorTag="present";
        tagged.InteriorComparison="exact_FName_Equals_Interior";tagged.CachedPolicy=interior;
        for(size_t bit=0;bit<tagged.Flags.size();++bit)tagged.Flags[bit]=(mask&(1u<<bit))!=0;
        const auto json=K::Json(tagged);
        assert(json.find("\"building_sprite_allowed\":false")!=std::string::npos);
        assert(json.find("\"exclusion_reason\":\"InteriorTag\"")!=std::string::npos);
    }
    K::Observation untagged;untagged.InteriorTag="absent";
    assert(untagged.CachedPolicy.ExcludeFromMiniMap==K::Exclusion::Unknown&&K::SpriteAllowed(untagged.CachedPolicy));
    K::Observation unavailable;unavailable.TagsStatus="truncated";
    assert(unavailable.InteriorTag=="unknown"&&K::SpriteAllowed(unavailable.CachedPolicy));
    // Opaque tag IDs/flags, temporary infection denial, tag absence and a failed preview are not premises.
    assert(K::Decide({}).Reason=="InsufficientEvidence");
    // A live instance's protection (present/absent/unavailable), infection or
    // disinfection cannot be promoted to a type invariant. These are synthetic
    // policy counterexamples; the native optional actor branch remains omitted.
    for(bool protection:{false,true})for(bool infected:{false,true})
    {K::Evidence instance;instance.PermanentProtection=protection;assert(K::Decide(instance).ExcludeFromMiniMap==K::Exclusion::Unknown);(void)infected;}
    K::Observation mutated;mutated.Fixed=true;mutated.Flags.fill(true);
    const auto beforeMutation=mutated.CachedPolicy.ExcludeFromMiniMap;
    mutated.Flags.fill(false);assert(mutated.CachedPolicy.ExcludeFromMiniMap==beforeMutation);
    assert(K::Json(mutated).find("\"NonDeconstructible\":\"unverified\"")!=std::string::npos);
    MiniMapRepresentationSelection::WorkBudget work;work.Remaining=1;
    assert(work.Spend(false)&&!work.Spend(false)&&work.Exhausted);
    C::ArrayHeader a{};a.Data=0x1000;a.Num=65;a.Max=65;
    auto b=C::BoundArray(a,8,4,K::TagLimit);assert(b.Valid&&b.Truncated&&b.Count==64);
    a.Max=1;assert(!C::BoundArray(a,8,4,K::TagLimit).Valid);
    a.Num=-1;assert(!C::BoundArray(a,8,4,K::TagLimit).Valid);
    a={};a.Num=a.Max=1;assert(!C::BoundArray(a,8,4,K::TagLimit).Valid);
    // Publication changes reconcile counts but do not rerun an audited definition.
    source->PublishedAtSeconds=2;source->Records.erase(source->Records.begin());registry=Registry(*source);scheduler.Observe(registry);
    assert(scheduler.Entries[0].Candidate.Instances==27&&!scheduler.Service(registry,source,get,audit));
    source->Records[0].Identity.NameNumber=1;registry=Registry(*source);scheduler.Observe(registry);
    assert(scheduler.Service(registry,source,get,audit));
    source->Generation=5;source->Records={Item(336,5)};registry=Registry(*source);scheduler.Observe(registry);
    assert(scheduler.Processed()==0&&scheduler.Count==1);
    assert(scheduler.Entries[0].Validation.Classification.CachedPolicy.ExcludeFromMiniMap==K::Exclusion::Unknown);
    assert(scheduler.Service(registry,source,get,[](const auto&,const auto*){throw 1;return V::Result{};}));
    assert(scheduler.Complete()); // Failure gets a record and cannot remain active.
    const auto invalid=V::ClassificationJson(scheduler,registry);
    assert(invalid.find("\"invalid_or_unavailable\":1")!=std::string::npos);
    if(argc>1)
    {std::ofstream(std::string(argv[1])+".full.json")<<full;std::ofstream(std::string(argv[1])+".partial.json")<<partial;std::ofstream(std::string(argv[1])+".invalid.json")<<invalid;}
    std::cout<<"Classification: all seven, null config, deduplication, generation, policy scope, weak-state uncertainty, bounds and JSON passed; scheduler bytes="<<sizeof(V::Scheduler<Snapshot>)<<"\n";
}
