#pragma once
#if defined(MODLOADER_CLIENT_BUILD) && defined(MINIMAP_BUILDING_SPRITES)
#include "BuildingSpritePipelineCore.h"
#include "CaptureDispatchCore.h"
#include "Map/Buildings/BuildingCollector.h"
#include <chrono>
namespace MiniMapBuildingSprites
{
    void Reset(uint64_t generation);
    void ServiceMaintenance(); // bounded CPU cleanup, including inactive gameplay
    void Shutdown(); // final CPU teardown; never certifies native retirement
    void Record(const MiniMapSpriteDiscoveryCore::Key&,MiniMapBuildingSpriteCore::State,const std::string&);
    bool ServiceInitialization(double budgetMs=2.0); // bounded pinned-plan progress; true only for latest membership
    MiniMapBuildingSpriteCore::Admission Queue(const MiniMapSpriteDiscoveryCore::Key&,MiniMapCaptureLeases::Token&);
    bool ReservationCurrent(const MiniMapCaptureLeases::Token&);
    bool StartCapture(const MiniMapCaptureLeases::Token&);
    void AbandonCapture(const MiniMapCaptureLeases::Token&,bool transient=false) noexcept;
    void RecordForLease(const MiniMapCaptureLeases::Token&,MiniMapBuildingSpriteCore::State,const std::string&);
    bool ReturnLease(const MiniMapCaptureLeases::Token&,const MiniMapCaptureLeases::RetirementReceipt&) noexcept;
    MiniMapCaptureDispatch::RendererEvidence InspectDispatch(const MiniMapSpriteDiscoveryCore::Key&,
        const std::shared_ptr<const MiniMapSpriteDiscoveryCore::Registry>&) noexcept; // read-only, no allocation
    MiniMapCaptureLeases::Inspection InspectLease(const MiniMapCaptureLeases::Token&) noexcept;
    MiniMapCaptureLeases::Snapshot ObserveLeases() noexcept;
    MiniMapCaptureLeases::ArmResult ArmRetirementObservation(const MiniMapCaptureLeases::Snapshot&,const MiniMapSpriteDiscoveryCore::Key&) noexcept;
    void SealRetirementObservation(const MiniMapCaptureLeases::ObservationTicket&) noexcept;
    MiniMapCaptureLeases::RetirementObservation ObserveRetirement(const MiniMapCaptureLeases::ObservationTicket&) noexcept;
    bool AcknowledgeRetirement(const MiniMapCaptureLeases::ObservationTicket&,const MiniMapCaptureLeases::Token&) noexcept;
    bool CancelUnadmittedObservation(const MiniMapCaptureLeases::ObservationTicket&) noexcept;
    size_t ActiveLeaseCount(); // Independent of renderer generation reset; combine with native CanAcceptCandidate.
    bool AcquireObservationLease(const MiniMapSpriteDiscoveryCore::Key&,uint64_t sourceRevision,MiniMapCaptureLeases::Token&);
    bool ObserveRegistry(std::shared_ptr<const MiniMapSpriteDiscoveryCore::Registry>);
    void Complete(const MiniMapCaptureLeases::Token&,const MiniMapStaticAssembly::Footprint&,const std::vector<uint8_t>&);
    enum class Timing { DiscoveryService,CaptureService,TextureUpload,WidgetCpu,WidgetInterval,RegistryScan,RepresentationAudit,NativeClassResolution,Classification,RepresentationTraversal,AssemblyRecipe,CaptureAdmission,DiagnosticBuild,DiagnosticPublish,ReadinessService,ReadinessSample,Count };
    void RecordTiming(Timing,double milliseconds) noexcept;
    struct TimingScope
    {
        Timing Kind;std::chrono::steady_clock::time_point Start=std::chrono::steady_clock::now();
        explicit TimingScope(Timing kind):Kind(kind){}
        ~TimingScope(){RecordTiming(Kind,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-Start).count());}
    };
    std::string Diagnostics();
}
#endif
