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
    inline std::atomic<uint64_t> NextOwnerIncarnation{0};
    class Ledger
    {
        const uint64_t Incarnation=NextOwnerIncarnation.fetch_add(1)+1;
        Token Current;
        uint64_t NextRequest{}, NextLease{};
        Ownership State=Ownership::Returned;
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
            State=Ownership::Held;out=Current;return true;
        }
        bool Holds(const Token& token) const noexcept { return bool(token) && State==Ownership::Held && Current==token; }
        bool Return(const Token& token,const RetirementReceipt& receipt) noexcept
        {
            if(!receipt.Certified() || !Holds(token))return false;
            State=Ownership::Returned;return true;
        }
        size_t ActiveCount() const noexcept { return State==Ownership::Held?1:0; }
    };
}
