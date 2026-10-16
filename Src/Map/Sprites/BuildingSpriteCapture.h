#pragma once
#if defined(MINIMAP_STAGE_R2_SMELTER_CAPTURE) && defined(MODLOADER_CLIENT_BUILD)
#include "CaptureDispatchCore.h"
namespace MiniMapStaticAssembly { struct Recipe; }
namespace MiniMapSpriteCapture { struct Candidate;struct Decision; }
namespace MiniMapBuildingCollector { struct Record; }
namespace MiniMapSmelterCapture { bool ResolvePrerequisites(); void Initialize(); void Shutdown(); }
namespace MiniMapSmelterCapture
{
    void OnGameplaySessionReady(void* world, uint64_t generation) noexcept;
    bool HasResourceWatchCapacity(); // GT, bounded ticket-array inspection only
    // GT only: reserve evidence before handoff. Keep Fence on the synchronous
    // dispatch stack and Close it before reconciliation. Retain the CPU Ticket
    // through generation cancellation; acknowledge only after consuming its result.
    MiniMapCaptureLeases::ScopedArmResult ArmRetirementObservation(const MiniMapCaptureDispatch::OwnerObservation&,const MiniMapSpriteDiscoveryCore::Key&) noexcept;
    MiniMapCaptureDispatch::AttemptReconciliation ReconcileRetirementObservation(const MiniMapCaptureLeases::ObservationTicket&) noexcept;
    MiniMapCaptureDispatch::OwnerObservation ObserveNativeOwner() noexcept; // GT, CPU observations only
    MiniMapCaptureDispatch::Reconciliation ReconcileNativeOwner(const MiniMapCaptureLeases::Token&) noexcept;
    // Same candidate audit opportunity, independent of static recipe eligibility.
    // Never reserves/publishes production sprites or changes the audit result.
    void OfferParallelBlueprint(const MiniMapSpriteCapture::Candidate&, uintptr_t visualClass,
        uint32_t buildingId, void* gameplayWorld) noexcept;
    bool CanAcceptCandidate(); // game-thread, no native work; one active owner
    const char* CandidateServiceStatus(); // game-thread diagnostics only
    // Synchronous game-thread handoff; on success acquire independent native
    // strong references before the representation Context releases its roots.
    // Candidate-based front door to the existing owner. Borrowed recipe pointers
    // are synchronously retained before this returns; no second capture backend.
    MiniMapSpriteCapture::Decision OfferStaticAssembly(const MiniMapSpriteCapture::Candidate&,
        const MiniMapBuildingCollector::Record&,const MiniMapStaticAssembly::Recipe&,void* world);
}
#endif
