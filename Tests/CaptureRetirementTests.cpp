#include "../Src/Experiments/CaptureRetirementCore.h"
#include <cassert>
#include <cstdio>
#include <type_traits>
#include <initializer_list>
using namespace MiniMapCaptureRetirement;
struct Mock
{
    bool AvailableValue=true, ConstructOk=true, BeginOk=true, Complete=false;
    int Constructors=0, Insertions=0, Polls=0, Destructors=0;
    bool Available(){return AvailableValue;}
    bool Construct(void*){++Constructors;return ConstructOk;}
    bool Begin(void*){++Insertions;return BeginOk;}
    bool Poll(void*){++Polls;return Complete;}
    void Destroy(void*){++Destructors;}
};
Evidence Rejected()
{
    Evidence e; e.External=e.CaptureCommitted=true;
    e.Enqueue=EnqueueState::RejectedBeforeEnqueue; return e;
}
int main()
{
    static_assert(!std::is_copy_constructible_v<FenceOwner> && !std::is_copy_assignable_v<FenceOwner>);
    auto e=Rejected(); assert(EmptyRejectedCapture(e));
    // Exercise the SAME enqueue-boundary helper used by Operations::Submit.
    std::atomic<EnqueueState> enqueue{EnqueueState::NotAttempted};
    std::atomic<bool> invoked{false};int nativeCalls=0;
    assert(!SubmitWithEvidence(false,&enqueue,&invoked,[&]{++nativeCalls;}));
    assert(!invoked.load() && enqueue.load()==EnqueueState::RejectedBeforeEnqueue && nativeCalls==0);
    assert(SubmitWithEvidence(true,&enqueue,&invoked,[&]
    {
        assert(invoked.load() && enqueue.load()==EnqueueState::NativeInvoked);
        ++nativeCalls; // callback may run synchronously before native Submit returns
        enqueue.store(EnqueueState::CallbackExecuting);
    }));
    assert(nativeCalls==1 && invoked.load() && enqueue.load()==EnqueueState::CallbackExecuting);
    assert(ObserveEnqueue(EnqueueState::NativeInvoked,true,1,true,false)==EnqueueState::NativeInvoked);
    assert(ObserveEnqueue(enqueue.load(),true,1,false,false)==EnqueueState::CallbackExecuting);
    assert(ObserveEnqueue(enqueue.load(),true,0,false,false)==EnqueueState::CallbackDestroyed);
    assert(ObserveEnqueue(enqueue.load(),true,0,false,true)==EnqueueState::ReadbackCompleted);
    assert(ObserveEnqueue(EnqueueState::Uncertain,true,0,false,true)==EnqueueState::Uncertain);
    try {SubmitWithEvidence(true,&enqueue,&invoked,[]{throw 1;});assert(false);}
    catch(...){enqueue.store(EnqueueState::Uncertain);}
    assert(invoked.load()); // throwing void enqueue can NEVER become a never-enqueued job
    // CommandQueued is deliberately irrelevant: only actual enqueue evidence counts.
    for(auto state:{EnqueueState::NotAttempted,EnqueueState::NativeInvoked,EnqueueState::CallbackExecuting,
        EnqueueState::CallbackDestroyed,EnqueueState::ReadbackCompleted,EnqueueState::Uncertain})
    {auto bad=e;bad.Enqueue=state;assert(!EmptyRejectedCapture(bad));}
    bool Evidence::* flags[]={&Evidence::NativeEverInvoked,&Evidence::ReadbackAllocated,&Evidence::TextureOwned,
        &Evidence::CopyIssued,&Evidence::MappingPossible,&Evidence::PendingFenceWritePossible,&Evidence::Outstanding};
    for(auto flag:flags){auto bad=e;bad.*flag=true;assert(!EmptyRejectedCapture(bad));}
    {auto bad=e;bad.Callables=1;assert(!EmptyRejectedCapture(bad));}
    {auto bad=e;bad.CaptureCommitted=false;assert(!EmptyRejectedCapture(bad));}
    {auto bad=e;bad.External=false;assert(!EmptyRejectedCapture(bad));}
    assert(!CanStart(EmptyRejectedCapture(e),false,true,false,true)); // renderer unavailable
    assert(!CanStart(EmptyRejectedCapture(e),true,false,false,true)); // resolution unavailable
    assert(!CanStart(EmptyRejectedCapture(e),true,true,true,false)); // stale world cannot detach
    assert(CanStart(EmptyRejectedCapture(e),true,true,false,false)); // before-EndPlay already detached
    assert(CanStart(EmptyRejectedCapture(e),true,true,true,true));
    assert(!CanPoll(EmptyRejectedCapture(e),true,true,false)); // detach must precede fence
    assert(CanPoll(EmptyRejectedCapture(e),true,true,true));
    for(int failure=0;failure<3;++failure)
    {
        Mock m;FenceOwner f;
        if(failure==0)m.AvailableValue=false;
        if(failure==1)m.ConstructOk=false;
        if(failure==2)m.BeginOk=false;
        const bool prepared=f.Prepare(m);
        if(failure<2){assert(!prepared);assert(m.Insertions==0);}
        else {assert(prepared);assert(!f.Insert(m,true));assert(!f.Release(m));}
        assert(m.Destructors==0); // uncertain native ownership never destroyed
    }
    // Cancellation before commit: initialized empty fence can be destroyed once.
    {Mock m;FenceOwner f;assert(f.Prepare(m));assert(!f.Prepare(m));assert(f.Release(m));assert(f.Release(m));assert(m.Destructors==1);}
    // World end before insertion: retain until detachment, then insert exactly once.
    {Mock m;FenceOwner f;assert(f.Prepare(m));assert(!f.Insert(m,false));assert(m.Insertions==0);
     assert(f.Insert(m,true));assert(!f.Insert(m,true));assert(m.Insertions==1);assert(!f.Release(m));
     assert(!f.Poll(m));assert(m.Polls==1);m.Complete=true;assert(f.Poll(m));
     auto bad=e;bad.Callables=1;assert(!CanPoll(EmptyRejectedCapture(bad),true,true,true)); // fence completion cannot override callback ownership
     assert(f.Release(m));assert(f.Release(m));assert(m.Destructors==1);assert(!f.Insert(m,true));}
    // World end after insertion: components already detached; no second fence.
    {Mock m;FenceOwner f;assert(f.Prepare(m));assert(f.Insert(m,true));
     assert(CanPoll(EmptyRejectedCapture(e),true,true,true));m.AvailableValue=false;assert(!f.Poll(m));assert(m.Polls==0);
     m.AvailableValue=true;m.Complete=true;assert(f.Poll(m));assert(f.Release(m));assert(m.Insertions==1);}
    // Successful/queued GPU jobs remain outside this recovery policy.
    e.NativeEverInvoked=true;assert(!CanStart(EmptyRejectedCapture(e),true,true,false,true));
    puts("Capture retirement policy/ownership tests passed");
}
