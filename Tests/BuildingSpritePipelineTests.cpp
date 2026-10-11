#include "../Src/Experiments/BuildingSpritePipelineCore.h"
#include "../Src/Experiments/CaptureRetirementCore.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <unordered_map>
namespace P=MiniMapBuildingSpriteCore;namespace C=MiniMapSpriteCapture;namespace S=MiniMapStaticAssembly;
static MiniMapSpriteDiscoveryCore::Key Key(int n){MiniMapSpriteDiscoveryCore::Key k;k.Generation=4;k.Building=n;k.DefinitionObject=n+1;k.NameIndex=n+2;return k;}
static S::Recipe Recipe(size_t count)
{
    S::Recipe r;r.General=true;r.Count=count;r.RootVerified=r.Complete=true;r.Generation=4;r.ActorClass=7;
    for(size_t i=0;i<count&&i<S::Limit;++i){auto& x=r.Instances[i];x.Mesh=100+i%3;x.ComponentIndex=uint32_t(i);
        x.Visible=x.Verified=true;x.Hidden=false;x.MaterialCount=1;x.Materials[0]=200+i;
        x.LocalBounds.Extent={1,2,3};x.Transform.T={double(i*10),0,0};}
    return r;
}
struct FenceMock
{
    bool Done=false;size_t Constructed{},Destroyed{};
    bool Available(){return true;}bool Construct(void*){++Constructed;return true;}
    bool Begin(void*){return true;}bool Poll(void*){return Done;}void Destroy(void*){++Destroyed;}
};
int main(int argc,char** argv)
{
    P::Queue q;q.Reset(4);
    std::vector<uint8_t> transparent(256*256*4,0);transparent[0]=255;
    assert(!P::HasCoverage(transparent));transparent[3]=1;assert(P::HasCoverage(transparent));
    transparent.pop_back();assert(!P::HasCoverage(transparent));
    for(int id:{238,18,317,336,220,341,4}){assert(q.Observe(Key(id)));assert(q.Observe(Key(id)));}
    assert(q.Count==7);assert(!q.Observe(MiniMapSpriteDiscoveryCore::Key{}));
    q.Set(Key(18),P::State::Excluded,"InteriorTag");assert(!q.QueueCapture(Key(18)));
    assert(q.QueueCapture(Key(238)));assert(!q.QueueCapture(Key(238)));
    assert(!q.Set(Key(238),P::State::Awaiting,"repeated_snapshot"));
    q.Set(Key(238),P::State::Failed,"failure");assert(!q.QueueCapture(Key(238)));
    assert(q.QueueCapture(Key(336)));q.Set(Key(336),P::State::Ready,"reference");
    assert(!q.Set(Key(336),P::State::Failed,"cleanup"));assert(q.Find(Key(336))->Status==P::State::Ready);
    q.Set(Key(336),P::State::Excluded,"InteriorTag");assert(q.Find(Key(336))->Status==P::State::Excluded);
    const auto diagnostic=P::Json(q);assert(diagnostic.find("InteriorTag")!=std::string::npos);
    assert(diagnostic.find("actual draw submissions")!=std::string::npos);
    const auto timed=P::Json(q,62881,62881,"idle","{\"widget_cpu\":{\"max\":0.03}}");
    assert(timed.find("\"timings_ms\":{\"widget_cpu\"")!=std::string::npos);
    q.Reset(5);assert(q.Count==0&&!q.Observe(Key(336)));q.Reset(4);
    for(size_t i=0;i<MiniMapSpriteDiscoveryCore::Capacity;++i)assert(q.Observe(Key(int(i))));
    assert(!q.Observe(Key(1000)));
    for(size_t i=0;i<P::Capacity;++i)assert(q.QueueCapture(Key(int(i))));
    assert(!q.QueueCapture(Key(100)));assert(q.Captures==P::Capacity);
    // General recipes have variable cardinality; repeats keep distinct materials.
    for(size_t n:{size_t(1),size_t(3),size_t(10),size_t(16)})
    {auto r=Recipe(n);S::Footprint f;assert(!S::Validate(r,f));
     S::Ledger ledger;ledger.Count=n;for(size_t i=0;i<n;++i)ledger.Entries[i]={i+1,true,true};
     assert(ledger.Complete(n));ledger.Entries[0].Member=false;assert(!ledger.Complete(n));
     if(n>3)assert(r.Instances[0].Mesh==r.Instances[3].Mesh&&r.Instances[0].Materials!=r.Instances[3].Materials);}
    for(size_t n:{size_t(0),size_t(17)}){auto r=Recipe(n);S::Footprint f;assert(S::Validate(r,f));}
    auto r=Recipe(1);S::Footprint f;assert(!S::Validate(r,f));
    r.Instances[0].Materials[0]=0;assert(S::Validate(r,f));r=Recipe(1);r.Instances[0].Hidden=true;assert(S::Validate(r,f));
    // Unknown is admitted only explicitly; no ID whitelist for the generic path.
    for(int id:{4,18,220,238,317,336,341,1001})
    {
        C::Candidate c;c.General=true;c.Identity=Key(id);c.ActorObject=10;c.ActorName=20;
        c.ClassificationObserved=c.RepresentationVerified=true;c.Truncated=false;c.Tier=MiniMapSpriteDiscoveryCore::Tier::Low;
        C::Evidence e{true,true,true,true,true};assert(!C::Decide(c,e).Allowed());assert(C::Decide(c,e,true).Allowed());
        c.Classification.Reason="InteriorTag";assert(!C::Decide(c,e,true).Allowed());
        c.Classification.Reason="InsufficientEvidence";c.Truncated=true;assert(!C::Decide(c,e,true).Allowed());
        c.Truncated=false;e.SourceCurrent=false;assert(!C::Decide(c,e,true).Allowed());
        e.SourceCurrent=true;e.SourcesRetained=false;assert(C::Decide(c,e,true).Status==C::State::NeedsRetention);
    }
    auto variant=Key(336);variant.TintBits[0]=0;assert(!(variant==Key(336)));
    std::unordered_map<MiniMapSpriteDiscoveryCore::Key,int,P::KeyHash> cache;
    cache.emplace(Key(336),1);cache.emplace(variant,2);assert(cache.size()==2&&cache.at(Key(336))==1&&cache.at(variant)==2);
    variant=Key(336);++variant.VisualClass;assert(!(variant==Key(336)));
    // Actual quad mapping includes camera padding and the off-center recipe origin.
    f.Initialized=true;f.Min={10,20,0};f.Max={30,40,2};
    std::array<std::array<double,2>,4> quad{};
    const auto near=[](double a,double b){return std::abs(a-b)<1e-6;};
    assert(P::Quad({100,200,0},{0,0,0,1},{2,3,1},f,quad));
    assert(near(quad[0][0],118)&&near(quad[0][1],257));
    auto rotation=MiniMapAssemblyRecipe::FromRotator({0,90,0});
    assert(P::Quad({100,200,0},rotation,{2,3,1},f,quad));
    assert(near(quad[0][0],43)&&near(quad[0][1],218));
    assert(!P::Quad({0,0,0},rotation,{-1,1,1},f,quad));
    assert(!P::Quad({0,0,0},MiniMapAssemblyRecipe::FromRotator({20,0,0}),{1,1,1},f,quad));
    assert(!P::Quad({NAN,0,0},rotation,{1,1,1},f,quad));
    // Entire fog rectangle, including a single hidden interior cell, is tested.
    size_t visits=0;auto reveal=[&](int,int){++visits;return true;};
    assert(P::Revealed(0,0,3,3,0,0,1,16,64,reveal)&&visits==16);
    assert(!P::Revealed(0,0,3,3,0,0,1,16,64,[](int x,int y){return !(x==1&&y==1);}));
    visits=0;assert(!P::Revealed(0,0,15,15,0,0,1,16,64,reveal)&&visits==0);
    assert(!P::Revealed(-1,0,1,1,0,0,1,16,64,reveal));assert(!P::Revealed(NAN,0,1,1,0,0,1,16,64,reveal));
    // Owner storage cannot be reused before verified retirement/destruction.
    MiniMapCaptureRetirement::FenceOwner fence;FenceMock mock;
    for(int job=0;job<3;++job){assert(fence.Rearm());assert(fence.Prepare(mock));assert(!fence.Rearm());
        assert(fence.Insert(mock,true));assert(!fence.Rearm());mock.Done=false;assert(!fence.Poll(mock));assert(!fence.Release(mock));
        mock.Done=true;assert(fence.Poll(mock));assert(!fence.Rearm());assert(fence.Release(mock));assert(fence.Release(mock));}
    assert(mock.Constructed==3&&mock.Destroyed==3);
    if(argc>1){std::ofstream file(argv[1]);file<<timed;assert(file.good());}
    std::cout<<"Building sprite pipeline: definitions, exclusions, deduplication/capacity, failure continuation, generation, variable recipes, variants, orientation, full fog coverage, fallback and repeated retirement passed\n";
}
