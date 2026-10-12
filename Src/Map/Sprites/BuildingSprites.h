#pragma once
#if defined(MODLOADER_CLIENT_BUILD) && defined(MINIMAP_BUILDING_SPRITES)
#include "BuildingSpritePipelineCore.h"
#include "Map/Buildings/BuildingCollector.h"
#include <chrono>
namespace MiniMapBuildingSprites
{
    void Reset(uint64_t generation);
    void Record(const MiniMapSpriteDiscoveryCore::Key&,MiniMapBuildingSpriteCore::State,const std::string&);
    bool Queue(const MiniMapSpriteDiscoveryCore::Key&);
    void Complete(const MiniMapSpriteDiscoveryCore::Key&,const MiniMapStaticAssembly::Footprint&,const std::vector<uint8_t>&);
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
    enum class Timing { DiscoveryService,CaptureService,TextureUpload,WidgetCpu,WidgetInterval,RegistryScan,RepresentationAudit,NativeClassResolution,Classification,RepresentationTraversal,AssemblyRecipe,CaptureAdmission,DiagnosticBuild,DiagnosticPublish,Count };
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
