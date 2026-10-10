#include "../Src/Experiments/StaticAssemblyCaptureCore.h"
#include "../Src/Experiments/CaptureRetirementCore.h"
#include <cassert>
#include <iostream>
namespace S=MiniMapStaticAssembly;
namespace A=MiniMapAssemblyRecipe;
static bool Near(double a,double b){return std::abs(a-b)<1e-6;}
static S::Recipe Recipe()
{
    S::Recipe r;r.Count=10;r.ActorClass=999;r.RootVerified=r.Complete=r.HighDetail=true;r.Generation=5;
    constexpr uintptr_t mesh[]={10,23,24,26,27,23,26,24,12,17};
    const A::V positions[]={{0,0,0},{-231,0,300},{-247.73365,0,299.07521},{-231,0,260},{0,0,0},
        {301,0,372},{302,0,332},{322.26635,0,371.07521},{120,0,0},{120,0,0}};
    for(size_t n=0;n<10;++n)
    {auto& i=r.Instances[n];i.Mesh=mesh[n];i.ComponentIndex=uint32_t(n+1);i.Transform.T=positions[n];i.Verified=i.Visible=true;i.Hidden=false;i.LocalBounds.Extent={1,2,3};i.MaterialCount=1;i.Materials[0]=100+n;}
    r.Instances[1].Transform.R=A::FromRotator({0,180,0});
    r.Instances[2].Transform.R=A::FromRotator({0,90,0});r.Instances[2].Transform.S={2,3,4};
    r.Instances[7].Transform.R=A::FromRotator({0,270,0});r.Instances[7].Transform.S={2,3,4};
    r.Instances[2].Overrides[0]=r.Instances[7].Overrides[0]=true;
    return r;
}
int main()
{
    auto r=Recipe();S::Footprint f;assert(!S::Validate(r,f));
    assert(r.Instances[2].Mesh==r.Instances[7].Mesh&&r.Instances[2].Materials[0]!=r.Instances[7].Materials[0]);
    assert(r.Instances[8].Transform.T==r.Instances[9].Transform.T&&r.Instances[8].Mesh!=r.Instances[9].Mesh);
    assert(f.Min[0]<-247&&f.Max[0]>322&&Near(S::OrthoWidth(f),(f.Max[0]-f.Min[0])*1.1));
    for(size_t n=0;n<10;++n)
    {
        auto x=r;x.Instances[n].Mesh=0;assert(S::Validate(x,f));
        x=r;x.Instances[n].Materials[0]=0;assert(S::Validate(x,f));
        x=r;x.Instances[n].Verified=false;assert(S::Validate(x,f));
        x=r;x.Instances[n].Transform.S[0]=-1;assert(S::Validate(x,f));
        x=r;x.Instances[n].LocalBounds.Extent[0]=NAN;assert(S::Validate(x,f));
    }
    auto x=r;x.Count=9;assert(S::Validate(x,f));x=r;x.Count=17;assert(S::Validate(x,f));
    x=r;x.Instances[9].Mesh=x.Instances[8].Mesh;assert(S::Validate(x,f));
    x=r;x.Instances[1].ComponentIndex=x.Instances[0].ComponentIndex;assert(S::Validate(x,f));
    x=r;x.Instances[0].Hidden=true;assert(S::Validate(x,f));
    x=r;x.Instances[0].MaterialCount=33;assert(S::Validate(x,f));
    x=r;for(auto& i:x.Instances)i.MaterialCount=32;assert(S::Validate(x,f));
    x=r;x.RootVerified=false;assert(S::Validate(x,f));
    x=r;x.ActorClass=0;assert(S::Validate(x,f));
    // Independent rotated-box oracle: scale (2,3,4), yaw 90, translation.
    S::Instance box;box.LocalBounds.Extent={1,2,3};box.Transform.S={2,3,4};
    box.Transform.T={10,20,30};box.Transform.R=A::FromRotator({0,90,0});f={};assert(S::Add(f,box));
    assert(Near(f.Min[0],4)&&Near(f.Max[0],16)&&Near(f.Min[1],18)&&Near(f.Max[1],22)&&Near(f.Min[2],18)&&Near(f.Max[2],42));
    box.LocalBounds.Extent[0]=0;assert(!S::Add(f,box));box.LocalBounds.Extent[0]=INFINITY;assert(!S::Add(f,box));
    auto expected=r.Instances[2].Transform;auto observed=expected;for(auto& q:observed.R)q=-q;
    assert(S::Equivalent(expected,observed));observed.T[0]=NAN;assert(!S::Equivalent(expected,observed));
    observed=expected;observed.S[2]=INFINITY;assert(!S::Equivalent(expected,observed));
    observed=expected;observed.T[2]+=200000;assert(!S::Equivalent(expected,observed));expected.T[2]+=200000;assert(S::Equivalent(expected,observed));
    // Failure injection after every allocation, registration and membership step.
    for(size_t allocated=0;allocated<=10;++allocated)for(size_t registered=0;registered<=allocated;++registered)
    {
        S::Ledger l;l.Count=allocated;
        for(size_t i=0;i<allocated;++i)l.Entries[i]={i+1,i<registered,i<registered};
        size_t destroyed=0,detached=0;uintptr_t prior=100;
        assert(!l.Release(false,[&](uintptr_t){++destroyed;}));assert(!destroyed);
        l.Detach([&](uintptr_t p){assert(p<prior);prior=p;++detached;});assert(detached==registered);
        l.Detach([&](uintptr_t){assert(false);});prior=100;
        assert(l.Release(true,[&](uintptr_t p){assert(p<prior);prior=p;++destroyed;}));assert(destroyed==allocated);
        assert(l.Release(true,[&](uintptr_t){assert(false);}));
    }
    S::Ledger l;l.Count=10;for(size_t i=0;i<10;++i)l.Entries[i]={i+1,true,true};assert(l.Complete());
    for(size_t i=0;i<10;++i){auto bad=l;bad.Entries[i].Member=false;assert(!bad.Complete());bad=l;bad.Entries[i].Registered=false;assert(!bad.Complete());}
    l.Entries[9].Component=l.Entries[0].Component;assert(!l.Complete());
    for(bool uncertain:{false,true})for(bool registered:{false,true})for(bool current:{false,true})
        assert(S::CanCleanBeforeCommit(uncertain,registered,current)==(!uncertain&&(!registered||current)));
    assert(!S::CanCleanBeforeCommit(false,true,false)); // stale-world cleanup denied
    std::cout<<"static assembly inputs, distinct materials/instances, QST bounds, membership and partial retirement passed\n";
}
