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
    struct DrawFrame
    {
        std::shared_ptr<const DrawState> View;
        uint64_t Generation{};
        size_t Hits{},Fallbacks{},Budget=128;
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
