#pragma once
#include "CaptureWorkLedgerCore.h"

namespace MiniMapCaptureDispatch
{
    namespace D=MiniMapSpriteDiscoveryCore;namespace V=MiniMapSpriteValidation;
    namespace L=MiniMapCaptureLeases;namespace W=MiniMapCaptureWork;
    enum class State { Ready,Deferred,Stale,Cancelled,Terminal };
    enum class Reason { None,Unavailable,Generation,Membership,AdoptionPending,ValidationPending,RecoveryPending,
        ValidationStale,RendererPending,SourceUnknown,SourceChanged,SelectionChanged,ProofUnavailable,NativeOwned,PolicyTerminal };
    struct Readiness {State Status=State::Deferred;Reason Why=Reason::Unavailable;bool NeedsAudit=false;};
    // Scalar CPU evidence copied under renderer owner-access -> publication mutex.
    struct RendererEvidence
    {
        uint64_t Generation{},SourceRevision{},ImageRevision{},Advancement{};
        MiniMapBuildingSpriteCore::State PolicyState=MiniMapBuildingSpriteCore::State::Awaiting;bool HasCachedImage=false;
        bool Current=false,MetadataReady=false,KeyAdopted=false,SourceKnown=false,Terminal=false,InFlight=false,InitialAdmission=false,LeaseHeld=false;
    };
    template<class Snapshot> Readiness Query(const V::Scheduler<Snapshot>& validation,
        const std::shared_ptr<const D::Registry>& latest,const D::Key& key,W::Revision expected,
        const RendererEvidence& renderer,uint64_t currentGeneration,double currentSourcePublication,bool nativeAvailable)noexcept
    {
        if(!currentGeneration||key.Generation!=currentGeneration||validation.Generation!=currentGeneration)
            return {State::Cancelled,Reason::Generation};
        if(!latest||!latest->Complete()||latest->Generation!=currentGeneration||validation.LatestRegistry!=latest||validation.AdoptionBlocked)
            return {State::Deferred,Reason::AdoptionPending};
        if(!std::isfinite(currentSourcePublication)||currentSourcePublication<latest->PublicationSeconds)
            return {State::Deferred,Reason::Unavailable};
        if(latest->Index.find(key)==latest->Index.end())return {State::Stale,Reason::Membership};
        // FIFO preflight only. Legacy runtime service retains its whole-publication gate.
        const auto epoch=validation.AdoptionEpoch(key,latest);
        if(!epoch)return {State::Deferred,Reason::AdoptionPending};
        const auto* attempt=validation.Entries.Find(key);
        if(!attempt||attempt->MembershipEpoch!=epoch)return {State::Deferred,Reason::ValidationPending,true};
        if(attempt->RecoveryRequired||attempt->Validation.Disposition!=V::AuditDisposition::Completed)
            return {State::Deferred,Reason::RecoveryPending,true};
        if(!attempt->Done)return {State::Deferred,Reason::ValidationPending,true};
        // Membership above acknowledges the latest publication. Audit provenance
        // remains the original receipt; transport epochs are not evidence revisions.
        const bool reusable=V::ReusableEligibleEvidence(*attempt,currentGeneration);
        if((!reusable&&(attempt->AuditedMembershipEpoch!=epoch||attempt->AuditedSourcePublication<latest->PublicationSeconds))||
            !std::isfinite(attempt->AuditedSourcePublication)||attempt->AuditedSourcePublication>currentSourcePublication)
            return {State::Deferred,Reason::ValidationStale,true};
        const auto& audit=attempt->Validation;
        if(audit.Status==V::Outcome::Unsupported||audit.Status==V::Outcome::Invalid||
            audit.Classification.CachedPolicy.ExcludeFromMiniMap==MiniMapSpriteClassification::Exclusion::Exclude)
            return {State::Terminal,Reason::PolicyTerminal};
        // Conditional/nonresident audits without tracked dependencies still lack
        // an authoritative residency wake. Do not silently make them permanent.
        if(audit.Status!=V::Outcome::Eligible)return {State::Deferred,Reason::ValidationPending,true};
        if(renderer.Generation!=currentGeneration||!renderer.Current||!renderer.KeyAdopted)
            return {State::Deferred,Reason::RendererPending};
        const auto& provenance=attempt->Provenance;
        if(!provenance||provenance->Generation!=currentGeneration||provenance->Serial!=attempt->AuditSerial||
            provenance->Membership!=attempt->AuditedMembershipEpoch||provenance->SourcePublication!=attempt->AuditedSourcePublication)
            return {State::Deferred,Reason::ValidationStale,true};
        if(!provenance->RendererRevision)return {State::Deferred,Reason::SourceUnknown,true};
        if(expected.Source!=renderer.SourceRevision||*provenance->RendererRevision!=renderer.SourceRevision)
            return {State::Stale,Reason::SourceChanged,true};
        if(expected.Selection!=renderer.ImageRevision)return {State::Stale,Reason::SelectionChanged};
        if(renderer.Terminal)return {State::Terminal,Reason::PolicyTerminal};
        if(renderer.InFlight||renderer.LeaseHeld||!nativeAvailable)return {State::Deferred,Reason::NativeOwned};
        if(renderer.InitialAdmission&&!renderer.Advancement)return {State::Deferred,Reason::ProofUnavailable};
        // Preflight only. Existing source revalidation, Policy::Admit and StartCapture remain mandatory.
        return {State::Ready,Reason::None};
    }
    enum class OwnerState { Unresolved,NotAdmitted,Held,CapturePending,CleanupPending,CertifiedRetired };
    struct OwnerEvidence
    {bool GameThread=false,OwnerMatches=false,RequestMatches=false,CapturePending=false,CleanupPending=false,Uncertain=false;};
    struct Reconciliation {OwnerState Status=OwnerState::Unresolved;bool LeaseHeld=false;};
    inline Reconciliation Reconcile(L::Inspection inspection,const OwnerEvidence& evidence)noexcept
    {
        if(!evidence.GameThread)return {};
        // Per-token resource retirement, not sprite success or global owner availability.
        if(inspection==L::Inspection::CertifiedRetired)return {OwnerState::CertifiedRetired,false};
        if(inspection!=L::Inspection::Held)return {};
        if(evidence.Uncertain||!evidence.OwnerMatches)return {OwnerState::Unresolved,true};
        if(evidence.CleanupPending)return {OwnerState::CleanupPending,true};
        if(!evidence.RequestMatches)return {OwnerState::Unresolved,true};
        return {evidence.CapturePending?OwnerState::CapturePending:OwnerState::Held,true};
    }
    struct OwnerObservation
    {bool QueryAllowed=false,Available=false;L::Token NativeOwner;L::Snapshot Leases;};
    struct AttemptReconciliation {Reconciliation Owner;std::optional<L::Token> Identity;};
    // Exact before/after request fence, not an inference from missing callbacks or elapsed time.
    inline AttemptReconciliation Correlate(const OwnerObservation& before,const OwnerObservation& after,
        const D::Key& key,uint64_t sourceRevision)noexcept
    {
        if(!before.QueryAllowed||!before.Available||before.NativeOwner||before.Leases.State!=L::Ownership::Returned||
            !after.QueryAllowed||!before.Leases.Incarnation||
            before.Leases.Incarnation!=after.Leases.Incarnation)return {};
        if(after.Leases.LastIssuedRequest==before.Leases.LastIssuedRequest){
            if(after.Available&&after.Leases.State==L::Ownership::Returned&&!after.NativeOwner)
                return {{OwnerState::NotAdmitted,false},{}};
            return {};
        }
        if(before.Leases.LastIssuedRequest==UINT64_MAX||after.Leases.LastIssuedRequest!=before.Leases.LastIssuedRequest+1)return {};
        const auto& token=after.Leases.Current;
        if(!token||token.OwnerIncarnation!=after.Leases.Incarnation||token.RequestSerial!=after.Leases.LastIssuedRequest||
            token.Candidate!=key||token.SourceRevision!=sourceRevision)return {};
        // The native wrapper must now query this exact token's authoritative held/returned evidence.
        return {{OwnerState::Unresolved,after.Leases.State==L::Ownership::Held},token};
    }
}
