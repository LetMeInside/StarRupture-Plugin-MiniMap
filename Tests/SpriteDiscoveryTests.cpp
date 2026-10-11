#include "../Src/Experiments/SpriteDiscoveryCore.h"
#include <cassert>
#include <fstream>
#include <iostream>
namespace C=MiniMapSpriteDiscoveryCore;
C::Input Item(int n,uint64_t generation=1)
{
    C::Input r;r.DefinitionValid=true;r.Identity.Generation=generation;
    r.Identity.Building=n;r.Identity.DefinitionObject=n;r.Identity.NameIndex=n;
    return r;
}
void Finish(C::Registry& r,const std::vector<C::Input>& v)
{
    size_t ticks=0;while(!r.Finished){r.Step([&](size_t i){return v[i];});
        assert(r.LastVisits<=128&&r.LastNew<=16);assert(++ticks<10000);}
}
int main(int argc,char** argv)
{
    struct Snapshot {uint64_t Generation=1;bool Complete=true;double PublishedAtSeconds=1;std::vector<C::Input> Records;};
    auto source=std::make_shared<Snapshot>();source->Records.assign(300,Item(1));
    C::Session<Snapshot> session;auto get=[](const C::Input& r){return r;};
    assert(session.Service(source,true,1,get)==C::Publication::None);
    auto replacement=std::make_shared<Snapshot>();replacement->Records={Item(2)};replacement->PublishedAtSeconds=2;
    assert(session.Service(replacement,true,2,get)==C::Publication::None);
    assert(session.Held.get()==source.get()); // same-generation publisher replacement cannot corrupt the cursor
    assert(session.Service(replacement,true,3,get)==C::Publication::Ready);
    assert(session.Published.RepresentedInstances==300&&!session.Held);
    assert(session.Service(replacement,true,4,get)==C::Publication::None); // bounded publication cadence
    assert(session.Service(replacement,true,13,get)==C::Publication::Ready);
    assert(session.Published.Count==1&&session.Published.Entries[0].Identity.Building==2);
    session.Invalidate(1);assert(session.Service(replacement,true,14,get)==C::Publication::Invalidated);
    assert(session.Service(replacement,true,15,get)==C::Publication::None&&session.Published.Count==0);
    replacement->Generation=2;replacement->Records={Item(3,2)};
    assert(session.Service(replacement,true,16,get)==C::Publication::Ready);
    assert(session.Published.Generation==2&&session.Published.Count==1);
    assert(session.Service(replacement,false,17,get)==C::Publication::Invalidated);
    assert(!session.Held&&session.Published.Count==0);
    source=std::make_shared<Snapshot>();source->Records.assign(300,Item(1));
    session.Service(source,true,18,get); // interrupt a partial scan with a new generation
    assert(session.Service(replacement,true,19,get)==C::Publication::Ready);
    assert(session.Published.RepresentedInstances==1&&session.Published.Generation==2);
    source=std::make_shared<Snapshot>();source->Records.assign(C::RecordLimit+1,Item(1));
    assert(session.Service(source,true,20,get)==C::Publication::Ready);
    assert(!session.Held&&session.Published.Cursor==128&&session.Published.RecordTruncated&&!session.Published.Complete());
    C::Registry r;std::vector<C::Input> v(200,Item(1));v[150]=Item(2);
    r.Begin(1,v.size(),true);r.Step([&](size_t i){return v[i];});assert(r.Cursor==128&&!r.Finished);
    Finish(r,v);assert(r.Complete()&&r.Count==2&&r.Entries[0].Instances==199&&r.Entries[1].Instances==1);
    for(int i=0;i<10;++i)r.Step([&](size_t j){return v[j];});assert(r.RepresentedInstances==200); // no accumulation
    v.erase(v.begin(),v.begin()+100);r.Begin(1,v.size(),true);Finish(r,v);assert(r.RepresentedInstances==100);
    v.push_back(Item(3));r.Begin(1,v.size(),true);Finish(r,v);assert(r.Count==3&&r.RepresentedInstances==101);
    v={Item(1),Item(1),Item(1)};v[1].Identity.Representation=C::Tier::High;v[2].Identity.VisualClass=9;
    r.Begin(1,v.size(),true);Finish(r,v);assert(r.Count==3); // tiers/copied visual state do not merge
    assert(r.Entries[0].Status==C::State::NeedsValidation);assert(r.Entries[2].CopiedGeometry==false);
    v={Item(1),Item(1)};C::CopyTint(v[1],{0.5f,1,1,1});r.Begin(1,2,true);Finish(r,v);assert(r.Count==2);
    C::Input negativeZero=Item(1),positiveZero=Item(1);
    C::CopyTint(negativeZero,{-0.0f,1,1,1});C::CopyTint(positiveZero,{0.0f,1,1,1});assert(negativeZero.Identity==positiveZero.Identity);
    v={Item(1)};assert(!C::CopyTint(v[0],{INFINITY,1,1,1}));r.Begin(1,1,true);Finish(r,v);assert(r.InvalidRecords==1&&r.Count==0);
    v={Item(1)};v[0].Identity.Spline=true;r.Begin(1,1,true);Finish(r,v);
    assert(r.Entries[0].Status==C::State::Unsupported);
    v={Item(1),Item(2,2)};r.Begin(2,2,true);Finish(r,v);assert(r.InvalidRecords==1&&r.Count==1&&!r.Complete());
    v={Item(1)};v[0].DefinitionValid=false;r.Begin(1,1,true);Finish(r,v);assert(r.Count==0&&r.InvalidRecords==1);
    v.clear();for(int i=0;i<300;++i)v.push_back(Item(i));
    r.Begin(1,v.size(),true);r.Step([&](size_t i){return v[i];});assert(r.LastNew==16&&r.Cursor==16);
    Finish(r,v);assert(r.Count==256&&r.CapacitySkipped==44&&!r.Complete());
    auto partial=C::Json(r,5000);assert(partial.size()<=5000&&partial.find("\"output_truncated\":true")!=std::string::npos);
    auto full=C::Json(r);assert(full.size()<=C::OutputLimit);
    if(argc>1){std::ofstream(std::string(argv[1])+".full.json")<<full;std::ofstream(std::string(argv[1])+".partial.json")<<partial;}
    r.Begin(1,C::RecordLimit+1,true);while(!r.Finished)r.Step([](size_t){return Item(1);});
    assert(r.Cursor==C::RecordLimit&&r.RecordTruncated&&r.RepresentedInstances==C::RecordLimit);
    r.Begin(1,1,false);r.Step([](size_t){return Item(1);});assert(!r.Complete());
    assert(C::Json(r).find("provisional_definition_group_variants_unobserved")!=std::string::npos);
    r.Begin(2,0,true);r.Step([](size_t){return Item(1);});assert(r.Count==0&&r.Complete());
    assert(C::Json(r,C::OutputLimit,true).find("\"lifecycle\":\"Invalidated\"")!=std::string::npos);
    std::cout<<"Sprite discovery: grouping, provisional identity, budgets, cursor, reconciliation, invalid metadata and bounded JSON passed\n";
}
