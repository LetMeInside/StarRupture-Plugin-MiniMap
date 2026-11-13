// Reads authored terrain metadata and resolves map-space tiles without scanning during ticks.
#if defined(MODLOADER_CLIENT_BUILD)

#include "TerrainSource.h"
#include "TerrainProfile.h"
#include "Map/Map.h"

#include "SDK/Chimera_classes.hpp"
#include "SDK/Engine_classes.hpp"
#include "SDK/ChimeraUI_classes.hpp"
#include "SDK/ChimeraUI_structs.hpp"
#include "SDK/CoreUObject_structs.hpp"
#include "SDK/SlateCore_structs.hpp"

#include "Native/NativeApi.h"
#include "plugin_helpers.h"
#include "Native/MapMenuDevSettingsLookup.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

static_assert(sizeof(void*) == 0x8);
static_assert(sizeof(int32_t) == 0x4);
static_assert(sizeof(SDK::TSoftObjectPtr<SDK::UCrMapMenuTerrainData>) == 0x28);
static_assert(alignof(SDK::TSoftObjectPtr<SDK::UCrMapMenuTerrainData>) == alignof(void*));
static_assert(sizeof(decltype(SDK::TPersistentObjectPtr<
    SDK::FakeSoftObjectPtr::FSoftObjectPath>::WeakPtr)) == 0x8);
static_assert(sizeof(decltype(SDK::TPersistentObjectPtr<
    SDK::FakeSoftObjectPtr::FSoftObjectPath>::ObjectID)) == 0x20);
static_assert(offsetof(SDK::TPersistentObjectPtr<
    SDK::FakeSoftObjectPtr::FSoftObjectPath>, WeakPtr) == 0x0);
static_assert(offsetof(SDK::TPersistentObjectPtr<
    SDK::FakeSoftObjectPtr::FSoftObjectPath>, ObjectID) == 0x8);
static_assert(offsetof(SDK::UCrMapMenuDevSettings, TerrainData) == 0x3C8);
static_assert(sizeof(decltype(SDK::UCrMapMenuDevSettings::TerrainData)) == 0x28);
static_assert(sizeof(decltype(SDK::UCrMapMenuTerrainData::MapTerrainTopLeftPivotPoint)) == 0xC);
static_assert(sizeof(decltype(SDK::UCrMapMenuTerrainData::MapTerrainSegmentSize)) == 0xC);
static_assert(offsetof(SDK::UCrMapMenuTerrainData, MapTerrainTopLeftPivotPoint) == 0x30);
static_assert(offsetof(SDK::UCrMapMenuTerrainData, MapTerrainSegmentSize) == 0x3C);
static_assert(offsetof(SDK::UCrMapMenuTerrainData, TerrainSegmentsData) == 0x70);
static_assert(sizeof(SDK::FVector3f) == 0xC);
static_assert(alignof(SDK::FVector3f) == alignof(float));
static_assert(offsetof(SDK::FVector3f, X) == 0x0);
static_assert(offsetof(SDK::FVector3f, Y) == 0x4);
static_assert(offsetof(SDK::FVector3f, Z) == 0x8);
static_assert(sizeof(decltype(SDK::FVector3f::X)) == 0x4);
static_assert(std::numeric_limits<float>::is_iec559);
static_assert(sizeof(decltype(SDK::UCrMapMenuTerrainData::TerrainSegmentsData)) == 0x10);
static_assert(alignof(decltype(SDK::UCrMapMenuTerrainData::TerrainSegmentsData)) == alignof(void*));
static_assert(sizeof(SDK::FCrTerrainSegmentData) == 0x170);
static_assert(alignof(SDK::FCrTerrainSegmentData) == alignof(SDK::FSlateBrush));
static_assert(offsetof(SDK::FCrTerrainSegmentData, TerrainSegmentGridIndex) == 0x0);
static_assert(sizeof(decltype(SDK::FCrTerrainSegmentData::TerrainSegmentGridIndex)) == 0x8);
static_assert(offsetof(SDK::FCrTerrainSegmentData, TerrainSegmentTexture) == 0x10);
static_assert(sizeof(decltype(SDK::FCrTerrainSegmentData::TerrainSegmentTexture)) == 0xB0);
static_assert(offsetof(SDK::FCrTerrainSegmentData, TerrainSegmentTextureRadiation2) == 0xC0);
static_assert(sizeof(decltype(SDK::FCrTerrainSegmentData::TerrainSegmentTextureRadiation2)) == 0xB0);
static_assert(sizeof(SDK::FIntPoint) == 0x8);
static_assert(alignof(SDK::FIntPoint) == alignof(int32_t));
static_assert(offsetof(SDK::FIntPoint, X) == 0x0);
static_assert(offsetof(SDK::FIntPoint, Y) == 0x4);
static_assert(sizeof(decltype(SDK::FIntPoint::X)) == sizeof(int32_t));
static_assert(sizeof(SDK::FSlateBrush) == 0xB0);
static_assert(alignof(SDK::FSlateBrush) == 0x10);
static_assert(std::is_same_v<
    decltype(SDK::ACrMapMenuDataReplicationHelper::CurrentRadiationLevelReplicated), int32_t>);
static_assert(offsetof(SDK::ACrMapMenuDataReplicationHelper, CurrentRadiationLevelReplicated) == 0x468);
static_assert(sizeof(decltype(SDK::ACrMapMenuDataReplicationHelper::CurrentRadiationLevelReplicated)) == 0x4);
static_assert(offsetof(SDK::UWorld, GameState) == 0x1B0);
static_assert(offsetof(SDK::ACrGameStateBase, MapMenuDataReplicationHelper) == 0x368);

namespace
{
    bool g_pluginSelfAvailable = false;
    SDK::UCrMapMenuDevSettings* g_devSettings = nullptr;


    SDK::UCrMapMenuTerrainData* LoadTerrainData()
    {
        const MiniMapNative::NativeApi* native =
            MiniMapNative::Get();

        if (native == nullptr ||
            native->asset.loadSynchronous == nullptr)
        {
            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
#endif
                LOG_ERROR(
                    "MiniMap: terrain source native API is unavailable");
            }

            return nullptr;
        }

        if (g_devSettings == nullptr)
        {
            auto* self = g_pluginSelfAvailable ? GetSelf() : nullptr;
            g_devSettings =
                self ? MiniMapNative::FindMapMenuDevSettingsCDO(true) : nullptr;

            if (g_devSettings == nullptr)
            {
                {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                    MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
#endif
                    LOG_ERROR(
                        "MiniMap: CrMapMenuDevSettings CDO was not found");
                }

                return nullptr;
            }
        }

        void* terrainDataSoftPtr =
            static_cast<void*>(
                &g_devSettings->TerrainData);

        SDK::UObject* loadedObject =
            native->asset.loadSynchronous(
                terrainDataSoftPtr);

        if (loadedObject == nullptr)
        {
            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
#endif
                LOG_ERROR(
                    "MiniMap: TerrainData could not be loaded");
            }

            return nullptr;
        }

        return static_cast<SDK::UCrMapMenuTerrainData*>(
            loadedObject);
    }


    bool TryGetCurrentRadiationLevel(
        int& outRadiationLevel)
    {
        outRadiationLevel = 0;

        SDK::UWorld* world =
            MiniMapMap::GetWorld();

        if (world == nullptr)
        {
            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
#endif
                LOG_ERROR(
                    "MiniMap: terrain source cannot read radiation level "
                    "because the active world is unavailable");
            }

            return false;
        }

        SDK::AGameStateBase* baseGameState =
            world->GameState;

        if (baseGameState == nullptr)
        {
            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
#endif
                LOG_ERROR(
                    "MiniMap: terrain source cannot read radiation level "
                    "because GameState is unavailable");
            }

            return false;
        }

        auto* gameState =
            static_cast<SDK::ACrGameStateBase*>(
                baseGameState);

        SDK::ACrMapMenuDataReplicationHelper* helper =
            gameState->MapMenuDataReplicationHelper;

        if (helper == nullptr)
        {
            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
#endif
                LOG_ERROR(
                    "MiniMap: terrain source cannot read radiation level "
                    "because MapMenuDataReplicationHelper is unavailable");
            }

            return false;
        }

        outRadiationLevel =
            helper->CurrentRadiationLevelReplicated;

        return true;
    }


    const char* GetVariantName(
        MiniMapTerrainSource::Variant variant)
    {
        switch (variant)
        {
        case MiniMapTerrainSource::Variant::Radiation2:
            return "Radiation2";

        case MiniMapTerrainSource::Variant::Ordinary:
        default:
            return "Ordinary";
        }
    }
}


namespace MiniMapTerrainSource
{
    bool Initialize(
        IPluginSelf* self)
    {
        g_pluginSelfAvailable = self != nullptr;
        g_devSettings = nullptr;

        if (self == nullptr ||
            self->hooks == nullptr)
        {
            LOG_ERROR(
                "MiniMap: hooks are unavailable "
                "during terrain source initialization");

            return false;
        }

        return true;
    }


    void Shutdown()
    {
        g_devSettings = nullptr;
        g_pluginSelfAvailable = false;
    }


    bool TryGetWorldOrigin(double& outX, double& outY)
    {
        outX = outY = 0.0;
        SDK::UCrMapMenuTerrainData* terrainData = LoadTerrainData();
        if (!terrainData) return false;
        const auto& pivot = terrainData->MapTerrainTopLeftPivotPoint;
        if (!std::isfinite(pivot.X) || !std::isfinite(pivot.Y)) return false;
        outX = static_cast<double>(pivot.X);
        outY = static_cast<double>(pivot.Y);
        return true;
    }


    // Calculates the actual terrain-record bounding extent in native map units.
    // Outputs remain zero when terrain metadata, segment size or coverage is invalid.
    bool TryGetWorldDimensions(
        double& outWidthMapUnits,
        double& outHeightMapUnits)
    {
        outWidthMapUnits = 0.0;
        outHeightMapUnits = 0.0;
        SDK::UCrMapMenuTerrainData* terrainData = LoadTerrainData();
        if (terrainData == nullptr) return false;
        const double worldTileWidth = 100.0 * static_cast<double>(terrainData->MapTerrainSegmentSize.X),
            worldTileHeight = 100.0 * static_cast<double>(terrainData->MapTerrainSegmentSize.Y);
        const int segmentCount = terrainData->TerrainSegmentsData.Num();
        if (!std::isfinite(worldTileWidth) || !std::isfinite(worldTileHeight) || worldTileWidth <= 0.0 ||
            worldTileHeight <= 0.0 || segmentCount <= 0) return false;
        const auto& firstSegment = terrainData->TerrainSegmentsData[0];
        std::int64_t minimumIndexX = firstSegment.TerrainSegmentGridIndex.X,
            maximumIndexX = minimumIndexX,
            minimumIndexY = firstSegment.TerrainSegmentGridIndex.Y,
            maximumIndexY = minimumIndexY;
        for (int segmentIndex = 1; segmentIndex < segmentCount; ++segmentIndex)
        {
            const auto& segment = terrainData->TerrainSegmentsData[segmentIndex];
            const std::int64_t indexX = segment.TerrainSegmentGridIndex.X,
                indexY = segment.TerrainSegmentGridIndex.Y;
            minimumIndexX = (std::min)(minimumIndexX, indexX);
            maximumIndexX = (std::max)(maximumIndexX, indexX);
            minimumIndexY = (std::min)(minimumIndexY, indexY);
            maximumIndexY = (std::max)(maximumIndexY, indexY);
        }
        const long double tileCountX = static_cast<long double>(maximumIndexY) -
            static_cast<long double>(minimumIndexY) + 1.0L,
            tileCountY = static_cast<long double>(maximumIndexX) -
                static_cast<long double>(minimumIndexX) + 1.0L;
        const long double widthMapUnits = tileCountX * static_cast<long double>(worldTileWidth),
            heightMapUnits = tileCountY * static_cast<long double>(worldTileHeight);
        if (!std::isfinite(widthMapUnits) || !std::isfinite(heightMapUnits) ||
            widthMapUnits <= 0.0L || heightMapUnits <= 0.0L ||
            widthMapUnits > static_cast<long double>((std::numeric_limits<double>::max)()) ||
            heightMapUnits > static_cast<long double>((std::numeric_limits<double>::max)()))
        {
            return false;
        }
        outWidthMapUnits = static_cast<double>(widthMapUnits);
        outHeightMapUnits = static_cast<double>(heightMapUnits);
        if (!std::isfinite(outWidthMapUnits) || !std::isfinite(outHeightMapUnits) ||
            outWidthMapUnits <= 0.0 || outHeightMapUnits <= 0.0)
        {
            outWidthMapUnits = outHeightMapUnits = 0.0;
            return false;
        }
        return true;
    }


    bool TryResolveSourceTile(
        const SDK::FVector& worldPosition,
        SourceTile& outSourceTile,
        bool verbose)
    {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        MiniMapTerrainProfile::Scope sourceProfile(MiniMapTerrainProfile::Phase::Source);
#endif
        outSourceTile = {};

        SDK::UCrMapMenuTerrainData* terrainData =
            LoadTerrainData();

        if (terrainData == nullptr)
        {
            return false;
        }

        const MiniMapNative::NativeApi* native =
            MiniMapNative::Get();

        if (native == nullptr ||
            native->texture.getBrushTexture == nullptr)
        {
            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
#endif
                LOG_ERROR(
                    "MiniMap: terrain source texture API is unavailable");
            }

            return false;
        }

        int radiationLevel = 0;

        if (!TryGetCurrentRadiationLevel(
            radiationLevel))
        {
            return false;
        }

        if (verbose)
        {
            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
#endif
                LOG_INFO(
                    "MiniMap: F8 diagnostic: "
                    "current radiation level=%d",
                    radiationLevel);
            }
        }

        const auto& pivot =
            terrainData->MapTerrainTopLeftPivotPoint;

        const auto& segmentSize =
            terrainData->MapTerrainSegmentSize;

        const int segmentCount =
            terrainData->TerrainSegmentsData.Num();

        if (verbose)
        {
            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
#endif
                LOG_INFO(
                    "MiniMap: F8 diagnostic: "
                    "TerrainData origin=(%.3f, %.3f, %.3f) "
                    "segment-size=(%.3f, %.3f, %.3f) "
                    "segments=%d",
                    pivot.X,
                    pivot.Y,
                    pivot.Z,
                    segmentSize.X,
                    segmentSize.Y,
                    segmentSize.Z,
                    segmentCount);
            }
        }

        const double worldTileWidth =
            100.0 *
            static_cast<double>(
                segmentSize.X);

        const double worldTileHeight =
            100.0 *
            static_cast<double>(
                segmentSize.Y);

        if (worldTileWidth <= 0.0 ||
            worldTileHeight <= 0.0)
        {
            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
#endif
                LOG_ERROR(
                    "MiniMap: F8 diagnostic: "
                    "invalid terrain world tile size "
                    "(%.3f x %.3f)",
                    worldTileWidth,
                    worldTileHeight);
            }

            return false;
        }

        const double horizontalTile =
            (worldPosition.X -
                static_cast<double>(
                    pivot.X)) /
            worldTileWidth;

        const double verticalTile =
            (worldPosition.Y -
                static_cast<double>(
                    pivot.Y)) /
            worldTileHeight;

        const int gridX =
            static_cast<int>(
                std::floor(
                    verticalTile));

        const int gridY =
            static_cast<int>(
                std::floor(
                    horizontalTile));

        const double localU =
            horizontalTile -
            static_cast<double>(
                gridY);

        const double localV =
            verticalTile -
            static_cast<double>(
                gridX);

        if (verbose)
        {
            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
#endif
                LOG_INFO(
                    "MiniMap: F8 diagnostic: "
                    "player terrain grid=(%d, %d) "
                    "local-uv=(%.6f, %.6f)",
                    gridX,
                    gridY,
                    localU,
                    localV);
            }
        }

        const SDK::FCrTerrainSegmentData* sourceSegment =
            nullptr;

        for (int i = 0; i < segmentCount; ++i)
        {
            const auto& segment =
                terrainData->
                TerrainSegmentsData[i];

            if (segment.TerrainSegmentGridIndex.X ==
                gridX &&
                segment.TerrainSegmentGridIndex.Y ==
                gridY)
            {
                sourceSegment =
                    &segment;

                break;
            }
        }

        if (sourceSegment == nullptr)
        {
            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
#endif
                LOG_ERROR(
                    "MiniMap: F8 diagnostic: "
                    "player position maps outside available terrain "
                    "or grid=(%d, %d) has no terrain record",
                    gridX,
                    gridY);
            }

            return false;
        }

        Variant selectedVariant =
            Variant::Ordinary;

        SDK::UTexture2D* sourceTexture =
            nullptr;

        if (radiationLevel == 1)
        {
            sourceTexture =
                native->texture.getBrushTexture(
                    sourceSegment->
                    TerrainSegmentTextureRadiation2);

            if (sourceTexture != nullptr)
            {
                selectedVariant =
                    Variant::Radiation2;
            }
            else
            {
                if (verbose)
                {
                    {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                        MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
#endif
                        LOG_INFO(
                            "MiniMap: F8 diagnostic: "
                            "terrain grid=(%d, %d) has no usable "
                            "Radiation2 texture; falling back to ordinary",
                            gridX,
                            gridY);
                    }
                }
            }
        }

        if (sourceTexture == nullptr)
        {
            sourceTexture =
                native->texture.getBrushTexture(
                    sourceSegment->
                    TerrainSegmentTexture);

            selectedVariant =
                Variant::Ordinary;
        }

        if (verbose)
        {
            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
#endif
                LOG_INFO(
                    "MiniMap: F8 diagnostic: "
                    "selected terrain segment grid=(%d, %d) "
                    "radiation-level=%d variant=%s texture=%p",
                    gridX,
                    gridY,
                    radiationLevel,
                    GetVariantName(
                        selectedVariant),
                    sourceTexture);
            }
        }

        if (sourceTexture == nullptr)
        {
            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
#endif
                LOG_ERROR(
                    "MiniMap: F8 diagnostic: "
                    "selected terrain brush did not provide "
                    "a UTexture2D");
            }

            return false;
        }

        outSourceTile.GridX =
            gridX;

        outSourceTile.GridY =
            gridY;

        outSourceTile.LocalU =
            localU;

        outSourceTile.LocalV =
            localV;

        outSourceTile.RadiationLevel =
            radiationLevel;

        outSourceTile.SelectedVariant =
            selectedVariant;

        outSourceTile.Texture =
            sourceTexture;

        return true;
    }
}

#endif
