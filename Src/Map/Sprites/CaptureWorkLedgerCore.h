#pragma once
#include "SpriteValidationCore.h"
#include "BuildingSpritePipelineCore.h"
#include <atomic>

namespace MiniMapCaptureWork
{
    namespace D=MiniMapSpriteDiscoveryCore;
    namespace V=MiniMapSpriteValidation;
    namespace C=MiniMapSpriteCapture;
    namespace P=MiniMapBuildingSpriteCore;
    // Single scheduling-thread CPU state. No native object, recipe, lease or texture ownership.
    enum class State { Ready,Considering,Deferred,Completed,NativeAdmitted,Uncertain };
    enum class Wake { Source,Owner,Prerequisites,Advancement,Recovery,Cooldown,Count };
    // Opaque producer source/selection identities. Scheduling never manufactures or orders native proofs.
    struct Revision {uint64_t Source{},Selection{};bool operator==(const Revision&)const=default;};
    // Scheduling incarnation/serials only; this ticket is not a native lease or admission proof.
    struct Ticket {D::Key Key;Revision Version;uint64_t Owner{},Intent{},Serial{},Publication{};};
    enum class Submit { Inserted,Coalesced,Changed,NotCurrent,AllocationFailed,Exhausted };
    struct View {State Status{};Wake Waiting{};Revision Wanted;uint64_t Intent{};};
    struct Selection {std::optional<Ticket> Work;size_t Visits{};};
    inline std::atomic<uint64_t> NextOwner{};
    inline uint64_t AllocateOwner()noexcept
    {auto value=NextOwner.load();while(value!=UINT64_MAX)if(NextOwner.compare_exchange_weak(value,value+1))return value+1;return 0;}
    inline constexpr auto RepresentationAuditRetryCooldown=std::chrono::milliseconds(1000);
    class Ledger
    {
        struct Entry
        {
            D::Key Key;Revision Wanted;uint64_t Intent{},TicketSerial{},SeenWake{},ActiveIntent{},ActivePublication{};Revision ActiveVersion;
            State Status=State::Completed;Wake Waiting=Wake::Source;
            std::chrono::steady_clock::time_point RetryAt{};
            Entry *Previous{},*Next{};
        };
        struct List {Entry *Head{},*Tail{};size_t Count{};};
        struct Store
        {
            std::map<D::Key,Entry,D::KeyLess> Entries;List Ready;
            std::array<List,size_t(Wake::Count)> Waiting;
            Store* Next{};
        };
        const uint64_t Owner=AllocateOwner();
        uint64_t World{},NextIntent{},NextTicket{},NextPublication{};
        std::array<uint64_t,size_t(Wake::Count)> Signals{};
        Store *Active{},*RetiredHead{},*RetiredTail{};
        std::shared_ptr<const D::Registry> Membership;
        bool MembershipValid=false;
        List* LinkedList(Entry& e)noexcept
        {if(e.Status==State::Ready)return &Active->Ready;if(e.Status==State::Deferred)return &Active->Waiting[size_t(e.Waiting)];return nullptr;}
        void Unlink(Entry& e)noexcept
        {
            if(auto* list=LinkedList(e)){
                if(!list->Count)std::terminate();
                if(e.Previous)e.Previous->Next=e.Next;else list->Head=e.Next;
                if(e.Next)e.Next->Previous=e.Previous;else list->Tail=e.Previous;
                --list->Count;
            }
            e.Next=e.Previous=nullptr;
        }
        void Move(Entry& e,State state,Wake wait=Wake::Source)noexcept
        {
            Unlink(e);e.Status=state;e.Waiting=wait;
            if(state!=State::Deferred||wait!=Wake::Cooldown)e.RetryAt={};
            if(auto* list=LinkedList(e)){
                e.SeenWake=Signals[size_t(wait)];e.Previous=list->Tail;
                if(list->Tail)list->Tail->Next=&e;else list->Head=&e;
                list->Tail=&e;++list->Count;
            }
        }
        Entry* Find(const D::Key& key)noexcept
        {if(!Active)return nullptr;auto it=Active->Entries.find(key);return it==Active->Entries.end()?nullptr:&it->second;}
        static Wake CauseWake(C::Cause cause)noexcept
        {
            switch(cause){case C::Cause::NativeOwnerBusy:return Wake::Owner;
            case C::Cause::LifecycleUnavailable:case C::Cause::BackendUnavailable:case C::Cause::RetentionUnavailable:return Wake::Prerequisites;
            case C::Cause::ResourcePublicationPending:return Wake::Advancement;case C::Cause::OperationalException:return Wake::Recovery;
            default:return Wake::Source;}
        }
        void Apply(Entry& e,const V::Result& result)noexcept
        {
            const auto& handoff=result.Handoff;
            // These precede audit disposition: an interrupted audit can already own native work.
            if(handoff.Handoff==C::HandoffOutcome::Uncertain){Move(e,State::Uncertain);return;}
            if(handoff.Handoff==C::HandoffOutcome::Admitted){Move(e,State::NativeAdmitted);return;}
            if(result.Disposition==V::AuditDisposition::Interrupted){Move(e,State::Deferred,Wake::Recovery);return;}
            if(result.Disposition==V::AuditDisposition::Deferred){
                Move(e,State::Deferred,result.Wait==V::AuditWait::NativeOwnerBusy?Wake::Owner:
                    result.Wait==V::AuditWait::NativePrerequisites?Wake::Prerequisites:Wake::Source);return;}
            switch(handoff.Handoff){
            case C::HandoffOutcome::ObservationOnly:
                if(handoff.Admission){switch(*handoff.Admission){
                    case P::Admission::Accepted:Move(e,State::Uncertain);return; // Contradictory receipt, fail closed.
                    case P::Admission::Terminal:Move(e,State::Completed);return;
                    case P::Admission::AlreadyInFlight:case P::Admission::Capacity:Move(e,State::Deferred,Wake::Owner);return;
                    case P::Admission::SourceUnavailable:case P::Admission::InvalidGeneration:Move(e,State::Deferred,Wake::Source);return;
                    default:break;}}
                Move(e,State::Deferred,Wake::Advancement);return;
            case C::HandoffOutcome::Deferred:Move(e,State::Deferred,CauseWake(handoff.Why));return;
            case C::HandoffOutcome::Rejected:
                if(handoff.Why==C::Cause::Policy||handoff.Why==C::Cause::UnsupportedRepresentation){Move(e,State::Completed);return;}
                Move(e,State::Deferred,CauseWake(handoff.Why));return;
            default:break;
            }
            if(result.Status==V::Outcome::Unsupported||result.Status==V::Outcome::Invalid)Move(e,State::Completed);
            else Move(e,State::Deferred,result.Status==V::Outcome::Eligible?Wake::Owner:Wake::Source);
        }
    public:
        size_t AllocationFailures{},Exhaustions{};
        Ledger()=default;Ledger(const Ledger&)=delete;Ledger& operator=(const Ledger&)=delete;
        uint64_t Generation()const noexcept{return World;}
        bool HasRetired()const noexcept{return RetiredHead!=nullptr;}
        uint64_t PublicationEpoch()const noexcept{return MembershipValid?NextPublication:0;}
        size_t Size()const noexcept{return Active?Active->Entries.size():0;}
        size_t ReadyCount()const noexcept{return Active?Active->Ready.Count:0;}
        size_t DeferredCount(Wake cause)const noexcept{return Active&&cause<Wake::Count?Active->Waiting[size_t(cause)].Count:0;}
        std::optional<View> Inspect(const D::Key& key)noexcept
        {if(auto* e=Find(key))return View{e->Status,e->Waiting,e->Wanted,e->Intent};return {};}
        bool MatchesPublication(const std::shared_ptr<const D::Registry>& registry)const noexcept
        {return MembershipValid&&Membership==registry;}
        bool Current(const D::Key& key)const noexcept
        {return MembershipValid&&key.Generation==World&&Membership&&Membership->Index.find(key)!=Membership->Index.end();}
        void Reset(uint64_t generation)noexcept
        {
            // Native ownership lives elsewhere. Retiring CPU intent never returns a lease.
            MembershipValid=false;Membership.reset();
            if(Active){if(RetiredTail)RetiredTail->Next=Active;else RetiredHead=Active;RetiredTail=Active;Active=nullptr;}
            World=generation;Signals={};
        }
        bool ObservePublication(const std::shared_ptr<const D::Registry>& registry)noexcept
        {
            const bool valid=registry&&World&&registry->Generation==World&&registry->Complete()&&
                std::get_deleter<D::RetireRegistry>(registry)&&std::isfinite(registry->PublicationSeconds);
            if(!valid||(Membership&&registry->PublicationSeconds<Membership->PublicationSeconds)){
                MembershipValid=false;return false;
            }
            if(Membership==registry){MembershipValid=true;return true;}
            if(NextPublication==UINT64_MAX||Signals[size_t(Wake::Source)]==UINT64_MAX){MembershipValid=false;++Exhaustions;return false;}
            // Only retirement-backed immutable roots are accepted. Last-reference release enqueues.
            Membership=registry;MembershipValid=true;++NextPublication;
            Signal(Wake::Source,World,Signals[size_t(Wake::Source)]+1);return true;
        }
        // Capture the fence before reading producer revisions; stale submissions cannot overwrite current intents.
        Submit Offer(const D::Key& key,Revision revision,uint64_t publicationEpoch)noexcept
        {
            if(!Owner||!publicationEpoch||publicationEpoch!=PublicationEpoch()||!Current(key))return Submit::NotCurrent;
            if(auto* e=Find(key)){
                if(e->Wanted==revision)return Submit::Coalesced;
                if(NextIntent==UINT64_MAX){++Exhaustions;return Submit::Exhausted;}
                e->Wanted=revision;e->Intent=++NextIntent;
                // Ready positions survive supersession. Native/uncertain tickets cannot be replayed.
                if(e->Status==State::Completed||(e->Status==State::Deferred&&e->Waiting!=Wake::Recovery))Move(*e,State::Ready);
                return Submit::Changed;
            }
            if(NextIntent==UINT64_MAX){++Exhaustions;return Submit::Exhausted;}
            try{
                auto prepared=Active?std::unique_ptr<Store>{}:std::make_unique<Store>();
                auto* target=Active?Active:prepared.get();
                auto [it,inserted]=target->Entries.emplace(key,Entry{key,revision,NextIntent+1});
                if(!inserted)std::terminate();
                if(prepared)Active=prepared.release();
                ++NextIntent;Move(it->second,State::Ready);return Submit::Inserted;
            }catch(const std::bad_alloc&){++AllocationFailures;return Submit::AllocationFailed;}
        }
        // Selection is CPU consideration only, never capture entitlement. Stage 2b must retain all native gates.
        Selection Take(bool adoptionReady,double budgetMs=2.0,size_t limit=D::VisitsPerTick)noexcept
        {
            Selection out;if(!adoptionReady||!MembershipValid||!Active)return out;
            const auto start=std::chrono::steady_clock::now();
            while(Active->Ready.Head&&out.Visits<limit&&std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<budgetMs){
                auto& e=*Active->Ready.Head;++out.Visits;
                if(!Current(e.Key)){Move(e,State::Deferred,Wake::Source);continue;}
                if(NextTicket==UINT64_MAX){++Exhaustions;return out;}
                Move(e,State::Considering);e.TicketSerial=++NextTicket;e.ActiveIntent=e.Intent;e.ActiveVersion=e.Wanted;e.ActivePublication=PublicationEpoch();
                out.Work=Ticket{e.Key,e.Wanted,Owner,e.Intent,e.TicketSerial,e.ActivePublication};return out;
            }
            return out;
        }
        bool MatchesConsideration(const Ticket& ticket)noexcept
        {
            const auto* entry=Find(ticket.Key);
            return ticket.Owner==Owner&&ticket.Key.Generation==World&&entry&&entry->Status==State::Considering&&
                entry->TicketSerial==ticket.Serial&&entry->ActiveIntent==ticket.Intent&&entry->ActiveVersion==ticket.Version&&entry->ActivePublication==ticket.Publication;
        }
        bool Finish(const Ticket& ticket,const V::Result& result)noexcept
        {
            auto* e=Find(ticket.Key);
            if(ticket.Owner!=Owner||ticket.Key.Generation!=World||!e||e->Status!=State::Considering||e->TicketSerial!=ticket.Serial||e->ActiveIntent!=ticket.Intent||e->ActiveVersion!=ticket.Version||e->ActivePublication!=ticket.Publication)return false;
            if(ticket.Publication!=PublicationEpoch()&&result.Handoff.Handoff!=C::HandoffOutcome::Admitted&&
                result.Handoff.Handoff!=C::HandoffOutcome::Uncertain){
                // Superseded non-native evidence cannot complete the newest intent.
                Move(*e,Current(e->Key)?State::Ready:State::Deferred,Wake::Source);return true;
            }
            Apply(*e,result);
            if(e->Intent!=ticket.Intent&&e->Status!=State::NativeAdmitted&&e->Status!=State::Uncertain)
                Move(*e,Current(e->Key)?State::Ready:State::Deferred,Wake::Source);
            return true;
        }
        // Records an externally established completion; does not certify cleanup or refund native ownership.
        bool RecordNativeCompletion(const Ticket& ticket)noexcept
        {
            auto* e=Find(ticket.Key);
            if(ticket.Owner!=Owner||ticket.Key.Generation!=World||!e||e->Status!=State::NativeAdmitted||e->TicketSerial!=ticket.Serial||e->ActiveIntent!=ticket.Intent||e->ActiveVersion!=ticket.Version||e->ActivePublication!=ticket.Publication)return false;
            Move(*e,e->Intent==ticket.Intent?State::Completed:Current(e->Key)?State::Ready:State::Deferred,Wake::Source);return true;
        }
        // Uncertain work needs an externally certified exact observation, unlike
        // the legacy completion notification. This never certifies/refunds a lease.
        bool RecordReconciledCompletion(const Ticket& ticket,const MiniMapCaptureLeases::RetirementObservation& receipt)noexcept
        {
            if(!receipt.Sealed||receipt.State!=MiniMapCaptureLeases::ObservationState::CertifiedRetired||!receipt.Lease||receipt.Lease.Candidate!=ticket.Key)return false;
            auto* e=Find(ticket.Key);
            if(ticket.Owner!=Owner||ticket.Key.Generation!=World||!e||e->Status!=State::Uncertain||e->TicketSerial!=ticket.Serial||
                e->ActiveIntent!=ticket.Intent||e->ActiveVersion!=ticket.Version||e->ActivePublication!=ticket.Publication)return false;
            Move(*e,e->Intent==ticket.Intent?State::Completed:Current(e->Key)?State::Ready:State::Deferred,Wake::Source);return true;
        }
        // Runtime transitions are exact-ticket operations; none certify native cleanup.
        bool Defer(const Ticket& ticket,Wake cause)noexcept
        {if(!MatchesConsideration(ticket))return false;Move(*Find(ticket.Key),State::Deferred,cause);return true;}
        // Completed unsuccessful audits wait off the runnable FIFO. This is
        // scheduling only; no result reuse, native entitlement or lease ownership.
        bool Cooldown(const Ticket& ticket,std::chrono::steady_clock::time_point now)noexcept
        {if(!MatchesConsideration(ticket))return false;auto* e=Find(ticket.Key);
            Move(*e,State::Deferred,Wake::Cooldown);e->RetryAt=now+RepresentationAuditRetryCooldown;return true;}
        size_t ServiceCooldown(std::chrono::steady_clock::time_point now,double budgetMs=2.0,size_t limit=16)noexcept
        {
            if(!Active)return 0;
            const auto start=std::chrono::steady_clock::now();auto& list=Active->Waiting[size_t(Wake::Cooldown)];
            size_t visits=0;
            // A monotonic clock and one fixed interval keep deadlines in FIFO
            // order. An unexpired head cannot hide due work behind it. Removed
            // identities lose their deadline here, at latest when it comes due.
            while(list.Head&&visits<limit&&std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<budgetMs){
                auto& e=*list.Head;++visits;
                if(!Current(e.Key))Move(e,State::Deferred,Wake::Source);
                else if(now>=e.RetryAt)Move(e,State::Ready);
                else break;
            }
            return visits;
        }
        bool Requeue(const Ticket& ticket)noexcept
        {if(!MatchesConsideration(ticket))return false;Move(*Find(ticket.Key),Current(ticket.Key)?State::Ready:State::Deferred,Wake::Source);return true;}
        bool CompleteConsideration(const Ticket& ticket)noexcept
        {if(!MatchesConsideration(ticket))return false;Move(*Find(ticket.Key),State::Completed);return true;}
        bool ResolveNonAdmission(const Ticket& ticket)noexcept
        {auto* e=Find(ticket.Key);if(!e||ticket.Owner!=Owner||ticket.Key.Generation!=World||e->Status!=State::Uncertain||
            e->TicketSerial!=ticket.Serial||e->ActiveIntent!=ticket.Intent||e->ActiveVersion!=ticket.Version||e->ActivePublication!=ticket.Publication)return false;
            Move(*e,State::Deferred,Wake::Recovery);return true;}
        bool WakeCandidate(const D::Key& key,Wake cause)noexcept
        {auto* e=Find(key);if(!e||!Current(key)||e->Status==State::Considering||e->Status==State::NativeAdmitted||e->Status==State::Uncertain)return false;
            // Exact authoritative reevaluation bypasses ordinary retry delay.
            if(e->Status==State::Deferred&&e->Waiting==Wake::Cooldown){
                if(cause!=Wake::Advancement)return false;Move(*e,State::Ready);return true;}
            if(e->Status==State::Ready)return true;
            if(e->Status==State::Deferred&&e->Waiting!=cause)return false;
            Move(*e,State::Ready);return true;}
        bool Pulse(Wake cause)noexcept
        {if(cause>=Wake::Count||Signals[size_t(cause)]==UINT64_MAX){++Exhaustions;return false;}
            return Signal(cause,World,Signals[size_t(cause)]+1);}
        // Wake sequence numbers identify CPU events, NOT resource advancement proofs.
        bool Signal(Wake cause,uint64_t generation,uint64_t sequence)noexcept
        {
            if(!World||cause>=Wake::Count||generation!=World||!sequence||sequence<=Signals[size_t(cause)])return false;
            Signals[size_t(cause)]=sequence;return true;
        }
        size_t ServiceWake(Wake cause,double budgetMs=2.0,size_t limit=D::VisitsPerTick)noexcept
        {
            if(!Active||cause>=Wake::Count||cause==Wake::Cooldown)return 0;
            const auto start=std::chrono::steady_clock::now();auto& list=Active->Waiting[size_t(cause)];size_t visits=0;
            while(list.Head&&list.Head->SeenWake<Signals[size_t(cause)]&&visits<limit&&
                std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<budgetMs){
                auto& e=*list.Head;++visits;
                Move(e,Current(e.Key)?State::Ready:State::Deferred,Wake::Source);
            }
            return visits;
        }
        size_t MaintainRetired(double budgetMs=2.0,size_t limit=16)noexcept
        {
            const auto start=std::chrono::steady_clock::now();size_t visits=0;
            while(RetiredHead&&visits<limit&&std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<budgetMs){
                auto* store=RetiredHead;if(!store->Entries.empty())store->Entries.erase(store->Entries.begin());
                if(store->Entries.empty()){RetiredHead=store->Next;if(!RetiredHead)RetiredTail=nullptr;delete store;}
                ++visits;
            }
            return visits;
        }
        ~Ledger(){Reset(0);while(MaintainRetired((std::numeric_limits<double>::max)(),16)){};} // Final CPU drain only.
    };

    // Dormant adapter: Stage 2b must explicitly activate it at existing adoption/audit call sites.
    // It neither removes the global readiness barrier nor invokes native APIs.
    template<class Snapshot> class ValidationAdapter
    {
        using Scheduler=V::Scheduler<Snapshot>;
        std::shared_ptr<const D::Registry> Plan,Finished;
        uint64_t World{};size_t Cursor{};
    public:
        size_t LastVisits{},LastNew{};bool AllocationBlocked=false,RevisionUnavailable=false;
        template<class Revisions> bool Service(Scheduler& validation,Ledger& ledger,Revisions revisions,double budgetMs=2.0)
        {
            LastVisits=LastNew=0;AllocationBlocked=RevisionUnavailable=false;
            if(World!=validation.Generation){Plan.reset();Finished.reset();Cursor=0;World=validation.Generation;ledger.Reset(World);}
            const auto latest=validation.AdoptionBlocked?nullptr:validation.LatestRegistry;
            if(!ledger.ObservePublication(latest))return false;
            if(!Plan){
                if(Finished==latest)return true;
                Plan=validation.PendingRegistry?validation.PendingRegistry:validation.AdoptedRegistry;Cursor=0;
                if(!Plan)return false;
            }
            const auto start=std::chrono::steady_clock::now();
            while(Cursor<Plan->Count&&LastVisits<D::VisitsPerTick&&LastNew<D::NewPerTick&&
                std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<budgetMs){
                const auto& key=Plan->Entries[Cursor].Identity;
                if(ledger.Current(key)){
                    const auto* attempt=validation.Entries.Find(key);
                    if(!attempt)return false; // Producer's bounded adoption has not reached this key yet.
                    try{
                        // Pure CPU getter supplies authoritative source/selection identities, not publication ticks or invented proofs.
                        const auto publicationEpoch=ledger.PublicationEpoch();
                        const auto revision=revisions(*attempt);
                        if(!revision){RevisionUnavailable=true;return false;}
                        if(validation.LatestRegistry!=latest){RevisionUnavailable=true;return false;}
                        const auto result=ledger.Offer(key,*revision,publicationEpoch);
                        if(result==Submit::AllocationFailed){AllocationBlocked=true;return false;}
                        if(result==Submit::NotCurrent){RevisionUnavailable=true;return false;}
                        if(result==Submit::Exhausted)return false;
                        if(result==Submit::Inserted)++LastNew;
                    }catch(const std::bad_alloc&){AllocationBlocked=true;return false;}
                }
                ++Cursor;++LastVisits;
            }
            if(Cursor!=Plan->Count)return false;
            Finished=std::move(Plan);Cursor=0;return Finished==latest;
        }
        Selection Take(const Scheduler& validation,Ledger& ledger,double budgetMs=2.0)noexcept
        {return ledger.Take(validation.LatestRegistry&&ledger.MatchesPublication(validation.LatestRegistry)&&validation.ReadyFor(*validation.LatestRegistry),budgetMs);}
        bool Result(const Scheduler& validation,Ledger& ledger,const Ticket& ticket,const V::Result& result)noexcept
        {
            // Do not drop admitted/uncertain ownership evidence when membership supersedes an audit.
            // Ledger checks exact CPU ticket identity; native admission remains external authority.
            if(validation.Generation!=ledger.Generation())return false;
            ledger.ObservePublication(validation.AdoptionBlocked?nullptr:validation.LatestRegistry);
            return ledger.Finish(ticket,result);
        }
        template<class Get,class Audit> V::ExactServiceResult ServiceExact(Scheduler& validation,Ledger& ledger,
            const Ticket& ticket,const std::shared_ptr<const Snapshot>& source,Get get,Audit audit,double budgetMs=2.0)
        {
            if(ticket.Key.Generation!=validation.Generation||ledger.Generation()!=validation.Generation)
                return {V::ExactState::Cancelled,V::ExactReason::Generation};
            if(!ledger.MatchesConsideration(ticket))return {V::ExactState::Stale,V::ExactReason::TicketChanged};
            if(!ledger.MatchesPublication(validation.LatestRegistry)||ticket.Publication!=ledger.PublicationEpoch())
                return {V::ExactState::Stale,V::ExactReason::Publication};
            auto prepared=validation.PrepareExact(ticket.Key,validation.LatestRegistry,source);
            if(!prepared.Request)return prepared.Result;
            // Match once more at invocation: no unrelated candidate may satisfy this ticket.
            auto guarded=[&](const auto& first,const auto* second){
                if(!ledger.MatchesConsideration(ticket)||ticket.Publication!=ledger.PublicationEpoch()){
                    V::Result result;result.MarkDeferred(V::AuditWait::SourceUnavailable);return result;
                }
                return audit(first,second);
            };
            auto serviced=validation.ServiceExact(*prepared.Request,std::move(get),guarded,budgetMs);
            if(serviced.Status==V::ExactState::Cancelled||serviced.Status==V::ExactState::Stale)return serviced;
            if(ticket.Key.Generation!=ledger.Generation())return {V::ExactState::Cancelled,V::ExactReason::Generation,serviced.Invoked};
            if(!ledger.MatchesConsideration(ticket)||ticket.Publication!=ledger.PublicationEpoch())
                return {V::ExactState::Stale,V::ExactReason::TicketChanged,serviced.Invoked};
            return serviced;
        }
        void Clear()noexcept{Plan.reset();Finished.reset();World=0;Cursor=0;}
    };
}
