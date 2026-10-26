#if defined(MODLOADER_CLIENT_BUILD)

#include "TerrainSource.h"
#include "TerrainProfile.h"
#include "Map/Map.h"

#include "SDK/Chimera_classes.hpp"
#include "SDK/Engine_classes.hpp"
#include "SDK/ChimeraUI_classes.hpp"

#include "Native/NativeApi.h"
#include "plugin_helpers.h"
#include "Native/MapMenuDevSettingsLookup.h"

#include <cmath>

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
                MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
                LOG_ERROR(
                    "MiniMap: terrain source native API is unavailable");
            }

            return nullptr;
        }

        if (g_devSettings == nullptr)
        {
            auto* self = g_pluginSelfAvailable ? GetSelf() : nullptr;
            g_devSettings =
                MiniMapNative::FindMapMenuDevSettingsCDO(self, true);

            if (g_devSettings == nullptr)
            {
                {
                    MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
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
                MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
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
                MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
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
                MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
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
                MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
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


    bool TryResolveSourceTile(
        const SDK::FVector& worldPosition,
        SourceTile& outSourceTile,
        bool verbose)
    {
        MiniMapTerrainProfile::Scope sourceProfile(MiniMapTerrainProfile::Phase::Source);
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
                MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
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
                MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
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
                MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
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
                MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
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
                MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
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
                MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
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
                        MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
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
                MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
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
                MiniMapTerrainProfile::Scope existingLog(MiniMapTerrainProfile::Phase::ExistingLog);
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
