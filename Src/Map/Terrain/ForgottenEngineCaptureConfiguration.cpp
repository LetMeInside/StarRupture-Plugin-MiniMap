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
        definition.Page = {regionId, 0, 0, regionId};
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

    /** Returns the five independently calibrated capture slabs in physical elevation order. */
    std::vector<Services::Map::Terrain::ITerrainDataProvider::CaptureDefinition>
        ForgottenEngineCaptureConfiguration::CreateDefinitions()
    {
        auto level4 = CreateDefinition("ChimeraMain_ForgottenEngine1_Level4", 5468.0, 5820.0);
        level4.ExcludedStaticMeshAssets = {
            "/Game/Chimera/Buildings/ForgottenEngine/Interior/SM_FE_ceiling.SM_FE_ceiling"};
        return {
            CreateDefinition("ChimeraMain_ForgottenEngine1_Level5", 4985.0, 5445.0),
            level4,
            CreateDefinition("ChimeraMain_ForgottenEngine1_Level3", 6185.0, 6640.0),
            CreateDefinition("ChimeraMain_ForgottenEngine1_Level2", 6810.0, 7110.0),
            CreateDefinition("ChimeraMain_ForgottenEngine1_Level1", 7468.0, 7740.0)};
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
