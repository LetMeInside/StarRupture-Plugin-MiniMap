#pragma once
#include "SpriteDiscoveryCore.h"
#include <limits>
#include <atomic>

namespace MiniMapCaptureLeases
{
    // CPU identity only. Neither a native pointer nor a display-texture reservation.
    struct Token
    {
        uint64_t OwnerIncarnation{}, Generation{}, SourceRevision{}, RequestSerial{}, LeaseSerial{};
        MiniMapSpriteDiscoveryCore::Key Candidate;
        bool operator==(const Token&) const = default;
        explicit operator bool() const noexcept { return OwnerIncarnation && Generation && RequestSerial && LeaseSerial; }
    };
    // These facts are supplied only after the existing native cleanup operations
    // succeed. Publication, cancellation and InFlight=false are not receipts.
    struct RetirementReceipt
    {
        bool JobDestroyed{}, FenceReleased{}, ComponentsReleased{}, RootsReleased{},
            PreparedReleased{}, RequestReleased{}, AssociationCertain{};
        bool Certified() const noexcept
        { return JobDestroyed && FenceReleased && ComponentsReleased && RootsReleased &&
            PreparedReleased && RequestReleased && AssociationCertain; }
    };
    enum class Ownership { Returned, Held };
    enum class Inspection { Unknown,Held,CertifiedRetired };
    struct Snapshot {Token Current,LastRetired;Ownership State=Ownership::Returned;uint64_t Incarnation{},LastIssuedRequest{};bool ObservationPending=false;};
    // One preallocated observation reservation for the single FIFO handoff.
    // It is evidence retention only: never a lease owner, refund or candidate quota.
    struct ObservationTicket
    {uint64_t Incarnation{},Serial{};bool operator==(const ObservationTicket&)const=default;
        explicit operator bool()const noexcept{return Incarnation&&Serial;}};
    enum class ArmState { Armed,Busy,Stale,Exhausted };
    struct ArmResult {ArmState Status=ArmState::Stale;ObservationTicket Ticket;};
    enum class ObservationState { Unknown,AwaitingAdmission,NotAdmitted,Held,CertifiedRetired,Unresolved };
    struct RetirementObservation {ObservationState State=ObservationState::Unknown;Token Lease;bool Sealed=false;};
    // Move-only synchronous handoff fence. Copy the CPU ticket into the work
    // record, but keep this guard on the dispatch stack through return/exception.
    struct ObservationFence
    {
        using Seal=void(*)(const ObservationTicket&) noexcept;
        ObservationTicket Ticket;Seal CloseFunction{};
        ObservationFence()=default;
        ObservationFence(ObservationTicket ticket,Seal close)noexcept:Ticket(ticket),CloseFunction(close){}
        ObservationFence(const ObservationFence&)=delete;ObservationFence& operator=(const ObservationFence&)=delete;
        ObservationFence(ObservationFence&& other)noexcept:Ticket(other.Ticket),CloseFunction(other.CloseFunction){other.CloseFunction=nullptr;}
        ObservationFence& operator=(ObservationFence&&)=delete;
        void Close()noexcept{if(CloseFunction){const auto close=CloseFunction;CloseFunction=nullptr;close(Ticket);}}
        ~ObservationFence(){Close();}
    };
    struct ScopedArmResult {ArmState Status=ArmState::Stale;ObservationTicket Ticket;ObservationFence Fence;};
    inline std::atomic<uint64_t> NextOwnerIncarnation{0};
    class Ledger
    {
        const uint64_t Incarnation=NextOwnerIncarnation.fetch_add(1)+1;
        Token Current,LastRetired;
        uint64_t NextRequest{}, NextLease{};
        Ownership State=Ownership::Returned;
        ObservationTicket Observer;uint64_t NextObservation{},ObservedRequest{};
        MiniMapSpriteDiscoveryCore::Key ObservedKey;
        Token ObservedLease;bool ObservationCertified=false,ObservationUnresolved=false,ObservationClosed=false;
        void ClearObservation()noexcept
        {Observer={};ObservedRequest=0;ObservedKey={};ObservedLease={};ObservationCertified=ObservationUnresolved=ObservationClosed=false;}
    public:
        Ledger() noexcept=default;
        Ledger(const Ledger&)=delete;
        Ledger& operator=(const Ledger&)=delete;
        // One ledger for the verified single native owner; it deliberately
        // survives renderer world resets. No allocation at the handoff boundary.
        bool Acquire(const MiniMapSpriteDiscoveryCore::Key& key,uint64_t source,Token& out) noexcept
        {
            if(!Incarnation || State==Ownership::Held || !key.Generation ||
                NextRequest==(std::numeric_limits<uint64_t>::max)() || NextLease==(std::numeric_limits<uint64_t>::max)())return false;
            Current={Incarnation,key.Generation,source,++NextRequest,++NextLease,key};
            State=Ownership::Held;
            if(Observer&&!ObservationClosed&&!ObservedLease&&!ObservationUnresolved){
                if(Current.RequestSerial==ObservedRequest&&Current.Candidate==ObservedKey)ObservedLease=Current;
                else ObservationUnresolved=true; // Another request cannot impersonate the armed handoff.
            }
            out=Current;return true;
        }
        bool Holds(const Token& token) const noexcept { return bool(token) && State==Ownership::Held && Current==token; }
        bool Return(const Token& token,const RetirementReceipt& receipt) noexcept
        {
            if(!receipt.Certified() || !Holds(token))return false;
            LastRetired=Current;State=Ownership::Returned;
            if(Observer&&ObservedLease==token&&!ObservationUnresolved)ObservationCertified=true;
            return true;
        }
        // The legacy last receipt plus an explicitly armed exact receipt. Both
        // originate exclusively in Return; unknown history remains Unknown.
        Inspection Inspect(const Token& token)const noexcept
        {if(!token)return Inspection::Unknown;if(Holds(token))return Inspection::Held;
            return token==LastRetired||(Observer&&ObservationClosed&&ObservationCertified&&!ObservationUnresolved&&token==ObservedLease)?Inspection::CertifiedRetired:Inspection::Unknown;}
        // Arm BEFORE synchronous handoff, against its exact before-request fence.
        // No native/texture ownership or allocation; reset never clears this slot.
        ArmResult ArmRetirement(const Snapshot& before,const MiniMapSpriteDiscoveryCore::Key& key)noexcept
        {
            if(Observer)return {ArmState::Busy,{}};
            if(!Incarnation||!key.Generation||State!=Ownership::Returned||before.State!=Ownership::Returned||
                before.Incarnation!=Incarnation||before.LastIssuedRequest!=NextRequest||before.Current!=Current||before.LastRetired!=LastRetired)
                return {};
            if(NextObservation==UINT64_MAX||NextRequest==UINT64_MAX)return {ArmState::Exhausted,{}};
            Observer={Incarnation,++NextObservation};ObservedRequest=NextRequest+1;ObservedKey=key;
            ObservedLease={};ObservationCertified=ObservationUnresolved=ObservationClosed=false;return {ArmState::Armed,Observer};
        }
        void SealObservation(const ObservationTicket& ticket)noexcept
        {
            if(!ticket||ticket!=Observer||ObservationClosed)return;
            ObservationClosed=true;
            if(!ObservedLease&&(State!=Ownership::Returned||NextRequest!=ObservedRequest-1))ObservationUnresolved=true;
        }
        RetirementObservation ObserveRetirement(const ObservationTicket& ticket)const noexcept
        {
            if(!ticket||ticket!=Observer)return {};
            if(ObservationUnresolved)return {ObservationState::Unresolved,{},ObservationClosed};
            if(!ObservedLease)return {ObservationClosed?ObservationState::NotAdmitted:ObservationState::AwaitingAdmission,{},ObservationClosed};
            if(ObservationCertified)return {ObservationClosed?ObservationState::CertifiedRetired:ObservationState::Unresolved,ObservedLease,ObservationClosed};
            return {Holds(ObservedLease)?ObservationState::Held:ObservationState::Unresolved,ObservedLease,ObservationClosed};
        }
        bool AcknowledgeRetirement(const ObservationTicket& ticket,const Token& token)noexcept
        {
            if(!ticket||ticket!=Observer||!ObservationClosed||!ObservationCertified||ObservationUnresolved||!token||token!=ObservedLease)return false;
            ClearObservation();return true;
        }
        // Only a proven synchronous non-admission may cancel. Never clears a
        // bound/uncertain request, including across generations or quarantine.
        bool CancelUnadmittedObservation(const ObservationTicket& ticket)noexcept
        {
            if(!ticket||ticket!=Observer||!ObservationClosed||ObservedLease||ObservationUnresolved)return false;
            ClearObservation();return true;
        }
        Snapshot Observe()const noexcept{return {Current,LastRetired,State,Incarnation,NextRequest,bool(Observer)};}
        size_t ActiveCount() const noexcept { return State==Ownership::Held?1:0; }
    };
}
