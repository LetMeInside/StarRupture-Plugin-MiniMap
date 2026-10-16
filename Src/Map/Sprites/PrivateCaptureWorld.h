#pragma once
#include <cstdint>
#include <cstddef>
namespace MiniMapSpriteCapture
{
    struct Candidate;
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
        // Same worker candidate stream; independent private ownership, never a publication lease.
        void OfferCandidate(const Candidate&, uintptr_t visualClass, uint32_t buildingId, void* gameplayWorld) noexcept;
        void ObserveProduction(const Candidate&, const unsigned char* rgba, size_t bytes) noexcept;
        bool ValidateReady(void* gameplayWorld, uint64_t generation) noexcept;
        void EndSession(void* gameplayWorld) noexcept; // Before native EndPlay.
    private:
        struct Implementation;
        // Deliberately process-lived: uncertain native references stay quarantined.
        Implementation* impl = nullptr;
    };
}
