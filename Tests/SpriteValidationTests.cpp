#include "../Src/Experiments/SpriteValidationCore.h"
#include "../Src/Experiments/AssemblyRecipeCore.h"
#include <cassert>
#include <fstream>
#include <iostream>
namespace V=MiniMapSpriteValidation;
namespace D=MiniMapSpriteDiscoveryCore;
namespace A=MiniMapAssemblyRecipe;
struct Snapshot {uint64_t Generation=4;bool Complete=true;double PublishedAtSeconds=1;std::vector<D::Input> Records;};
D::Input Item(int id,uint64_t generation=4)
{D::Input r;r.DefinitionValid=true;r.Identity.Generation=generation;r.Identity.Building=id;r.Identity.DefinitionObject=id;r.Identity.NameIndex=id;return r;}
D::Registry Registry(const Snapshot& s)
{D::Registry r;r.Begin(s.Generation,s.Records.size(),s.Complete);while(!r.Finished)r.Step([&](size_t i){return s.Records[i];});return r;}
V::Result Eligible(size_t count=1)
{V::Result r;r.Resident=r.Root=r.Transforms=r.Materials=true;r.Static=count;r.UniqueMeshes=1;r.Route="high_detail_actor";r.Status=V::Classify(r,false);return r;}
int main(int argc,char** argv)
{
    auto source=std::make_shared<Snapshot>();
    for(const auto [id,count]:{std::pair{220,1},{317,2},{341,1},{4,1},{18,4},{238,28},{336,2}})
        for(int i=0;i<count;++i)source->Records.push_back(Item(id));
    auto registry=Registry(*source);assert(registry.Count==7&&registry.RepresentedInstances==39);
    auto schedulerOwner=std::make_unique<V::Scheduler<Snapshot>>();auto& scheduler=*schedulerOwner;
    scheduler.Observe(registry);std::vector<int> order;
    auto get=[](const D::Input& r){return r;};
    auto audit=[&](const D::Input& r,const D::Input* alternate)
    {
        order.push_back(r.Identity.Building);
        if(r.Identity.Building==220)throw 7; // Failure must not stop the other six.
        auto result=Eligible();
        if(r.Identity.Building==317){assert(alternate);result.Root=false;}
        if(r.Identity.Building==341){result.Resident=false;result.Config="null";result.Reason="cached_config_null; authored_reference_not_examined";}
        if(r.Identity.Building==4)result.Static=0;
        if(r.Identity.Building==18){result.Route="mass_ism";result.Skeletal=1;}
        if(r.Identity.Building==238){result.Static=2;result.UniqueMeshes=2;}
        if(r.Identity.Building==336){if(order.size()<=7)assert(alternate);result.Static=10;result.UniqueMeshes=7;result.Skeletal=1;result.Nodes=15;}
        result.Status=V::Classify(result,r.Identity.Spline);return result;
    };
    for(size_t tick=0;tick<7;++tick)
    {const auto before=order.size();assert(scheduler.Service(registry,source,get,audit));assert(order.size()==before+1);assert(scheduler.LastCpuVisits<=128);}
    assert((order==std::vector<int>{220,317,341,4,18,238,336}));
    assert(scheduler.Complete()&&scheduler.Processed()==7&&scheduler.NativeAudits==7);
    assert(scheduler.Entries[0].Validation.Status==V::Outcome::Invalid);
    assert(scheduler.Entries[1].Validation.Status==V::Outcome::Conditional);
    assert(scheduler.Entries[2].Validation.Status==V::Outcome::NoResident);
    assert(scheduler.Entries[3].Validation.Status==V::Outcome::Unsupported);
    assert(scheduler.Entries[5].Validation.Status==V::Outcome::Eligible);
    assert(scheduler.Entries[6].Validation.Static==10&&scheduler.Entries[6].Validation.Skeletal==1);
    scheduler.Observe(registry);assert(!scheduler.Service(registry,source,get,audit)); // no repeat audits/count accumulation
    const auto json=V::Json(scheduler,registry);
    assert(json.find("\"classification\":\"unknown\"")!=std::string::npos);
    assert(json.find("\"human_readable_name\":null")!=std::string::npos);
    assert(json.find("\"capture_enabled\":false")!=std::string::npos);
    const auto partial=V::Json(scheduler,registry,5000);assert(partial.size()<=5000);
    assert(partial.find("\"output_truncated\":true")!=std::string::npos);
    if(argc>1){std::ofstream(std::string(argv[1])+".full.json")<<json;std::ofstream(std::string(argv[1])+".partial.json")<<partial;}

    // Classification is independent of semantic name and never certifies live fidelity.
    auto r=Eligible(6);assert(V::Classify(r,false)==V::Outcome::Eligible);
    for(auto member:{&V::Result::Root,&V::Result::Transforms,&V::Result::Materials})
    {r=Eligible();r.*member=false;assert(V::Classify(r,false)==V::Outcome::Conditional);}
    for(auto member:{&V::Result::Truncated,&V::Result::Inherited,&V::Result::Child,&V::Result::OtherSkinned,&V::Result::DerivedStatic})
    {r=Eligible();r.*member=true;assert(V::Classify(r,false)==V::Outcome::Conditional);}
    assert(V::Classify(Eligible(),true)==V::Outcome::Unsupported);
    r=Eligible();r.Static=0;r.Skeletal=1;assert(V::Classify(r,false)==V::Outcome::Unsupported);
    auto materials=A::ProposedMaterials({{0,"authored",true},{1,"second",true}},{{0,"override",true}});
    assert(materials.size()==2&&materials[0].Material=="override"&&materials[1].Material=="second");

    // Partial CPU scan keeps an immutable snapshot despite publisher replacement.
    auto many=std::make_shared<Snapshot>();many->Records.assign(300,Item(238));many->Records.push_back(Item(336));
    auto mr=Registry(*many);auto cursorOwner=std::make_unique<V::Scheduler<Snapshot>>();auto& cursor=*cursorOwner;cursor.Observe(mr);
    assert(!cursor.Service(mr,many,get,audit)&&cursor.Cursor==128);
    auto replacement=std::make_shared<Snapshot>(*many);replacement->PublishedAtSeconds=2;replacement->Records={Item(238)};
    assert(!cursor.Service(mr,replacement,get,audit)&&cursor.Held.get()==many.get());
    assert(cursor.Service(mr,replacement,get,audit)&&cursor.LastCpuVisits==45);
    assert(cursor.Service(mr,replacement,get,audit)&&cursor.Complete());
    auto rr=Registry(*replacement);cursor.Observe(rr);assert(cursor.Processed()==1&&cursor.Complete());
    replacement->Records.push_back(Item(238));rr=Registry(*replacement);cursor.Observe(rr);
    assert(cursor.Entries[0].Candidate.Instances==2&&cursor.Processed()==1);
    replacement->Records.push_back(Item(999));rr=Registry(*replacement);cursor.Observe(rr);
    assert(cursor.Service(rr,replacement,get,[](const auto&,const auto*){return Eligible();}));assert(cursor.Complete());
    replacement->Records.back().Identity.VisualClass=123;rr=Registry(*replacement);cursor.Observe(rr);
    assert(!cursor.Complete());assert(cursor.Service(rr,replacement,get,[](const auto&,const auto*){return Eligible();}));
    replacement->Generation=5;replacement->Records={Item(336,5)};rr=Registry(*replacement);cursor.Observe(rr);
    assert(cursor.Count==1&&cursor.Processed()==0&&cursor.Generation==5&&!cursor.Held);
    assert(!cursor.Service(rr,source,get,audit)); // old generation rejected
    replacement->Complete=false;assert(!cursor.Service(rr,replacement,get,audit));
    replacement->Complete=true;assert(cursor.Service(rr,replacement,get,[](const auto&,const auto*){return Eligible();}));
    D::Input invalid=Item(777,5);invalid.DefinitionValid=false;replacement->Records.push_back(invalid);
    rr=Registry(*replacement);assert(!rr.Complete());cursor.Observe(rr);assert(!cursor.Service(rr,replacement,get,audit));
    // Ineligible/truncated discovery cannot initiate native work. Oversized source never held indefinitely.
    auto huge=std::make_shared<Snapshot>();huge->Records.assign(D::RecordLimit+1,Item(336));
    auto boundedOwner=std::make_unique<V::Scheduler<Snapshot>>();auto& bounded=*boundedOwner;
    auto one=std::make_shared<Snapshot>();one->Records={Item(336)};auto br=Registry(*one);bounded.Observe(br);
    assert(bounded.Service(br,huge,get,[](const auto&,const auto*){return Eligible();}));
    assert(bounded.ScanTruncated&&!bounded.Held&&bounded.LastCpuVisits==128);
    // A candidate removed from the source receives an explicit failure and ends.
    auto missingOwner=std::make_unique<V::Scheduler<Snapshot>>();auto& missing=*missingOwner;missing.Observe(br);
    auto empty=std::make_shared<Snapshot>();
    assert(missing.Service(br,empty,get,audit)&&missing.Complete());
    assert(missing.Entries[0].Validation.Reason=="no_current_matching_instance"&&missing.NativeAudits==0);
    // Bounded same-generation history cannot grow under candidate churn.
    auto capacityOwner=std::make_unique<V::Scheduler<Snapshot>>();auto& capacity=*capacityOwner;D::Registry cr;
    auto cs=std::make_shared<Snapshot>();for(int i=0;i<256;++i)cs->Records.push_back(Item(1000+i));
    cr=Registry(*cs);capacity.Observe(cr);assert(capacity.Count==256);
    cs->Records={Item(9999)};cr=Registry(*cs);capacity.Observe(cr);
    assert(capacity.Count==256&&capacity.QueueTruncated&&capacity.QueueDeferred==1&&!capacity.Complete());
    V::Attempt escaped;escaped.Validation.Reason="quote\" newline\n backslash\\";escaped.Validation.Details=std::string(V::DetailLimit+1,'x');
    const auto escapedJson=V::CandidateJson(escaped);
    assert(escapedJson.find("quote\\\" newline\\u000a backslash\\\\")!=std::string::npos);
    assert(escapedJson.find("\"authored_audit\":{\"truncated\":true}")!=std::string::npos);
    std::cout<<"Seven-candidate validation: deterministic order, failure isolation, deduplication, generation, budgets, classifications and JSON passed\n";
}
