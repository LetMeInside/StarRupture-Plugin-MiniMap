#pragma once
#include "SpriteDiscoveryCore.h"
#include "SpriteClassificationCore.h"
#include "SpriteCaptureCore.h"
#include <type_traits>
#include <stdexcept>
#include <atomic>

namespace MiniMapSpriteValidation
{
    namespace D=MiniMapSpriteDiscoveryCore;
    namespace J=MiniMapRepresentationCore;
    inline constexpr size_t DetailLimit=16384,OutputLimit=256*1024;
    enum class Outcome { Eligible,Conditional,Unsupported,NoResident,Invalid };
    enum class AuditDisposition { Completed,Deferred,Interrupted };
    enum class AuditWait { None,SourceUnavailable,NativeOwnerBusy,NativePrerequisites };
    inline const char* DispositionLabel(AuditDisposition d,bool done)
    {return d==AuditDisposition::Interrupted?"interrupted":d==AuditDisposition::Deferred?"deferred":done?"completed":"pending";}
    inline const char* Label(Outcome o)
    {switch(o){case Outcome::Eligible:return "private_blueprint_eligible";case Outcome::Conditional:return "conditional";
    case Outcome::Unsupported:return "unsupported_current_method";case Outcome::NoResident:return "no_validated_resident_representation";default:return "invalid_or_stale";}}
    struct Result
    {
        MiniMapSpriteClassification::Observation Classification;
        Outcome Status=Outcome::Invalid;
        AuditDisposition Disposition=AuditDisposition::Completed;
        AuditWait Wait=AuditWait::None;
        MiniMapSpriteCapture::Decision Handoff;
        MiniMapStaticAssembly::RecipeValidation RecipeCheck;
        // Observed before any offer; zero is a valid initial renderer revision, not an invented source.
        std::optional<uint64_t> AuditRendererRevision;
        std::string Reason="not_attempted",Route="none",High="not_examined",Low="not_examined",Mass="not_examined",
            Config="not_examined",AuthoredClass="not_examined",Selected="null",Details="{}",Inspection="{}",Profile="{}",
            CookedGeometry="{\"status\":\"not_attempted\"}",SkeletalMetadata="{\"status\":\"not_attempted\"}";
        size_t Static{},UniqueMeshes{},Skeletal{},Scene{},NonScene{},Nodes{},MaterialSlots{},OverrideSlots{},
            IdentityVisits{},GraphVisits{},AuditVisits{},Probes{};
        bool Root=false,Transforms=false,Materials=false,Resident=false,Truncated=false,Inherited=false,Child=false,OtherSkinned=false,DerivedStatic=false;
        // Truncated describes operational evidence only; output caps never veto a recipe.
        bool DiagnosticTruncated=false;
        void MarkDeferred(AuditWait wait)noexcept{Disposition=AuditDisposition::Deferred;Wait=wait;Status=Outcome::Invalid;}
        // Allocation-free even when the operational failure is bad_alloc.
        // A thrown adapter may have crossed native commitment before returning.
        // Uncertain is not permission to replay; the native owner remains authoritative.
        void MarkInterrupted()noexcept{Disposition=AuditDisposition::Interrupted;Wait=AuditWait::None;Status=Outcome::Invalid;
            if(Handoff.Handoff!=MiniMapSpriteCapture::HandoffOutcome::Admitted)Handoff.Handoff=MiniMapSpriteCapture::HandoffOutcome::Uncertain;}
        void RecordHandoff(const MiniMapSpriteCapture::Decision& decision)noexcept
        {Handoff=decision;if(decision.Why==MiniMapSpriteCapture::Cause::RecipeInvalid)RecipeCheck=decision.RecipeCheck;
            if(decision.Why==MiniMapSpriteCapture::Cause::OperationalException)MarkInterrupted();}
    };
    inline void BoundDiagnosticDetails(Result& r)
    {if(r.Details.size()>DetailLimit){r.Details="{\"truncated\":true}";r.DiagnosticTruncated=true;}}
    struct AuditProvenance
    {uint64_t Generation{},Serial{},Membership{};double SourcePublication{};std::optional<uint64_t> RendererRevision;};
    struct Attempt
    {
        D::Candidate Candidate;Result Validation;
        std::optional<AuditProvenance> Provenance;
        std::array<size_t,2> Indices{};size_t InstanceCount{};
        uint64_t MembershipEpoch{},RepresentativeEpoch{},AuditedMembershipEpoch{};
        double AuditedSourcePublication{};
        double RecoveryPublication{};
        uint64_t AuditSerial{};bool Auditing=false;
        bool Done=false,MissingRepresentative=false,RecoveryRequired=false;
    };
    // Reusable FIFO preflight evidence, not current membership or native admission.
    // The native handoff revalidates candidate identity and session ownership.
    inline bool ReusableEligibleEvidence(const Attempt& e,uint64_t generation)noexcept
    {
        const auto& p=e.Provenance;
        return e.Done&&!e.RecoveryRequired&&e.Validation.Disposition==AuditDisposition::Completed&&
            e.Validation.Status==Outcome::Eligible&&e.Candidate.Identity.Generation==generation&&p&&
            p->Generation==generation&&p->Serial==e.AuditSerial&&p->Membership==e.AuditedMembershipEpoch&&
            p->SourcePublication==e.AuditedSourcePublication&&std::isfinite(p->SourcePublication)&&p->RendererRevision.has_value();
    }
    // Stable ordinal cells plus exact-key lookup, detached in O(1) on reset.
    // No native request ever owns an Attempt pointer.
    class AttemptStorage
    {
        struct State {std::map<size_t,Attempt> Cells;std::map<D::Key,size_t,D::KeyLess> Index;State* Next{};};
        State* Active{};State* Head{};State* Tail{};size_t Borrows{};
    public:
        AttemptStorage()=default;AttemptStorage(const AttemptStorage&)=delete;
        struct Borrow {AttemptStorage& Owner;explicit Borrow(AttemptStorage& owner):Owner(owner){++Owner.Borrows;}~Borrow(){--Owner.Borrows;}Borrow(const Borrow&)=delete;};
        size_t size()const noexcept{return Active?Active->Cells.size():0;}
        Attempt& operator[](size_t i){return Active->Cells.at(i);}
        const Attempt& operator[](size_t i)const{return Active->Cells.at(i);}
        Attempt* Find(const D::Key& key)
        {if(!Active)return nullptr;auto it=Active->Index.find(key);return it==Active->Index.end()?nullptr:&Active->Cells.at(it->second);}
        const Attempt* Find(const D::Key& key)const
        {if(!Active)return nullptr;auto it=Active->Index.find(key);return it==Active->Index.end()?nullptr:&Active->Cells.at(it->second);}
        Attempt& Insert(const D::Candidate& candidate)
        {
            if(!Active)Active=new State;
            const auto ordinal=Active->Cells.size();
            Attempt prepared;prepared.Candidate=candidate;
            auto [cell,added]=Active->Cells.emplace(ordinal,std::move(prepared));
            if(!added)throw std::logic_error("validation ordinal collision");
            try{auto [index,inserted]=Active->Index.emplace(candidate.Identity,ordinal);
                if(!inserted)throw std::logic_error("duplicate validation identity");}
            catch(...){Active->Cells.erase(cell);throw;}
            return cell->second;
        }
        void Retire()noexcept
        {if(!Active)return;if(Tail)Tail->Next=Active;else Head=Active;Tail=Active;Active=nullptr;}
        size_t Maintain(size_t limit=16,double budgetMs=2.0)
        {
            if(Borrows)return 0;
            const auto start=std::chrono::steady_clock::now();size_t units=0;
            while(Head&&units<limit)
            {
                auto* state=Head;
                if(!state->Cells.empty()){auto it=state->Cells.begin();state->Index.erase(it->second.Candidate.Identity);state->Cells.erase(it);}
                if(state->Cells.empty()){Head=state->Next;if(!Head)Tail=nullptr;delete state;}
                ++units;if(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()>=budgetMs)break;
            }
            return units;
        }
        ~AttemptStorage(){Retire();while(Maintain(16,(std::numeric_limits<double>::max)())){}} // Final shutdown only.
    };
    inline std::atomic<uint64_t> NextExactOwner{};
    inline uint64_t AllocateExactOwner()noexcept
    {auto value=NextExactOwner.load();while(value!=UINT64_MAX)if(NextExactOwner.compare_exchange_weak(value,value+1))return value+1;return 0;}
    enum class ExactState { Completed,Deferred,Interrupted,Stale,Cancelled };
    enum class ExactReason { None,Generation,Publication,AdoptionPending,MissingKey,RecordChanged,AlreadyCompleted,
        RecoveryPending,SourceUnavailable,SourceInUse,RepresentativePending,RepresentativeUnavailable,Busy,AllocationFailure,Budget,CounterExhausted,TicketChanged };
    struct ExactServiceResult {ExactState Status=ExactState::Deferred;ExactReason Why=ExactReason::None;bool Invoked=false;};
    template<class Snapshot> struct Scheduler
    {
        AttemptStorage Entries;size_t Count{},Cursor{},CpuVisits{},NativeAudits{},LastCpuVisits{},QueueDeferred{};
        uint64_t Generation{};double SourcePublication{};
        const uint64_t ExactOwner=AllocateExactOwner();
        uint64_t ExactEpoch=1; // Never reset to an earlier epoch, including Clear/reuse of the same world key.

        bool Preparing=false,QueueTruncated=false,ScanTruncated=false;
        D::Key LastCompletedKey;std::shared_ptr<const Snapshot> Held;
        using RegistryPublication=std::shared_ptr<const D::Registry>;
        struct ExactRequest
        {
            D::Key Key;RegistryPublication Registry;std::shared_ptr<const Snapshot> Source;
            uint64_t Owner{},Epoch{},Membership{},RecordSerial{};double SourcePublication{};
            bool RepresentativesOnly=false;
        };
        struct ExactPreparation {ExactServiceResult Result;std::optional<ExactRequest> Request;};
        // One pinned work plan, one latest target, one committed result view.
        // Supersession coalesces the target; it never restarts in-flight adoption.
        RegistryPublication PendingRegistry,AdoptedRegistry,LatestRegistry;
        size_t AdoptionCursor{},LastAdoptionVisits{},LastAdoptionNew{},AdoptionFailures{},CompletedCurrent{},AdoptionDone{},AuditCursor{},LastSelectionVisits{},ScanFailures{};
        uint64_t NextEpoch{},PendingEpoch{},CurrentEpoch{},ScanEpoch{};
        double AdoptionPublication{},BlockedSourcePublication{};
        bool Adopting=false,AdoptionBlocked=false,AdoptionAllocationBlocked=false,HasAdoptionPublication=false,WaitingForSource=false,ScanAllocationBlocked=false;
        std::array<size_t,5> Outcomes{},AdoptionOutcomes{};
        size_t TotalIdentityVisits{},TotalGraphVisits{},TotalAuditVisits{};
        size_t MaintainRetired(size_t limit=16,double budgetMs=2.0){return Entries.Maintain(limit,budgetMs);}
        void Clear()noexcept
        {
            if(ExactEpoch)ExactEpoch=ExactEpoch==UINT64_MAX?0:ExactEpoch+1;
            Entries.Retire();Count=Cursor=CpuVisits=NativeAudits=LastCpuVisits=QueueDeferred=0;
            Generation=0;SourcePublication=0;Preparing=QueueTruncated=ScanTruncated=false;
            LastCompletedKey={};Held.reset();PendingRegistry.reset();AdoptedRegistry.reset();LatestRegistry.reset();
            AdoptionCursor=LastAdoptionVisits=LastAdoptionNew=AdoptionFailures=CompletedCurrent=AdoptionDone=AuditCursor=LastSelectionVisits=ScanFailures=0;
            NextEpoch=PendingEpoch=CurrentEpoch=ScanEpoch=0;AdoptionPublication=BlockedSourcePublication=0;
            Adopting=AdoptionBlocked=AdoptionAllocationBlocked=HasAdoptionPublication=WaitingForSource=ScanAllocationBlocked=false;
            Outcomes={};AdoptionOutcomes={};TotalIdentityVisits=TotalGraphVisits=TotalAuditVisits=0;
        }
        bool ReadyFor(const D::Registry& r)const noexcept
        {return !Adopting&&!AdoptionBlocked&&AdoptedRegistry==LatestRegistry&&AdoptedRegistry.get()==&r&&r.Complete()&&Generation==r.Generation;}
        bool IsCurrent(const Attempt& e)const noexcept{return CurrentEpoch&&e.MembershipEpoch==CurrentEpoch;}
        // FIFO-only acknowledgement: the record must have been visited in the
        // exact latest plan, not merely retained from an older pinned plan.
        uint64_t AdoptionEpoch(const D::Key& key,const RegistryPublication& registry)const noexcept
        {
            if(AdoptionBlocked||!registry||registry!=LatestRegistry||!registry->Complete()||registry->Generation!=Generation||key.Generation!=Generation||
                registry->Index.find(key)==registry->Index.end())return 0;
            const auto* e=Entries.Find(key);if(!e)return 0;
            const auto epoch=Adopting?(PendingRegistry==registry?PendingEpoch:0):(AdoptedRegistry==registry?CurrentEpoch:0);
            return epoch&&e->MembershipEpoch==epoch?epoch:0;
        }
        bool RequestExactReevaluation(const D::Key& key,const RegistryPublication& registry)
        {
            const auto epoch=AdoptionEpoch(key,registry);if(!epoch)return false;
            auto* e=Entries.Find(key);if(e->Auditing||e->AuditSerial==UINT64_MAX)return false;
            if(e->Done){auto& count=Adopting?AdoptionDone:CompletedCurrent;auto& outcomes=Adopting?AdoptionOutcomes:Outcomes;
                if(!count||!outcomes[size_t(e->Validation.Status)])throw std::logic_error("validation completion counter underflow");
                --count;--outcomes[size_t(e->Validation.Status)];e->Done=false;}
            ++e->AuditSerial;e->Provenance.reset();e->RecoveryRequired=false;e->MissingRepresentative=false;
            Held.reset();Preparing=false;WaitingForSource=false;return true;
        }
        bool RecordRecipeRejection(const D::Key& key,const RegistryPublication& registry,uint64_t serial,Result& rejection)
        {
            // Exact current audit + proven non-admission is checked by the caller.
            // Retain the failed evidence, not a permanent negative cache: no existing
            // provider necessarily observes the assignments that made it invalid.
            if(!AdoptionEpoch(key,registry)||rejection.Disposition!=AuditDisposition::Completed||
                rejection.Handoff.Handoff!=MiniMapSpriteCapture::HandoffOutcome::Rejected||
                rejection.Handoff.Why!=MiniMapSpriteCapture::Cause::RecipeInvalid||
                rejection.RecipeCheck.Failure==MiniMapStaticAssembly::RecipeFailure::None)return false;
            auto* e=Entries.Find(key);
            if(!e||!e->Done||e->Auditing||e->AuditSerial!=serial||e->Validation.Status!=Outcome::Eligible)return false;
            auto& outcomes=Adopting?AdoptionOutcomes:Outcomes;
            if(!outcomes[size_t(Outcome::Eligible)])throw std::logic_error("validation recipe rejection counter underflow");
            --outcomes[size_t(Outcome::Eligible)];++outcomes[size_t(Outcome::Conditional)];
            e->Validation.Status=Outcome::Conditional;e->Validation.RecipeCheck=rejection.RecipeCheck;
            e->Validation.Handoff=rejection.Handoff;e->Validation.Reason.swap(rejection.Reason);return true;
        }
        bool RequestReevaluation(const D::Key& key)
        {
            if(Adopting||AdoptionBlocked||!AdoptedRegistry||key.Generation!=Generation)return false;
            auto* e=Entries.Find(key);if(!e||!IsCurrent(*e)||e->Auditing)return false;
            if(e->AuditSerial==UINT64_MAX)return false;
            if(e->Done){if(!CompletedCurrent||!Outcomes[size_t(e->Validation.Status)])throw std::logic_error("validation completion counter underflow");--CompletedCurrent;--Outcomes[size_t(e->Validation.Status)];e->Done=false;}
            ++e->AuditSerial;e->Provenance.reset();
            e->RecoveryRequired=false;e->MissingRepresentative=false;Held.reset();Preparing=false;WaitingForSource=false;return true;
        }
        bool BeginAdoption()
        {
            if(NextEpoch==UINT64_MAX){AdoptionBlocked=true;return false;}
            PendingEpoch=++NextEpoch;PendingRegistry=LatestRegistry;Adopting=true;
            AdoptionAllocationBlocked=false;AdoptionCursor=0;AdoptionDone=0;AdoptionOutcomes={};return true;
        }
        bool Observe(const RegistryPublication& r,uint64_t currentGeneration)
        {
            if(!r||!currentGeneration||r->Generation!=currentGeneration||!r->Complete()||
                !std::get_deleter<D::RetireRegistry>(r)||!std::isfinite(r->PublicationSeconds))
            {AdoptionBlocked=true;return false;}
            if(Generation!=currentGeneration){Clear();Generation=currentGeneration;}
            if(HasAdoptionPublication&&r->PublicationSeconds<AdoptionPublication){AdoptionBlocked=true;return false;}
            AdoptionBlocked=false;if(r==LatestRegistry)return Adopting||AdoptedRegistry==r?true:BeginAdoption();
            LatestRegistry=r;AdoptionPublication=r->PublicationSeconds;HasAdoptionPublication=true;
            Held.reset();Preparing=false;WaitingForSource=false;Cursor=0;
            return Adopting?true:BeginAdoption();
        }
        bool ServiceAdoption(double budgetMs=2.0)
        {
            const auto start=std::chrono::steady_clock::now();LastAdoptionVisits=LastAdoptionNew=0;
            if(!Adopting||AdoptionBlocked||!PendingRegistry)return false;AdoptionAllocationBlocked=false;
            while(AdoptionCursor<PendingRegistry->Count&&LastAdoptionVisits<D::VisitsPerTick&&LastAdoptionNew<D::NewPerTick&&
                std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<budgetMs)
            {
                const auto& c=PendingRegistry->Entries[AdoptionCursor];auto* e=Entries.Find(c.Identity);
                if(!e){try{e=&Entries.Insert(c);Count=Entries.size();++LastAdoptionNew;}
                    catch(const std::bad_alloc&){++AdoptionFailures;AdoptionAllocationBlocked=true;return false;}}
                e->Candidate=c;e->MembershipEpoch=PendingEpoch;
                if(e->Done){++AdoptionDone;++AdoptionOutcomes[size_t(e->Validation.Status)];}
                ++AdoptionCursor;++LastAdoptionVisits;
            }
            if(AdoptionCursor<PendingRegistry->Count)return false;
            AdoptedRegistry.swap(PendingRegistry);PendingRegistry.reset();Adopting=false;
            CurrentEpoch=PendingEpoch;CompletedCurrent=AdoptionDone;Outcomes=AdoptionOutcomes;AuditCursor=0;
            if(AdoptedRegistry!=LatestRegistry){BeginAdoption();return false;}
            return true;
        }
        bool Complete()const{return !Adopting&&!AdoptionBlocked&&AdoptedRegistry&&AdoptedRegistry==LatestRegistry&&CompletedCurrent==AdoptedRegistry->Count;}
        size_t Processed()const{return CompletedCurrent;}
        // Legacy entry point retains registry-order selection and the same scan/recovery authority.
        template<class Get,class Audit> bool Service(const D::Registry& registry,std::shared_ptr<const Snapshot> latest,Get get,Audit audit,double budgetMs=2.0)
        {return ServiceImpl(registry,std::move(latest),std::move(get),std::move(audit),budgetMs);}
        ExactPreparation PrepareExact(const D::Key& key,const RegistryPublication& registry,std::shared_ptr<const Snapshot> latest,bool representativesOnly=false)
        {
            if(!ExactOwner||!ExactEpoch||!Generation||key.Generation!=Generation)return {{ExactState::Cancelled,ExactReason::Generation},{}};
            if(!registry||registry!=LatestRegistry||registry->Generation!=Generation||!registry->Complete())return {{ExactState::Stale,ExactReason::Publication},{}};
            if(registry->Index.find(key)==registry->Index.end())return {{ExactState::Stale,ExactReason::MissingKey},{}};
            const auto epoch=AdoptionEpoch(key,registry);
            if(!epoch)return {{ExactState::Deferred,ExactReason::AdoptionPending},{}};
            auto* entry=Entries.Find(key);
            if(!entry||entry->MembershipEpoch!=epoch)return {{ExactState::Stale,ExactReason::RecordChanged},{}};
            if(entry->Auditing)return {{ExactState::Deferred,ExactReason::Busy},{}};
            if(representativesOnly&&!ReusableEligibleEvidence(*entry,Generation))return {{ExactState::Deferred,ExactReason::RecordChanged},{}};
            if(entry->Done&&!representativesOnly){
                if(entry->AuditedMembershipEpoch!=epoch||entry->AuditedSourcePublication<registry->PublicationSeconds)
                    return {{ExactState::Stale,ExactReason::RecordChanged},{}};
                return {{ExactState::Completed,ExactReason::AlreadyCompleted},{}};
            }
            if(entry->AuditSerial==UINT64_MAX)return {{ExactState::Deferred,ExactReason::CounterExhausted},{}};
            if(!latest||!latest->Complete||latest->Generation!=Generation||!std::isfinite(latest->PublishedAtSeconds)||latest->PublishedAtSeconds<registry->PublicationSeconds)
                return {{ExactState::Deferred,ExactReason::SourceUnavailable},{}};
            if(entry->RecoveryRequired){
                if(latest->PublishedAtSeconds<=entry->RecoveryPublication)return {{ExactState::Deferred,ExactReason::RecoveryPending},{}};
                // A meaningful new source may wake recovery. Never restart an unfinished scan.
                if(Held&&Held->PublishedAtSeconds<=entry->RecoveryPublication){
                    if(Preparing)return {{ExactState::Deferred,ExactReason::SourceInUse},{}};
                    Held.reset();WaitingForSource=false;
                }
            }
            if(entry->MissingRepresentative&&Held&&!Preparing&&latest->PublishedAtSeconds>Held->PublishedAtSeconds){Held.reset();WaitingForSource=false;}
            if(Held&&!Preparing&&!entry->MissingRepresentative&&entry->RepresentativeEpoch!=ScanEpoch){Held.reset();WaitingForSource=false;}
            auto source=Held?Held:std::move(latest);
            if(entry->RecoveryRequired&&source->PublishedAtSeconds<=entry->RecoveryPublication)return {{ExactState::Deferred,ExactReason::RecoveryPending},{}};
            return {{ExactState::Deferred,ExactReason::RepresentativePending},
                ExactRequest{key,registry,source,ExactOwner,ExactEpoch,epoch,entry->AuditSerial,source->PublishedAtSeconds,representativesOnly}};
        }
        // Audit-only callback contract: use the existing representation auditor, never a capture offer.
        // Sources are the existing immutable/retirement-backed collector aliases; no native state is retained.
        template<class Get,class Audit> ExactServiceResult ServiceExact(const ExactRequest& request,Get get,Audit audit,double budgetMs=2.0)
        {
            if(!ExactOwner||request.Owner!=ExactOwner||!ExactEpoch||request.Epoch!=ExactEpoch||request.Key.Generation!=Generation)return {ExactState::Cancelled,ExactReason::Generation};
            if(!request.Registry||request.Registry!=LatestRegistry||!request.Registry->Complete())return {ExactState::Stale,ExactReason::Publication};
            if(request.Registry->Index.find(request.Key)==request.Registry->Index.end())return {ExactState::Stale,ExactReason::MissingKey};
            const auto epoch=AdoptionEpoch(request.Key,request.Registry);
            if(!epoch)return {ExactState::Deferred,ExactReason::AdoptionPending};
            auto* entry=Entries.Find(request.Key);
            if(!entry||request.Membership!=epoch||entry->MembershipEpoch!=epoch||entry->AuditSerial!=request.RecordSerial)return {ExactState::Stale,ExactReason::RecordChanged};
            if(entry->Auditing)return {ExactState::Deferred,ExactReason::Busy};
            if(request.RepresentativesOnly&&!ReusableEligibleEvidence(*entry,Generation))return {ExactState::Stale,ExactReason::RecordChanged};
            if(entry->Done&&!request.RepresentativesOnly)return {ExactState::Completed,ExactReason::AlreadyCompleted};
            if(!request.Source||!request.Source->Complete||request.Source->Generation!=Generation||
                !std::isfinite(request.SourcePublication)||request.Source->PublishedAtSeconds!=request.SourcePublication||request.SourcePublication<request.Registry->PublicationSeconds)
                return {ExactState::Stale,ExactReason::SourceUnavailable};
            if(Held&&Held!=request.Source)return {ExactState::Deferred,ExactReason::SourceInUse};
            if(entry->RecoveryRequired&&request.SourcePublication<=entry->RecoveryPublication)return {ExactState::Deferred,ExactReason::RecoveryPending};
            bool invoked=false;
            try{ServiceImpl(*request.Registry,request.Source,std::move(get),std::move(audit),budgetMs,&request,&invoked);}
            catch(const std::bad_alloc&){return {ExactState::Deferred,ExactReason::AllocationFailure,invoked};}
            if(request.Epoch!=ExactEpoch||request.Key.Generation!=Generation)return {ExactState::Cancelled,ExactReason::Generation,invoked};
            if(request.Registry!=LatestRegistry||AdoptionEpoch(request.Key,request.Registry)!=request.Membership)return {ExactState::Stale,ExactReason::Publication,invoked};
            entry=Entries.Find(request.Key);
            if(!entry||entry->MembershipEpoch!=request.Membership)return {ExactState::Stale,ExactReason::RecordChanged,invoked};
            if(invoked)return {entry->Validation.Disposition==AuditDisposition::Interrupted?ExactState::Interrupted:
                entry->Validation.Disposition==AuditDisposition::Deferred?ExactState::Deferred:ExactState::Completed,ExactReason::None,true};
            if(ScanAllocationBlocked)return {ExactState::Deferred,ExactReason::AllocationFailure};
            if(Preparing)return {ExactState::Deferred,ExactReason::RepresentativePending};
            if(entry->RepresentativeEpoch!=ScanEpoch||!entry->InstanceCount)return {ExactState::Deferred,ExactReason::RepresentativeUnavailable};
            if(request.RepresentativesOnly)return {ExactState::Completed,ExactReason::AlreadyCompleted};
            return {ExactState::Deferred,ExactReason::Budget};
        }
    private:
        template<class Get,class Audit> bool ServiceImpl(const D::Registry& registry,std::shared_ptr<const Snapshot> latest,Get get,Audit audit,double budgetMs,const ExactRequest* exact=nullptr,bool* invoked=nullptr)
        {
            const auto start=std::chrono::steady_clock::now();LastCpuVisits=LastSelectionVisits=0;ScanAllocationBlocked=false;
            if((exact?AdoptionEpoch(exact->Key,exact->Registry)!=exact->Membership:!ReadyFor(registry))||!latest||latest->Generation!=Generation||!latest->Complete||
                latest->PublishedAtSeconds<registry.PublicationSeconds||(!exact&&Complete()))return false;
            if(!Held)
            {
                if(WaitingForSource&&latest->PublishedAtSeconds<=BlockedSourcePublication)return false;
                if(ScanEpoch==UINT64_MAX){AdoptionBlocked=true;return false;}
                ++ScanEpoch;Held=std::move(latest);Preparing=true;WaitingForSource=false;Cursor=AuditCursor=0;SourcePublication=Held->PublishedAtSeconds;
            }
            if(Preparing)
            {
                while(Cursor<Held->Records.size()&&LastCpuVisits<D::VisitsPerTick&&
                    std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<budgetMs)
                {
                    try{
                        const auto r=get(Held->Records[Cursor]);
                        if(r.DefinitionValid&&r.VisualInputsValid)if(auto* e=Entries.Find(r.Identity);e&&(exact?AdoptionEpoch(r.Identity,exact->Registry)!=0:IsCurrent(*e))&&
                            (!e->Done||(exact&&exact->RepresentativesOnly&&r.Identity==exact->Key))&&
                            (!e->RecoveryRequired||SourcePublication>e->RecoveryPublication))
                        {
                            // A new complete source wakes interrupted work even
                            // before renderer dependency tracking exists. The
                            // unchanged source never causes automatic reaudits.
                            e->RecoveryRequired=false;
                            if(e->RepresentativeEpoch!=ScanEpoch){e->RepresentativeEpoch=ScanEpoch;e->InstanceCount=0;}
                            if(e->InstanceCount<2)e->Indices[e->InstanceCount++]=Cursor;
                        }
                    }catch(const std::bad_alloc&){ScanAllocationBlocked=true;++ScanFailures;return false;}
                    ++Cursor;++LastCpuVisits;++CpuVisits;
                }
                Preparing=Cursor<Held->Records.size();if(Preparing)return false;
            }
            // Refresh CPU representatives only. Never invoke an audit or mutate its
            // receipt/counts/proofs merely to acknowledge a newer publication.
            if(exact&&exact->RepresentativesOnly)return false;
            while((exact?LastSelectionVisits==0:AuditCursor<registry.Count)&&LastSelectionVisits<D::VisitsPerTick&&
                std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<budgetMs)
            {
                Attempt* e=nullptr;
                if(exact)e=Entries.Find(exact->Key);
                else e=Entries.Find(registry.Entries[AuditCursor++].Identity);
                ++LastSelectionVisits;
                if(!e||(exact?e->MembershipEpoch!=exact->Membership:!IsCurrent(*e))||e->Done||e->RecoveryRequired||e->Auditing)continue;
                if(exact&&e->AuditSerial!=exact->RecordSerial)return false;
                if(e->AuditSerial==UINT64_MAX)return false;
                if(e->RepresentativeEpoch!=ScanEpoch||!e->InstanceCount){e->MissingRepresentative=true;continue;}
                e->MissingRepresentative=false;++NativeAudits;
                // Native calls may reenter a world/source transition. Pin CPU
                // retirement and source ownership for the synchronous borrow,
                // then revalidate before mutating completion state.
                AttemptStorage::Borrow borrow(Entries);const auto source=Held;
                const auto publication=exact?exact->Registry:AdoptedRegistry;const auto world=Generation;const auto epoch=exact?exact->Membership:CurrentEpoch;const auto key=e->Candidate.Identity;const auto auditSourcePublication=SourcePublication;
                auto current=[&]{return Generation==world&&Entries.Find(key)==e&&(exact?
                    AdoptionEpoch(key,publication)==epoch:CurrentEpoch==epoch&&AdoptedRegistry==publication&&ReadyFor(registry));};
                e->Auditing=true;++e->AuditSerial;e->Provenance.reset();
                struct AuditScope {Attempt& Record;~AuditScope(){Record.Auditing=false;}} auditScope{*e};
                if(invoked)*invoked=true;
                try {auto result=audit(source->Records[e->Indices[0]],e->InstanceCount>1?&source->Records[e->Indices[1]]:nullptr);
                    if(!current())return false;e->Validation=std::move(result);}
                catch(...){if(!current())return false;e->Validation.MarkInterrupted();}
                e->AuditedMembershipEpoch=epoch;e->AuditedSourcePublication=auditSourcePublication;
                // Deferral reuses source/explicit wakes; no timer retry or FIFO here.
                if(e->Validation.Disposition!=AuditDisposition::Completed)
                {e->RecoveryRequired=true;e->RecoveryPublication=SourcePublication;
                    if(e->Validation.Disposition==AuditDisposition::Interrupted)++ScanFailures;LastCompletedKey=e->Candidate.Identity;return true;}
                // Commit scalar provenance only after the result/current-source checks. No allocation or native entitlement.
                if(e->Validation.Status==Outcome::Eligible)
                    e->Provenance=AuditProvenance{world,e->AuditSerial,epoch,auditSourcePublication,e->Validation.AuditRendererRevision};
                e->Done=true;
                if(Adopting&&epoch==PendingEpoch){++AdoptionDone;++AdoptionOutcomes[size_t(e->Validation.Status)];}
                else {++CompletedCurrent;++Outcomes[size_t(e->Validation.Status)];}
                LastCompletedKey=e->Candidate.Identity;
                TotalIdentityVisits+=e->Validation.IdentityVisits;TotalGraphVisits+=e->Validation.GraphVisits;TotalAuditVisits+=e->Validation.AuditVisits;
                if(Complete())Held.reset();return true;
            }
            // Conditional P2 progress limitation: missing representatives may
            // wait for a newer complete source or explicit reevaluation. Avoiding
            // unchanged-input rescans means recovery need not be immediate; if
            // no trigger arrives, progress may stall without corrupting prior results.
            if(!exact&&AuditCursor==registry.Count){BlockedSourcePublication=SourcePublication;WaitingForSource=true;Held.reset();}
            return false;
        }
    public:
    };
    template<class Snapshot> std::string AdoptionStatusJson(const Scheduler<Snapshot>& s,const D::Registry& registry,const char* stage="R2.6b")
    {
        return J::Object({{"stage",J::Quote(stage)},
            {"world_generation",std::to_string(s.Generation)},
            {"source_registry_adopted",J::Bool(s.ReadyFor(registry))},
            {"completion_status",J::Quote("consumer_execution_deferred")},
            {"adoption_pending",J::Bool(s.Adopting)},
            {"adoption_blocked",J::Bool(s.AdoptionBlocked)},
            {"adoption_cursor",std::to_string(s.AdoptionCursor)},
            {"adoption_allocation_blocked",J::Bool(s.AdoptionAllocationBlocked)},
            {"adoption_allocation_failures",std::to_string(s.AdoptionFailures)},
            {"retained_attempts",std::to_string(s.Count)},
            {"capacity_deferred",std::to_string(s.QueueDeferred)},
            {"representative_record_limit_deferred","false"},
            {"representative_scan_incomplete",J::Bool(s.Preparing)},
            {"scan_allocation_blocked",J::Bool(s.ScanAllocationBlocked)},
            {"waiting_for_source_advance",J::Bool(s.WaitingForSource)},
            {"scan_failures",std::to_string(s.ScanFailures)}});
    }
    inline std::string CandidateJson(const Attempt& e,bool current=false)
    {
        const auto& c=e.Candidate;const auto& r=e.Validation;
        auto details=r.Details.size()<=DetailLimit?r.Details:"{\"truncated\":true}";
        return J::Object({{"session_local_candidate_key",J::Quote(D::KeyText(c.Identity))},
            {"building_id",std::to_string(c.Identity.Building)},{"observed_instance_count",std::to_string(c.Instances)},
            {"definition_object_index",std::to_string(c.Identity.DefinitionObject)},
            {"definition_fname_index",std::to_string(c.Identity.NameIndex)},{"definition_fname_number",std::to_string(c.Identity.NameNumber)},
            {"key_current",J::Bool(current)},{"processed",J::Bool(e.Done)},{"representative_unavailable",J::Bool(e.MissingRepresentative)},{"recovery_required",J::Bool(e.RecoveryRequired)},
            {"audit_disposition",J::Quote(DispositionLabel(r.Disposition,e.Done))},
            {"capture_handoff",J::Quote(MiniMapSpriteCapture::Label(r.Handoff.Handoff))},
            {"recipe_failure",r.RecipeCheck.Failure==MiniMapStaticAssembly::RecipeFailure::None?"null":J::Quote(MiniMapStaticAssembly::Label(r.RecipeCheck.Failure))},
            {"recipe_failure_instance",r.RecipeCheck.Instance==SIZE_MAX?"null":std::to_string(r.RecipeCheck.Instance)},
            {"capture_admission",r.Handoff.Admission?std::to_string(int(*r.Handoff.Admission)):"null"},
            {"audit_wait",std::to_string(unsigned(r.Wait))},
            {"recovery_trigger",J::Quote(e.RecoveryRequired?"newer_complete_source_or_explicit_reevaluation":"none")},
            {"recovery_source_publication",J::Number(e.RecoveryPublication)},
            {"human_readable_name","null"},{"name_provenance",J::Quote("no verified bounded runtime name conversion; offline enum labels are not asset display names")},
            {"classification",J::Quote("unknown")},{"classification_provenance",J::Quote("collector membership does not establish ownership or spawn origin")},
            {"experimental_eligibility",e.Done?J::Quote(Label(r.Status)):J::Quote("deferred")},
            {"reason",J::Quote(r.Reason)}, {"selected_visual_route",J::Quote(r.Route)},
            {"high_detail_actor_status",J::Quote(r.High)},{"low_detail_actor_status",J::Quote(r.Low)},
            {"mass_alternative_status",J::Quote(r.Mass)},{"cached_entity_config",J::Quote(r.Config)},
            {"authored_entity_config",J::Quote("not_examined_soft_reference_contract_unverified")},
            {"placement_actor_class",J::Quote(r.AuthoredClass)},
            {"selected_class",r.Selected},{"authored_static_instances",std::to_string(r.Static)},
            {"unique_static_meshes",std::to_string(r.UniqueMeshes)},{"skeletal_components",std::to_string(r.Skeletal)},
            {"nonmesh_scene_components",std::to_string(r.Scene)},{"non_scene_components",std::to_string(r.NonScene)},
            {"scs_nodes",std::to_string(r.Nodes)},{"material_slots",std::to_string(r.MaterialSlots)},{"override_slots",std::to_string(r.OverrideSlots)},
            {"native_root_verified",J::Bool(r.Root)},{"effective_transforms_verified",J::Bool(r.Transforms)},
            {"stored_material_mapping_verified",J::Bool(r.Materials)},{"resident_representation_validated",J::Bool(r.Resident)},
            {"inherited_selection_unverified",J::Bool(r.Inherited)},{"child_contributions_unresolved",J::Bool(r.Child)},
            {"other_skinned_contribution_unresolved",J::Bool(r.OtherSkinned)},
            {"derived_static_component_semantics_unverified",J::Bool(r.DerivedStatic)},
            {"instance_specific_spline",J::Bool(c.Identity.Spline)},
            {"identity_visits",std::to_string(r.IdentityVisits)},{"graph_visits",std::to_string(r.GraphVisits)},
            {"audit_visits",std::to_string(r.AuditVisits)},{"representative_probes",std::to_string(r.Probes)},
            {"operational_incomplete",J::Bool(r.Truncated)},{"diagnostic_truncated",J::Bool(r.DiagnosticTruncated||r.Details.size()>DetailLimit)},
            {"truncated",J::Bool(r.Truncated||r.DiagnosticTruncated||r.Details.size()>DetailLimit)},{"verified_safe_to_capture","false"},
            {"authored_audit",details},{"static_component_inspection",r.Inspection},{"validation_profile",r.Profile},
            {"cooked_geometry",r.CookedGeometry},{"skeletal_source_metadata",r.SkeletalMetadata}});
    }
    template<class Snapshot> std::string Json(const Scheduler<Snapshot>& s,const D::Registry& registry,size_t limit=OutputLimit)
    {
        limit=(std::min)(limit,OutputLimit);std::string records;size_t emitted=0;auto outcomes=s.Outcomes;
        if(limit<4096)return "{\"stage\":\"R2.6b\",\"output_truncated\":true,\"new_sprites_generated\":0}";
        size_t work=s.TotalIdentityVisits,graphs=s.TotalGraphVisits,audits=s.TotalAuditVisits;
        for(size_t i=0;i<s.Count&&i<D::VisitsPerTick;++i)
        {
            const auto& e=s.Entries[i];

            const auto record=CandidateJson(e,s.IsCurrent(e));
            if(records.size()+record.size()+4096>limit)break;
            if(emitted)records+=',';records+=record;++emitted;
        }
        return J::Object({{"schema_version","1"},{"stage",J::Quote("R2.6b")},{"game_build",J::Quote("CL-127004")},
            {"world_generation",std::to_string(s.Generation)},{"source_publication_steady_seconds",J::Number(s.SourcePublication)},
            {"discovered_candidates",std::to_string(registry.Count)},{"processed",std::to_string(s.Processed())},
            {"validated",std::to_string(outcomes[0])},{"conditional",std::to_string(outcomes[1])},
            {"unsupported",std::to_string(outcomes[2])},{"no_resident_representation",std::to_string(outcomes[3])},
            {"invalid_or_stale",std::to_string(outcomes[4])},
            {"remaining_or_deferred",std::to_string(registry.Count-(std::min)(registry.Count,s.Processed()))},
            {"completion_status",J::Quote(s.Generation&&s.ReadyFor(registry)&&s.Complete()?"complete":"partial_or_invalidated")},
            {"source_registry_complete",J::Bool(registry.Complete())},
            {"source_registry_adopted",J::Bool(s.ReadyFor(registry))},
            {"adoption_pending",J::Bool(s.Adopting)},
            {"adoption_cursor",std::to_string(s.AdoptionCursor)},
            {"adoption_allocation_blocked",J::Bool(s.AdoptionAllocationBlocked)},
            {"adoption_allocation_failures",std::to_string(s.AdoptionFailures)},{"queue_truncated",J::Bool(s.QueueTruncated)},
            {"representative_scan_truncated",J::Bool(s.ScanTruncated)},{"output_truncated",J::Bool(emitted<s.Count)},
            {"diagnostic_record_visit_limit",std::to_string(D::VisitsPerTick)},{"retained_attempts",std::to_string(s.Count)},{"work_counter_scope",J::Quote("returned_native_results_generation_cumulative")},{"candidate_records_emitted",std::to_string(emitted)},{"cpu_record_visits",std::to_string(s.CpuVisits)},
            {"native_candidate_audits",std::to_string(s.NativeAudits)}, {"identity_visits",std::to_string(work)},
            {"graph_visits",std::to_string(graphs)},{"audit_visits",std::to_string(audits)},
            {"new_sprites_generated","0"},{"capture_enabled","false"},{"candidates","["+records+"]"}});
    }
    template<class Snapshot> std::string ClassificationJson(const Scheduler<Snapshot>& s,const D::Registry& registry,size_t limit=OutputLimit)
    {
        limit=(std::min)(limit,OutputLimit);
        if(limit<4096)return "{\"stage\":\"R2.6b.4\",\"output_truncated\":true,\"capture_enabled\":false}";
        std::string rows;size_t emitted=0,unknown=0,include=0,exclude=0,invalid=0,visits=0,tags=0,invalidSource=0;
        // Output detail limits are independent of production/input completeness.
        bool truncated=s.QueueTruncated||s.ScanTruncated;
        for(size_t i=0;i<s.Count&&i<D::VisitsPerTick;++i)
        {
            const auto& e=s.Entries[i];if(!s.IsCurrent(e))continue;
            const auto& o=e.Validation.Classification;
            if(e.Done)
            {
                using E=MiniMapSpriteClassification::Exclusion;
                unknown+=o.CachedPolicy.ExcludeFromMiniMap==E::Unknown;include+=o.CachedPolicy.ExcludeFromMiniMap==E::Include;
                exclude+=o.CachedPolicy.ExcludeFromMiniMap==E::Exclude;invalid+=!o.Fixed;
                invalidSource+=e.Validation.Status==Outcome::Invalid;
            }
            truncated|=o.Truncated;visits+=o.IdentityVisits;tags+=o.TagVisits;
            const auto row=J::Object({{"session_local_candidate_key",J::Quote(D::KeyText(e.Candidate.Identity))},
                {"building_id",std::to_string(e.Candidate.Identity.Building)},
                {"definition_object_index",std::to_string(e.Candidate.Identity.DefinitionObject)},
                {"definition_fname_index",std::to_string(e.Candidate.Identity.NameIndex)},
                {"definition_fname_number",std::to_string(e.Candidate.Identity.NameNumber)},
                {"observed_instance_count",std::to_string(e.Candidate.Instances)},{"processed",J::Bool(e.Done)},{"representative_unavailable",J::Bool(e.MissingRepresentative)},{"recovery_required",J::Bool(e.RecoveryRequired)},
                {"audit_disposition",J::Quote(DispositionLabel(e.Validation.Disposition,e.Done))},
                {"recovery_trigger",J::Quote(e.RecoveryRequired?"newer_complete_source_or_explicit_reevaluation":"none")},
                {"observation",MiniMapSpriteClassification::Json(o)}});
            if(rows.size()+row.size()+4096>limit)break;
            if(emitted++)rows+=',';rows+=row;
        }
        return J::Object({{"schema_version","1"},{"stage",J::Quote("R2.6b.4")},{"game_build",J::Quote("CL-127004")},
            {"world_generation",std::to_string(s.Generation)},{"source_publication_steady_seconds",J::Number(s.SourcePublication)},
            {"candidates_discovered",std::to_string(registry.Count)},{"candidates_processed",std::to_string(s.Processed())},
            {"candidates_remaining",std::to_string(registry.Count-(std::min)(registry.Count,s.Processed()))},
            {"exclude",std::to_string(exclude)},{"include",std::to_string(include)},{"unknown",std::to_string(unknown)},
            {"invalid_or_unavailable",std::to_string(invalid)},{"identity_visits",std::to_string(visits)},
            {"invalid_or_stale_source",std::to_string(invalidSource)},{"tag_visits",std::to_string(tags)},
            {"native_candidate_audits",std::to_string(s.NativeAudits)},{"cpu_record_visits",std::to_string(s.CpuVisits)},
            {"completion_status",J::Quote(s.Generation&&s.ReadyFor(registry)&&s.Complete()?"complete":"partial_or_invalidated")},
            {"classification_summary_scope",J::Quote("bounded_history_prefix_only")},{"inputs_truncated",J::Bool(truncated)},{"output_truncated",J::Bool(emitted<registry.Count||s.Count>D::VisitsPerTick)},
            {"source_registry_complete",J::Bool(registry.Complete())},
            {"source_registry_adopted",J::Bool(s.ReadyFor(registry))},
            {"adoption_pending",J::Bool(s.Adopting)},
            {"adoption_cursor",std::to_string(s.AdoptionCursor)},
            {"adoption_allocation_blocked",J::Bool(s.AdoptionAllocationBlocked)},
            {"adoption_allocation_failures",std::to_string(s.AdoptionFailures)},
            {"observation_refresh",J::Quote("once_per_definition_key_per_world_generation; mutable inputs may change; no trusted policy cached")},
            {"candidates_emitted",std::to_string(emitted)},{"policy_observational_only","true"},
            {"new_sprites_generated","0"},{"capture_enabled","false"},{"candidates","["+rows+"]"}});
    }
}
