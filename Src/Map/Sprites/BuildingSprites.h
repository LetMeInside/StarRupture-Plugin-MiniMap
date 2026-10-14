#pragma once
#if defined(MODLOADER_CLIENT_BUILD) && defined(MINIMAP_BUILDING_SPRITES)
#include "BuildingSpritePipelineCore.h"
#include "Map/Buildings/BuildingCollector.h"
#include <chrono>
namespace MiniMapRendererReadiness {struct AssemblyObservation;}
namespace MiniMapBuildingSprites
{
    void Reset(uint64_t generation);
    void ServiceMaintenance(); // bounded CPU cleanup, including inactive gameplay
    void Shutdown(); // final CPU teardown; never certifies native retirement
    void Record(const MiniMapSpriteDiscoveryCore::Key&,MiniMapBuildingSpriteCore::State,const std::string&);
    bool AdoptCurrentCandidate(const MiniMapSpriteDiscoveryCore::Key&); // exact current metadata, retry on false
    bool ServiceInitialization(double budgetMs=2.0); // bounded pinned-plan progress; true only for latest membership
    MiniMapBuildingSpriteCore::Admission Queue(const MiniMapSpriteDiscoveryCore::Key&,MiniMapCaptureLeases::Token&);
    bool ReservationCurrent(const MiniMapCaptureLeases::Token&);
    bool WatchCurrent(const MiniMapSpriteDiscoveryCore::Key&,const std::vector<uint64_t>&);
    bool StartCapture(const MiniMapCaptureLeases::Token&);
    bool DependencySnapshot(const MiniMapSpriteDiscoveryCore::Key&,MiniMapRendererReadiness::Dependencies&);
    void WatchState(const MiniMapSpriteDiscoveryCore::Key&,MiniMapRendererReadiness::Dependencies::Watch);
    bool WaitingDemand();
    void FinishReevaluation(const MiniMapSpriteDiscoveryCore::Key&,double sourcePublication);
    void AcknowledgeReevaluation(const MiniMapSpriteDiscoveryCore::Key&,bool);
    void AbandonCapture(const MiniMapCaptureLeases::Token&,bool transient=false) noexcept;
    void RecordForLease(const MiniMapCaptureLeases::Token&,MiniMapBuildingSpriteCore::State,const std::string&);
    bool ReturnLease(const MiniMapCaptureLeases::Token&,const MiniMapCaptureLeases::RetirementReceipt&) noexcept;
    size_t ActiveLeaseCount(); // Independent of renderer generation reset; combine with native CanAcceptCandidate.
    bool AcquireObservationLease(const MiniMapSpriteDiscoveryCore::Key&,uint64_t sourceRevision,MiniMapCaptureLeases::Token&);
    void ObserveResources(const MiniMapSpriteDiscoveryCore::Key&,const MiniMapRendererReadiness::Result&);
    void ObserveAssembly(const MiniMapSpriteDiscoveryCore::Key&,const MiniMapRendererReadiness::AssemblyObservation&);
    // GT notifications carry CPU-owned provider evidence, never render pointers.
    MiniMapRendererReadiness::NotificationAck NotifyProviderChange(const MiniMapSpriteDiscoveryCore::Key&,
        const MiniMapRendererReadiness::ProviderObservation&,const std::vector<uint64_t>& source);
    std::array<uint64_t,5> PublicationWake(const MiniMapSpriteDiscoveryCore::Key&); // CPU-only meaningful change/registry epoch
    bool RequestProviderReevaluation(const MiniMapSpriteDiscoveryCore::Key&,uint32_t provider);
    bool TakeReadinessReevaluation(MiniMapSpriteDiscoveryCore::Key&,bool (*watchCapacity)());
    bool TracksDependencies(const MiniMapSpriteDiscoveryCore::Key&);
    bool ObserveRegistry(std::shared_ptr<const MiniMapSpriteDiscoveryCore::Registry>);
    void ServiceRegistry(); // bounded CPU-only reconciliation, no native observations
    void Complete(const MiniMapCaptureLeases::Token&,const MiniMapStaticAssembly::Footprint&,const std::vector<uint8_t>&,const MiniMapRendererReadiness::Result&);
    struct DrawState;
    struct SpatialSubmission
    {
        int32_t Index{},Serial{};uint32_t Building{};
        PluginTextureHandle Handle{};
        std::array<double,3> Position{};
        std::array<double,4> ScreenBounds{};
        double Yaw{},WorldWidth{},WorldHeight{};
    };
    struct SpatialSnapshot
    {std::array<SpatialSubmission,128> Entries{};size_t Count{};};
    enum class DrawCounter : size_t
    {
        Calls,NoReadyView,GenerationMismatch,InvalidInputs,LookupAttempts,LookupMisses,LookupMatches,
        InvalidTransform,QuadFailure,ProjectionFailure,ViewportRejection,BudgetRejection,FogRejection,
        InvalidHandle,Submissions,Count
    };
    struct DrawFrame
    {
        std::shared_ptr<const DrawState> View;
        uint64_t Generation{};
        size_t Hits{},Fallbacks{},Budget=128;
        std::unique_ptr<SpatialSnapshot> Spatial; // At most eight diagnostic frames per generation.
        std::array<size_t,size_t(DrawCounter::Count)> Counts{};
        uint32_t ReportedReasons{};
        DrawCounter SampleReason=DrawCounter::Count;
        MiniMapSpriteDiscoveryCore::Key SampleKey,SamplePublishedKey;
        bool SampleDefinitionValid{},SampleVisualValid{},SampleTransformValid{},SampleGeometry{},SampleProjection{};
        std::array<double,3> SamplePosition{},SampleScale{};
        std::array<double,4> SampleRotation{},SampleRegion{},SampleViewport{};
        std::array<std::array<double,2>,4> SampleWorld{},SampleScreen{};
    };
    DrawFrame BeginDraw(); // one immutable ready-texture view per map draw
    void EndDraw(const DrawFrame&); // one generation-checked counter commit
    inline void RecordFallback(DrawFrame& frame){++frame.Fallbacks;}
    bool Draw(DrawFrame&,IModLoaderImGui*,PluginDrawList,const MiniMapMap::Transform&,const MiniMapBuildingCollector::Record&);
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
