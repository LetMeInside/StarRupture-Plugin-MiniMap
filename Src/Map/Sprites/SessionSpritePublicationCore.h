#pragma once
#include "BuildingSpritePipelineCore.h"
#include <map>
#include <list>
#include <set>
#include <mutex>
#include <stdexcept>

// CPU ownership only. Native request tokens and retirement remain independent.
namespace MiniMapSessionPublication
{
    using Key=MiniMapSpriteDiscoveryCore::Key;
    struct Less { bool operator()(const Key& a,const Key& b)const noexcept{return a.Values()<b.Values();} };
    enum class Allocation { Slot,Index,Membership,Reserve,Pin,PinIndex,Version,Unselected };
    using AllocationHook=void(*)(Allocation); // Optional external-test fault injection; null in production.
    enum class ResetResult { Unchanged,Installed,WorldEnded,AllocationFailed };
    // Caller holds the global owner-access mutex. Allocate/retire never take it.
    // A failed transition retires the OLD world's authorization, but records an
    // explicit failed request; it never installs the old owner as the new world.
    template<class Owner,class Allocate,class Retire>
    ResetResult TransitionOwner(std::unique_ptr<Owner>& current,uint64_t requested,
        Allocate allocate,Retire retire,std::unique_ptr<Owner>& displaced)
    {
        if(current&&current->Generation->World==requested)return ResetResult::Unchanged;
        std::unique_ptr<Owner> replacement;bool failed=false;
        try{if(requested)replacement=allocate(requested);if(requested&&!replacement)failed=true;}
        catch(const std::bad_alloc&){failed=true;}
        displaced=std::move(current);
        if(displaced)retire(*displaced);
        current=std::move(replacement);
        return failed?ResetResult::AllocationFailed:requested?ResetResult::Installed:ResetResult::WorldEnded;
    }
    template<class Texture> class Store : public MiniMapBuildingSpriteCore::Policy, public std::enable_shared_from_this<Store<Texture>>
    {
    public:
        using Image=std::shared_ptr<Texture>;
        using Versions=std::map<uint64_t,Image>;
        struct Upload
        {Image Pixels;std::unique_ptr<Upload> Next;explicit Upload(Image image):Pixels(std::move(image)){};};
        struct Slot : MiniMapBuildingSpriteCore::Entry
        {
            uint64_t SlotId{};
            Image Cached;
            Versions Images;
            typename Versions::node_type Invisible;
            std::unique_ptr<Upload> Unselected;
            Slot* NextDirty{};
            bool Dirty=false,Waiting=false;
            MiniMapBuildingSpriteCore::State CountedState=MiniMapBuildingSpriteCore::State::Awaiting;
        };
        struct Pin
        {
            std::shared_ptr<Store> Owner;
            uint64_t Revision{};
            bool Registered=false;
            ~Pin(){if(Registered)Owner->Unpin(Revision);}
        };
        struct Prepared
        {
            Image DisplacedCache; // always released by caller after Commit unlocks
            typename Versions::node_type Visible,Invisible;
            std::unique_ptr<Upload> Unselected;
        };
        // Policy/membership methods require externally serialized owner access
        // (one GT producer); Draw touches only images/pins under Mutex.
        // Lock order: renderer owner-access -> Mutex. No native/backend calls here.
        mutable std::mutex Mutex;
        std::map<Key,Slot,Less> Slots;
        decltype(Slots)& Entries=Slots;
        size_t Count{};
        Slot* Touched{};
        std::array<size_t,8> StateCounts{};
        auto Counts()const{return StateCounts;}
        std::shared_ptr<Store> NextRetired;
        const uint64_t World;
        uint64_t Revision{},LastSlot{},PinEpoch{};
        size_t ImageSlots{},Pins{},SelectedCount{},SelectedBytes{},UnselectedCount{},UnselectedBytes{},WaitingCount{},VersionCount{};
        size_t AllocationFailures{},PublicationFailures{},CounterFailures{};
        bool Active=true,CurrentValid=false;
        std::set<Key,Less> Current;
        AllocationHook Fault=nullptr;
        explicit Store(uint64_t world):World(world){MiniMapBuildingSpriteCore::Policy::Reset(world);}
        void Check(Allocation at){if(Fault)Fault(at);}
        Slot* Find(const Key& key)
        {auto it=Slots.find(key);auto* next=it==Slots.end()?nullptr:&it->second;
            if(Touched!=next)FlushDemand();return Touched=next;} // owner-access held; no escaped draw pointers
        Slot* Observe(const Key& key)
        {
            if(key.Generation!=World||!World||!Active)return nullptr;
            if(auto* s=Find(key))return s;
            try
            {
                Check(Allocation::Slot);
                std::map<Key,Slot,Less> prepared;
                auto [it,added]=prepared.try_emplace(key);
                it->second.Key=key;it->second.Invisible=InvisibleNode();
                auto node=prepared.extract(it);
                std::scoped_lock lock(Mutex);
                if(!Active)return nullptr;
                if(LastSlot==(std::numeric_limits<uint64_t>::max)()){++PublicationFailures;return nullptr;}
                node.mapped().SlotId=LastSlot+1;
                Check(Allocation::Index);auto inserted=Slots.insert(std::move(node));
                if(inserted.inserted){++LastSlot;++Count;++StateCounts[0];}
                return Touched=&inserted.position->second;
            }
            catch(...){std::scoped_lock lock(Mutex);++AllocationFailures;return nullptr;}
        }
        MiniMapBuildingSpriteCore::Entry* Set(const Key& key,MiniMapBuildingSpriteCore::State state,const std::string& reason)
        {auto* s=Observe(key);return s?SetEntry(*s,state,reason):nullptr;}
        MiniMapBuildingSpriteCore::Admission QueueCapture(const Key& key,MiniMapCaptureLeases::Ledger& ledger,MiniMapCaptureLeases::Token& token)
        {auto* s=Observe(key);return s?Admit(*s,ledger,token):MiniMapBuildingSpriteCore::Admission::Capacity;}
        void FlushDemand(){if(Touched){auto* s=Touched;Touched=nullptr;SyncDemand(*s);
            if(s->CountedState!=s->Status){auto& old=StateCounts[size_t(s->CountedState)];
                if(!old){++CounterFailures;throw std::logic_error("sprite state counter underflow");}
                --old;++StateCounts[size_t(s->Status)];s->CountedState=s->Status;}}}
        bool SourceCurrent(const Key& key)const
        {return Active&&CurrentValid&&key.Generation==World&&Current.find(key)!=Current.end();}
        bool ObserveRegistry(const MiniMapSpriteDiscoveryCore::Registry& registry)
        {
            if(!registry.Complete()||registry.Generation!=World||!Active)return false;
            try{Check(Allocation::Membership);std::set<Key,Less> prepared;for(size_t i=0;i<registry.Count;++i)prepared.insert(registry.Entries[i].Identity);
                std::scoped_lock lock(Mutex);if(!Active)return false;Current.swap(prepared);CurrentValid=true;return true;}
            catch(...){std::scoped_lock lock(Mutex);CurrentValid=false;++AllocationFailures;return false;}
        }
        static bool Demand(const Slot& s)noexcept
        {const auto& d=s.Dependencies;return d.Tracked&&d.Demand&&d.ObservationWatch==MiniMapRendererReadiness::Dependencies::Watch::Waiting;}
        void SyncDemand(Slot& s)
        {
            const bool now=Demand(s);if(now==s.Waiting)return;
            if((now&&WaitingCount==(std::numeric_limits<size_t>::max)())||(!now&&!WaitingCount))
            {++CounterFailures;throw std::logic_error("sprite waiting demand counter inconsistent");}
            if(now)++WaitingCount;else --WaitingCount;s.Waiting=now;
        }
        struct DemandGuard
        {
            Store& Owner;Slot* Entry;
            ~DemandGuard()noexcept{if(Entry)try{Owner.SyncDemand(*Entry);}catch(...){std::terminate();}}
        };
        typename Versions::node_type InvisibleNode()
        {Check(Allocation::Reserve);Versions temp;temp.emplace(0,Image{});return temp.extract(temp.begin());}
        Prepared Prepare(Image image)
        {
            try {Check(Allocation::Version);Versions temp;temp.emplace(0,image);
            Prepared p;p.Visible=temp.extract(temp.begin());p.Invisible=InvisibleNode();
            Check(Allocation::Unselected);p.Unselected=std::make_unique<Upload>(std::move(image));return p;}
            catch(...){AllocationFailed();throw;}
        }
        std::shared_ptr<const Pin> Begin()
        {
            std::shared_ptr<Pin> pin;
            try
            {
                Check(Allocation::Pin);pin=std::make_shared<Pin>();pin->Owner=this->shared_from_this();
                std::scoped_lock lock(Mutex);if(!Active)return {};
                Check(Allocation::PinIndex);auto [it,added]=Revisions.try_emplace(Revision,0);
                if(it->second==(std::numeric_limits<size_t>::max)()||Pins==(std::numeric_limits<size_t>::max)())
                    {if(added)Revisions.erase(it);++CounterFailures;return {};}
                ++it->second;++Pins;pin->Revision=Revision;pin->Registered=true;return pin;
            }
            catch(...){std::scoped_lock lock(Mutex);++AllocationFailures;return {};}
        }
        Image Resolve(const Pin& pin,const Key& key)const
        {
            std::scoped_lock lock(Mutex);
            if(!Active||pin.Owner.get()!=this||!pin.Registered||key.Generation!=World)return {};
            auto found=Slots.find(key);if(found==Slots.end())return {};
            const auto& versions=found->second.Images;auto it=versions.upper_bound(pin.Revision);
            return it==versions.begin()?Image{}:std::prev(it)->second;
        }
        // Prepared allocations and image ownership precede loader upload. Caller
        // validates exact native token/source/canvas before invoking Commit.
        bool Commit(Slot& s,Prepared& p,uint64_t sourceRevision,uint64_t imageRevision)
        {
            std::scoped_lock lock(Mutex);
            if(!Active||!Owns(s)||s.Key.Generation!=World||s.Dependencies.SourceRevision!=sourceRevision||s.ImageRevision!=imageRevision||imageRevision==(std::numeric_limits<uint64_t>::max)()||Revision==(std::numeric_limits<uint64_t>::max)())
            {++PublicationFailures;return false;}
            if(s.Invisible.empty())s.Invisible=std::move(p.Invisible);
            if(s.Invisible.empty()||p.Visible.empty()||!p.Visible.mapped()||p.DisplacedCache){++PublicationFailures;return false;}
            p.Visible.key()=Revision+1;
            auto result=s.Images.insert(std::move(p.Visible));
            if(!result.inserted){p.Visible=std::move(result.node);++PublicationFailures;return false;}
            // Prior cached ownership remains in an immutable version until reaped.
            if(s.Cached){--SelectedCount;SelectedBytes-=s.Cached->RawBytes;}
            p.DisplacedCache=std::move(s.Cached);s.Cached=result.position->second;++SelectedCount;SelectedBytes+=s.Cached->RawBytes;
            if(s.Images.size()==1)++ImageSlots;
            ++Revision;++VersionCount;Dirty(s);return true;
        }
        bool Exclude(Slot& s)
        {
            std::scoped_lock lock(Mutex);
            if(!Owns(s)){++PublicationFailures;return false;}
            if(s.Images.empty()||!s.Images.rbegin()->second)return true;
            if(!Active||Revision==(std::numeric_limits<uint64_t>::max)()||s.Invisible.empty())
            {Active=false;++PublicationFailures;return false;} // fail closed; cannot represent a newer safety veto
            s.Invisible.key()=Revision+1;
            auto inserted=s.Images.insert(std::move(s.Invisible));
            if(!inserted.inserted){s.Invisible=std::move(inserted.node);Active=false;++PublicationFailures;return false;}
            ++Revision;++VersionCount;Dirty(s);return true;
        }
        void RetainUnselected(Slot& s,Prepared& p)
        {
            std::scoped_lock lock(Mutex);
            if(!p.Unselected||!p.Unselected->Pixels)return;
            ++UnselectedCount;UnselectedBytes+=p.Unselected->Pixels->RawBytes;
            p.Unselected->Next=std::move(s.Unselected);s.Unselected=std::move(p.Unselected);
        }
        void Invalidate(){std::scoped_lock lock(Mutex);Active=false;
            DirtyHead=DirtyTail=BlockedHead=BlockedTail=nullptr;}
        // Each unit extracts ONE version, unselected upload, or empty slot.
        // Detached nodes and potentially final texture refs die after unlocking.
        size_t Maintain(size_t limit=16,double budgetMs=2.0,bool finalDrain=false,bool retiredOwner=true)
        {
            const auto start=std::chrono::steady_clock::now();size_t units=0;
            while(units<limit)
            {
                typename Versions::node_type version;
                typename decltype(Slots)::node_type slot;
                typename decltype(Current)::node_type membership;
                std::unique_ptr<Upload> upload;
                {
                    std::scoped_lock lock(Mutex);
                    if(!Active)
                    {
                        // An internally fail-closed CURRENT owner still exposes
                        // policy counters under the outer access lock. It must
                        // first detach/reset before metadata retirement can run.
                        if(!retiredOwner)break;
                        if(Pins&&!finalDrain)break;
                        // Current membership stays fixed-capacity upstream at this
                        // checkpoint, but drain one node per unit here as well.
                        if(Slots.empty()){
                            if(Current.empty())break;membership=Current.extract(Current.begin());
                        }
                        else {
                        auto& s=Slots.begin()->second;
                        if(!s.Images.empty()){version=s.Images.extract(s.Images.begin());--VersionCount;if(s.Images.empty())--ImageSlots;}
                        else if(s.Unselected){upload=std::move(s.Unselected);s.Unselected=std::move(upload->Next);
                            --UnselectedCount;UnselectedBytes-=upload->Pixels->RawBytes;}
                        else {slot=Slots.extract(Slots.begin());if(Touched==&slot.mapped())Touched=nullptr;
                            --Count;--StateCounts[size_t(slot.mapped().CountedState)];
                            if(slot.mapped().Waiting){if(!WaitingCount)std::terminate();--WaitingCount;}
                            if(slot.mapped().Cached){--SelectedCount;SelectedBytes-=slot.mapped().Cached->RawBytes;}}
                        }
                    }
                    else
                    {
                        if(!DirtyHead)break;
                        Slot& s=*DirtyHead;auto keep=s.Images.end();
                        if(!s.Images.empty())
                        {
                            keep=Revisions.empty()?std::prev(s.Images.end()):s.Images.upper_bound(Revisions.begin()->first);
                            if(!Revisions.empty()&&keep!=s.Images.begin())--keep;
                        }
                        if(s.Images.begin()!=keep&&!s.Images.empty())
                        {version=s.Images.extract(s.Images.begin());--VersionCount;}
                        else
                        {
                            DirtyHead=s.NextDirty;if(!DirtyHead)DirtyTail=nullptr;
                            s.NextDirty=nullptr;
                            // Park blocked slots; pin release reactivates in O(1).
                            if(s.Images.size()>1){if(BlockedTail)BlockedTail->NextDirty=&s;else BlockedHead=&s;BlockedTail=&s;}
                            else s.Dirty=false;
                        }
                    }
                }
                ++units;
                if(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()>=budgetMs)break;
            }
            return units;
        }
        // Final shutdown is an explicit, finite O(H+V+U+S) CPU drain. Once
        // invalidated, Resolve cannot access slots even through an old pin.
        // A Draw already holding a copied Image remains safe independently.
        void DrainShutdown()
        {Invalidate();while(Maintain(16,(std::numeric_limits<double>::max)(),true)){};}
        void AllocationFailed(){std::scoped_lock lock(Mutex);++AllocationFailures;}
        std::array<size_t,9> Metrics()const
        {std::scoped_lock lock(Mutex);return {Pins,Revisions.size(),VersionCount>=ImageSlots?VersionCount-ImageSlots:0,
            UnselectedCount,UnselectedBytes,AllocationFailures,PublicationFailures,CounterFailures,Slots.size()};}
        bool Empty()const{std::scoped_lock lock(Mutex);return Slots.empty()&&Current.empty()&&!Pins;}
    private:
        std::map<uint64_t,size_t> Revisions;
        Slot *DirtyHead{},*DirtyTail{},*BlockedHead{},*BlockedTail{};
        bool Owns(const Slot& s)const noexcept
        {auto it=Slots.find(s.Key);return it!=Slots.end() && &it->second==&s;}
        void Dirty(Slot& s)noexcept
        {if(s.Dirty)return;s.Dirty=true;s.NextDirty=nullptr;if(DirtyTail)DirtyTail->NextDirty=&s;else DirtyHead=&s;DirtyTail=&s;}
        void Unpin(uint64_t revision)noexcept
        {
            std::scoped_lock lock(Mutex);auto it=Revisions.find(revision);
            if(it==Revisions.end()||!it->second||!Pins)std::terminate();
            if(!--it->second)Revisions.erase(it);--Pins;++PinEpoch;
            if(BlockedHead){if(DirtyTail)DirtyTail->NextDirty=BlockedHead;else DirtyHead=BlockedHead;DirtyTail=BlockedTail;BlockedHead=BlockedTail=nullptr;}
        }
    };
}
