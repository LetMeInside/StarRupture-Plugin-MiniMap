#pragma once
#include <cstdint>
#include <cstddef>
#include "CaptureDispatchCore.h"
namespace MiniMapSpriteCapture
{
    struct Candidate;
    struct Decision;
    // Member of the existing production capture worker. Native ownership never
    // escapes this lifecycle. No native releases from static destruction.
    class PrivateCaptureWorld final
    {
    public:
        PrivateCaptureWorld() = default;
        PrivateCaptureWorld(const PrivateCaptureWorld&) = delete;
        PrivateCaptureWorld& operator=(const PrivateCaptureWorld&) = delete;
        void OnSessionReady(void* gameplayWorld, uint64_t generation) noexcept;
        void Service() noexcept; // Existing guarded game-thread worker tick only.
        Decision OfferCandidate(const Candidate&, uintptr_t visualClass, uint32_t buildingId, void* gameplayWorld) noexcept;
        bool CanAcceptCandidate() const noexcept;
        MiniMapCaptureLeases::Token OwnerLease() const noexcept;
        MiniMapCaptureDispatch::Reconciliation Reconcile(const MiniMapCaptureLeases::Token&) const noexcept;
        const char* Status() const noexcept;
        bool ValidateReady(void* gameplayWorld, uint64_t generation) noexcept;
        void EndSession(void* gameplayWorld) noexcept; // Before native EndPlay.
    private:
        struct Implementation;
        // Deliberately process-lived: uncertain native references stay quarantined.
        Implementation* impl = nullptr;
    };
}
