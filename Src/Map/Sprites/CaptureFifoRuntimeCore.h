#pragma once
#include "CaptureDispatchCore.h"

namespace MiniMapCaptureFifoRuntime
{
    // Internal development gate. Set false to compare the preserved legacy dispatcher.
    inline constexpr bool DynamicCaptureFIFOEnabled = true;
    namespace W=MiniMapCaptureWork;namespace V=MiniMapSpriteValidation;namespace Q=MiniMapCaptureDispatch;namespace L=MiniMapCaptureLeases;
    // One scheduling-thread coordinator. Native ownership stays exclusively in the
    // existing lease ledger. Monitor holds CPU identities, never resources/leases.
    template<class Snapshot,class RetryClock=std::chrono::steady_clock> class Runtime
    {
        using S=V::Scheduler<Snapshot>;using Root=typename S::RegistryPublication;
        struct PendingWork {W::Ticket Work;std::optional<typename S::ExactRequest> Audit;std::shared_ptr<const Snapshot> Source;
            std::array<size_t,2> Indices{};size_t Instances{};uint64_t AuditSerial{},Epoch{};bool Audited=false;};
        struct NativeMonitor {W::Ticket Work;L::ObservationTicket Observation;bool Posted=false,Quarantined=false,Consumed=false;};
        std::optional<PendingWork> Pending;
        // CPU-only progress, never a native recipe/authorization. The ledger,
        // not this map, chooses when an owner-deferred key runs again.
        std::map<MiniMapSpriteDiscoveryCore::Key,PendingWork,MiniMapSpriteDiscoveryCore::KeyLess> OwnerDeferred;
        void DeferOwner()
        {
            if(Pending->Audited&&Pending->Source)
                try{OwnerDeferred.insert_or_assign(Pending->Work.Key,*Pending);}
                catch(const std::bad_alloc&){++Failures;} // Safe fallback: rebuild representatives on resumption.
            Work.Defer(Pending->Work,W::Wake::Owner);Pending.reset();
        }
        std::optional<NativeMonitor> Native;
        uint64_t SeenPublication{},SeenEpoch{};double SeenSourcePublication{};bool HasSeenSource=false;size_t SeenAdoption{};bool WasAvailable=false,ObserverWasBusy=false,Servicing=false;
    public:
        W::Ledger Work;W::ValidationAdapter<Snapshot> Submission;
        size_t Failures{},Admissions{},AuditCalls{};
        bool HasNativeObservation()const noexcept{return Native.has_value();}
        void CancelGeneration(uint64_t world)noexcept
        {Pending.reset();OwnerDeferred.clear();Submission.Clear();Work.Reset(world);SeenPublication=SeenEpoch=0;SeenSourcePublication=0;HasSeenSource=false;SeenAdoption=0;WasAvailable=false;}
        template<class Hooks> void Reconcile(Hooks& hooks)noexcept
        {
            if(!Native)return;
            auto& monitor=*Native;const auto result=hooks.Reconcile(monitor.Observation);
            // Quarantine blocks new handoffs, not observation of later exact proof.
            auto quarantine=[&]{if(!monitor.Quarantined)++Failures;monitor.Quarantined=true;};
            const bool cancelled=monitor.Work.Key.Generation!=Work.Generation();
            if(result.Owner.Status==Q::OwnerState::CertifiedRetired&&result.Identity){
                const auto receipt=hooks.Observe(monitor.Observation);
                if(!receipt.Sealed||receipt.State!=L::ObservationState::CertifiedRetired||receipt.Lease!=*result.Identity||
                    receipt.Lease.Candidate!=monitor.Work.Key||receipt.Lease.OwnerIncarnation!=monitor.Observation.Incarnation){quarantine();return;}
                if(!monitor.Consumed&&!cancelled){
                    if(!monitor.Posted||(!Work.RecordNativeCompletion(monitor.Work)&&!Work.RecordReconciledCompletion(monitor.Work,receipt))){quarantine();return;}
                }
                monitor.Consumed=true;monitor.Quarantined=false;
                if(!hooks.Acknowledge(monitor.Observation,*result.Identity))return;
                Native.reset();Work.Pulse(W::Wake::Owner);return;
            }
            if(result.Owner.Status==Q::OwnerState::NotAdmitted){
                const auto receipt=hooks.Observe(monitor.Observation);
                if(!receipt.Sealed||receipt.State!=L::ObservationState::NotAdmitted||receipt.Lease||result.Identity||result.Owner.LeaseHeld){quarantine();return;}
                if(!cancelled){const auto state=Work.Inspect(monitor.Work.Key);
                    // A claimed admitted ticket with proven non-admission is an
                    // unresolved CPU contract violation, not permission to replay.
                    if(!monitor.Posted||!state||state->Status==W::State::NativeAdmitted){quarantine();return;}
                    if(state->Status==W::State::Uncertain&&!Work.ResolveNonAdmission(monitor.Work)){quarantine();return;}}
                monitor.Quarantined=false;
                if(hooks.CancelObservation(monitor.Observation))Native.reset();
            }
            // Held/cleanup/unresolved remain monitored; neither timeout nor reset refunds a lease.
        }
        template<class Hooks> bool Service(S& validation,const std::shared_ptr<const Snapshot>& source,Hooks& hooks,double budgetMs=2.0)
        {
            if(Servicing)return false;Servicing=true;
            struct ServiceScope {bool& Active;~ServiceScope(){Active=false;}} scope{Servicing};
            const auto start=std::chrono::steady_clock::now();auto remaining=[&]{return (std::max)(0.0,budgetMs-std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());};
            Reconcile(hooks);
            if(Work.Generation()!=validation.Generation)CancelGeneration(validation.Generation);
            Work.MaintainRetired(remaining(),16);
            if(!source||!source->Complete||source->Generation!=validation.Generation)return false;
            // Scheduler recovery/missing representatives wait for a newer complete
            // collector source, which need not create a new discovery registry.
            if(std::isfinite(source->PublishedAtSeconds)&&(!HasSeenSource||source->PublishedAtSeconds>SeenSourcePublication)){
                HasSeenSource=true;SeenSourcePublication=source->PublishedAtSeconds;
                Work.Pulse(W::Wake::Recovery);Work.Pulse(W::Wake::Source);Work.Pulse(W::Wake::Prerequisites);
            }
            Submission.Service(validation,Work,[&](const V::Attempt& e)->std::optional<W::Revision>{
                if(!validation.AdoptionEpoch(e.Candidate.Identity,validation.LatestRegistry))return {};
                const auto renderer=hooks.Renderer(e.Candidate.Identity,validation.LatestRegistry);
                if(!renderer.Current||!renderer.KeyAdopted)return {};
                return W::Revision{renderer.SourceRevision,renderer.ImageRevision};
            },remaining());
            if(Work.PublicationEpoch()!=SeenPublication){OwnerDeferred.clear();SeenPublication=Work.PublicationEpoch();Work.Pulse(W::Wake::Recovery);Work.Pulse(W::Wake::Prerequisites);}
            if(validation.PendingEpoch!=SeenEpoch||validation.AdoptionCursor!=SeenAdoption){
                SeenEpoch=validation.PendingEpoch;SeenAdoption=validation.AdoptionCursor;Work.Pulse(W::Wake::Source);}
            const auto native=hooks.Owner();
            if(native.Available&&!WasAvailable||ObserverWasBusy&&!native.Leases.ObservationPending){Work.Pulse(W::Wake::Owner);Work.Pulse(W::Wake::Prerequisites);}WasAvailable=native.Available;ObserverWasBusy=native.Leases.ObservationPending;
            for(unsigned i=0;i<unsigned(W::Wake::Count)&&remaining()>0;++i)Work.ServiceWake(W::Wake(i),remaining(),16);
            Work.ServiceCooldown(RetryClock::now(),remaining(),16);
            if(remaining()<=0)return false;
            if(!Pending){auto selected=Work.Take(true,remaining(),16);if(!selected.Work)return false;Pending=PendingWork{*selected.Work};
                if(auto saved=OwnerDeferred.find(selected.Work->Key);saved!=OwnerDeferred.end()){
                    auto progress=std::move(saved->second);OwnerDeferred.erase(saved);
                    const auto* record=validation.Entries.Find(selected.Work->Key);
                    const auto root=validation.LatestRegistry;
                    if(progress.Work.Version==selected.Work->Version&&progress.Work.Publication==selected.Work->Publication&&
                        record&&record->AuditSerial==progress.AuditSerial&&V::ReusableEligibleEvidence(*record,validation.Generation)&&
                        validation.AdoptionEpoch(selected.Work->Key,root)==progress.Epoch&&progress.Source&&progress.Source->Complete&&
                        progress.Source->Generation==validation.Generation&&root&&progress.Source->PublishedAtSeconds>=root->PublicationSeconds){
                        progress.Work=*selected.Work;Pending=std::move(progress);
                    }
                }
            }
            const auto ticket=Pending->Work;const auto root=validation.LatestRegistry;
            if(!Work.MatchesConsideration(ticket)||!Work.MatchesPublication(root)||ticket.Publication!=Work.PublicationEpoch()){
                Work.Requeue(ticket);Pending.reset();return false;}
            auto renderer=hooks.Renderer(ticket.Key,root);
            const W::Revision actual{renderer.SourceRevision,renderer.ImageRevision};
            if(renderer.Current&&renderer.KeyAdopted&&actual!=ticket.Version){Work.Offer(ticket.Key,actual,Work.PublicationEpoch());Work.Requeue(ticket);Pending.reset();return false;}
            auto readiness=Q::Query(validation,root,ticket.Key,ticket.Version,renderer,source->Generation,source->PublishedAtSeconds,native.Available);
            if(readiness.Status==Q::State::Terminal){Work.CompleteConsideration(ticket);Pending.reset();return false;}
            if(readiness.Status==Q::State::Cancelled||readiness.Status==Q::State::Stale&&!readiness.NeedsAudit){Work.Defer(ticket,W::Wake::Source);Pending.reset();return false;}
            if(Pending->Audited&&readiness.NeedsAudit){Work.Defer(ticket,W::Wake::Source);Pending.reset();return false;}
            if(readiness.NeedsAudit||readiness.Status==Q::State::Ready&&!Pending->Audited){
                if(!Pending->Audit){auto* record=validation.Entries.Find(ticket.Key);
                    const bool reuse=readiness.Status==Q::State::Ready&&record&&V::ReusableEligibleEvidence(*record,validation.Generation);
                    if(!reuse&&record&&record->Done&&!validation.RequestExactReevaluation(ticket.Key,root)){Work.Defer(ticket,W::Wake::Source);Pending.reset();return false;}
                    auto prepared=validation.PrepareExact(ticket.Key,root,source,reuse);
                    if(!prepared.Request){Work.Defer(ticket,prepared.Result.Why==V::ExactReason::RecoveryPending?W::Wake::Recovery:W::Wake::Source);Pending.reset();return false;}
                    Pending->Audit=std::move(prepared.Request);
                }
                // Pin locally against reentrant cancellation. One audit or handoff per service call.
                const auto request=*Pending->Audit;
                const auto result=validation.ServiceExact(request,hooks.Copy,[&](const auto& first,const auto* second){++AuditCalls;return hooks.Audit(first,second);},remaining());
                if(!Pending||!Work.MatchesConsideration(ticket))return result.Invoked;
                if(result.Status==V::ExactState::Completed&&(result.Invoked||request.RepresentativesOnly)){
                    const auto* record=validation.Entries.Find(ticket.Key);
                    Pending->Source=request.Source;Pending->Indices=record->Indices;Pending->Instances=record->InstanceCount;
                    Pending->AuditSerial=record->AuditSerial;Pending->Epoch=request.Membership;Pending->Audited=true;Pending->Audit.reset();
                    if(result.Invoked)hooks.AuditFinished(ticket.Key,request.SourcePublication);
                    if(result.Invoked&&record->Validation.Disposition==V::AuditDisposition::Completed&&
                        (record->Validation.Status==V::Outcome::Conditional||record->Validation.Status==V::Outcome::NoResident)&&
                        record->Validation.Classification.CachedPolicy.ExcludeFromMiniMap!=MiniMapSpriteClassification::Exclusion::Exclude&&
                        record->Validation.Handoff.Handoff==MiniMapSpriteCapture::HandoffOutcome::NotOffered&&
                        !hooks.Renderer(ticket.Key,root).HasCachedImage){
                        Work.Cooldown(ticket,RetryClock::now());Pending.reset();
                    }

                }else if(result.Invoked||result.Status==V::ExactState::Stale||result.Status==V::ExactState::Cancelled){
                    // This callback is audit-only: interrupted traversal cannot have admitted native work.
                    const auto* failed=validation.Entries.Find(ticket.Key);
                    // Deferred and interrupted audits both set RecoveryRequired.
                    // Native availability alone cannot satisfy its source fence.
                    const auto cause=failed&&failed->RecoveryRequired?W::Wake::Recovery:W::Wake::Source;
                    Work.Defer(ticket,cause);
                    // A newer source may have arrived while this pinned scan was
                    // finishing. Consume that already-proven wake once, at the tail.
                    if(failed&&failed->RecoveryRequired&&source->Generation==validation.Generation&&
                        std::isfinite(source->PublishedAtSeconds)&&source->PublishedAtSeconds>failed->RecoveryPublication)
                        Work.WakeCandidate(ticket.Key,W::Wake::Recovery);
                    Pending.reset();
                }else if(result.Why!=V::ExactReason::RepresentativePending&&result.Why!=V::ExactReason::Budget){
                    Work.Defer(ticket,W::Wake::Source);
                    if(result.Why==V::ExactReason::RepresentativeUnavailable&&source->Generation==validation.Generation&&
                        std::isfinite(source->PublishedAtSeconds)&&source->PublishedAtSeconds>request.SourcePublication)
                        Work.WakeCandidate(ticket.Key,W::Wake::Source);
                    Pending.reset();
                }
                return result.Invoked;
            }
            if(readiness.Status!=Q::State::Ready){
                if(readiness.Why==Q::Reason::NativeOwned){DeferOwner();return false;}
                Work.Defer(ticket,readiness.Why==Q::Reason::ProofUnavailable?W::Wake::Advancement:W::Wake::Source);Pending.reset();return false;}
            if(Native){DeferOwner();return false;}
            const auto* record=validation.Entries.Find(ticket.Key);
            if(!record||!Pending->Source||record->AuditSerial!=Pending->AuditSerial||validation.AdoptionEpoch(ticket.Key,root)!=Pending->Epoch||
                Pending->Source->Generation!=validation.Generation||!Pending->Source->Complete||
                Pending->Source->PublishedAtSeconds<root->PublicationSeconds){Work.Defer(ticket,W::Wake::Source);Pending.reset();return false;}
            // Allocate fallback audit storage BEFORE arming/admitting. The ticket and monitor are allocation-free.
            V::Result result;
            const auto held=Pending->Source;const auto indices=Pending->Indices;const auto instances=Pending->Instances;
            const auto auditSerial=Pending->AuditSerial;
            const auto before=hooks.Owner();
            renderer=hooks.Renderer(ticket.Key,root);
            const auto finalReady=Q::Query(validation,root,ticket.Key,ticket.Version,renderer,source->Generation,source->PublishedAtSeconds,before.Available);
            if(finalReady.Status!=Q::State::Ready){
                if(finalReady.Why==Q::Reason::NativeOwned){DeferOwner();return false;}
                Work.Defer(ticket,finalReady.Why==Q::Reason::ProofUnavailable?W::Wake::Advancement:W::Wake::Source);Pending.reset();return false;}
            auto observation=hooks.Arm(before,ticket.Key);
            if(observation.Status!=L::ArmState::Armed){Work.Defer(ticket,W::Wake::Owner);Pending.reset();return false;}
            Native=NativeMonitor{ticket,observation.Ticket}; // reachable before irreversible handoff
            try{result=hooks.Handoff(held->Records[indices[0]],instances>1?&held->Records[indices[1]]:nullptr);}
            catch(...){result.MarkInterrupted();}
            observation.Fence.Close();
            const auto receipt=hooks.Observe(observation.Ticket);
            if(receipt.Lease&&result.Handoff.Handoff!=MiniMapSpriteCapture::HandoffOutcome::Admitted)
                result.Handoff.Handoff=MiniMapSpriteCapture::HandoffOutcome::Uncertain;
            if(result.Handoff.Handoff==MiniMapSpriteCapture::HandoffOutcome::Admitted&&
                (!result.Handoff.NativeLease||*result.Handoff.NativeLease!=receipt.Lease)){Native->Quarantined=true;++Failures;}
            if(result.Handoff.Handoff==MiniMapSpriteCapture::HandoffOutcome::Admitted)++Admissions;
            Native->Posted=Work.Finish(ticket,result);Pending.reset();
            if(Native->Posted&&receipt.Sealed&&receipt.State==L::ObservationState::NotAdmitted&&!receipt.Lease)
                validation.RecordRecipeRejection(ticket.Key,root,auditSerial,result);
            Reconcile(hooks);return true;
        }
    };
}
