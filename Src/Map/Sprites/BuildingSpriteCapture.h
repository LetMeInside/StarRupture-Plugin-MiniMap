#pragma once
#if defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE) && defined(MODLOADER_CLIENT_BUILD)
#include "CaptureDispatchCore.h"
namespace MiniMapSpriteCapture { struct Candidate;struct Decision; }
namespace MiniMapBuildingCollector { struct Record; }
namespace MiniMapBuildingCapture { bool ResolvePrerequisites(); void Initialize(); void Shutdown(); }
namespace MiniMapBuildingCapture
{
    void OnGameplaySessionReady(void* world, uint64_t generation) noexcept;
    // GT only: reserve evidence before handoff. Keep Fence on the synchronous
    // dispatch stack and Close it before reconciliation. Retain the CPU Ticket
    // through generation cancellation; acknowledge only after consuming its result.
    MiniMapCaptureLeases::ScopedArmResult ArmRetirementObservation(const MiniMapCaptureDispatch::OwnerObservation&,const MiniMapSpriteDiscoveryCore::Key&) noexcept;
    MiniMapCaptureDispatch::AttemptReconciliation ReconcileRetirementObservation(const MiniMapCaptureLeases::ObservationTicket&) noexcept;
    MiniMapCaptureDispatch::OwnerObservation ObserveNativeOwner() noexcept; // GT, CPU observations only
    MiniMapCaptureDispatch::Reconciliation ReconcileNativeOwner(const MiniMapCaptureLeases::Token&) noexcept;
    MiniMapSpriteCapture::Decision OfferBlueprint(const MiniMapSpriteCapture::Candidate&,
        uintptr_t visualClass,uint32_t buildingId,void* gameplayWorld) noexcept;
    bool CanAcceptCandidate(); // game-thread, no native work; one active owner
    const char* CandidateServiceStatus(); // game-thread diagnostics only
}
#endif
