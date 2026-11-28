// Defines the accepted five-floor Forgotten Engine capture metadata outside the generic terrain service.
#include "ForgottenEngineCaptureConfiguration.h"
#if defined(MODLOADER_CLIENT_BUILD)
#include "Services/Map/Terrain/TerrainService.h"
#endif
#include <array>
#include <cstdint>
#include <string>

namespace MiniMapTerrainSource
{
    namespace
    {
        constexpr char kOriginalWorldPackage[] = "/Game/Chimera/Maps/ChimeraMain/ChimeraMain";
        constexpr char kCaptureConfigurationRevision[] = "FE1.NativeCapture.v1";
        constexpr double kImageMinimumX = -2124.5304861843215;
        constexpr double kImageMinimumY = 428405.29135455645;
        constexpr double kImageMaximumX = 22875.46951381568;
        constexpr double kImageMaximumY = 453405.29135455645;
        constexpr std::uint32_t kImageDimensionPixels = 512;
        constexpr std::array<std::uint8_t, 3> kOpaqueBackground{0, 0, 0};
        constexpr Services::Map::Terrain::ITerrainDataProvider::Bounds kStreamingBounds{
            {-3000.0, 424000.0, 2500.0},
            {27000.0, 456000.0, 10500.0}};
        constexpr Services::Map::Terrain::TerrainImage::Registration kImageRegistration{
            {-2124.5304861843215, 428405.29135455645},
            {25000.0, 0.0},
            {0.0, 25000.0}};
        constexpr std::array<const char*, 2> kRequiredCookedCellPackages{
            "/Game/Chimera/Maps/ChimeraMain/ChimeraMain/_Generated_/4I3NLW0EYZ0649KRQPZ6WYBST",
            "/Game/Chimera/Maps/ChimeraMain/ChimeraMain/_Generated_/ETCYICFS8RXUANRCX9KW91CUK"};
    }

    /** Copies shared content and imaging values into one output with an independent Z slab and cache page. */
    ForgottenEngineCaptureConfiguration::CaptureDefinition
        ForgottenEngineCaptureConfiguration::CreateDefinition(
            const char* regionId,
            double minimumZ,
            double maximumZ)
    {
        CaptureDefinition definition;
        definition.Source.SourceId = std::string("StarRupture.ForgottenEngineCapture/") + regionId;
        definition.Source.ContentRevision = kCaptureConfigurationRevision;
        definition.Page = {std::string(regionId) + "_NativeCapture", 0, 0};
        definition.OriginalWorldPackage = kOriginalWorldPackage;
        definition.CookedCellPackages.assign(
            kRequiredCookedCellPackages.begin(), kRequiredCookedCellPackages.end());
        definition.TargetGrids = {"MainGrid"};
        definition.ActivatedDataLayerAssets = {};
        definition.StreamingBounds = kStreamingBounds;
        definition.CaptureBounds = {
            {kImageMinimumX, kImageMinimumY, minimumZ},
            {kImageMaximumX, kImageMaximumY, maximumZ}};
        definition.ImageRegistration = kImageRegistration;
        definition.WidthPixels = kImageDimensionPixels;
        definition.HeightPixels = kImageDimensionPixels;
        definition.SRgb = true;
        definition.OpaqueBackground = kOpaqueBackground;
        return definition;
    }

    /** Returns the five approved slabs in physical elevation order. */
    std::vector<Services::Map::Terrain::ITerrainDataProvider::CaptureDefinition>
        ForgottenEngineCaptureConfiguration::CreateDefinitions()
    {
        return {
            CreateDefinition("FE1_LevelMinus2", 4774.529, 5374.529),
            CreateDefinition("FE1_LevelMinus1", 5374.529, 5974.529),
            CreateDefinition("FE1_Level0", 5974.529, 6574.529),
            CreateDefinition("FE1_LevelPlus1", 6574.529, 7174.529),
            CreateDefinition("FE1_LevelPlus2", 7174.529, 7774.529)};
    }

#if defined(MODLOADER_CLIENT_BUILD)
    /** Submits the one accepted gameplay-generation batch without taking ownership of hooks or the world. */
    bool ForgottenEngineCaptureConfiguration::RequestBatch(
        ::Services::Map::Terrain::TerrainService& terrainService,
        SDK::UWorld* gameplayWorld,
        MiniMap::Services::INativeHooks* hooks)
    {
        const auto definitions = CreateDefinitions();
        if (!hooks || !gameplayWorld || definitions.size() != 5)
        {
            terrainService.RejectPrivateCaptureBatchAdmission(
                definitions, "Capture batch world or native hooks are unavailable.");
            return false;
        }
        return terrainService.RequestPrivateCaptureBatch(definitions, gameplayWorld, *hooks);
    }
#endif
}
