#if defined(MODLOADER_CLIENT_BUILD)

#include "TerrainSource.h"
#include "Map.h"

#include "SDK/Chimera_classes.hpp"
#include "SDK/Engine_classes.hpp"
#include "SDK/ChimeraUI_classes.hpp"

#include "../Native/NativeApi.h"
#include "../plugin_helpers.h"

#include <cmath>

namespace
{
    IPluginSelf* g_sourceSelf = nullptr;
    SDK::UCrMapMenuTerrainData* g_terrainData = nullptr;


    SDK::UCrMapMenuDevSettings* FindMapMenuDevSettingsCDO()
    {
        if (g_sourceSelf == nullptr ||
            g_sourceSelf->hooks == nullptr ||
            g_sourceSelf->hooks->ObjectWalker == nullptr)
        {
            LOG_ERROR(
                "MiniMap: ObjectWalker is unavailable");

            return nullptr;
        }

        auto* walker =
            g_sourceSelf->hooks->ObjectWalker;

        if (!walker->IsReady())
        {
            LOG_ERROR(
                "MiniMap: ObjectWalker is not ready");

            return nullptr;
        }

        PluginObjectInfo objects[8] = {};

        const int count =
            walker->FindObjectsByClassNameInto(
                "CrMapMenuDevSettings",
                PluginObjectLookup_CDOOnly,
                objects,
                8);

        LOG_INFO(
            "MiniMap: found %d CrMapMenuDevSettings CDO(s)",
            count);

        const int loggedCount =
            count < 8 ? count : 8;

        for (int i = 0; i < loggedCount; ++i)
        {
            LOG_INFO(
                "MiniMap: DevSettings CDO [%d]: "
                "object=%p name=%s class=%s",
                i,
                objects[i].object,
                objects[i].objectName,
                objects[i].className);
        }

        if (count != 1 ||
            objects[0].object == nullptr)
        {
            LOG_WARN(
                "MiniMap: expected exactly one "
                "CrMapMenuDevSettings CDO");

            return nullptr;
        }

        return static_cast<SDK::UCrMapMenuDevSettings*>(
            objects[0].object);
    }


    bool EnsureTerrainDataLoaded()
    {
        if (g_terrainData != nullptr)
        {
            return true;
        }

        const MiniMapNative::NativeApi* native =
            MiniMapNative::Get();

        if (native == nullptr ||
            native->asset.loadSynchronous == nullptr)
        {
            LOG_ERROR(
                "MiniMap: terrain source native API is unavailable");

            return false;
        }

        SDK::UCrMapMenuDevSettings* devSettings =
            FindMapMenuDevSettingsCDO();

        if (devSettings == nullptr)
        {
            LOG_ERROR(
                "MiniMap: CrMapMenuDevSettings CDO was not found");

            return false;
        }

        void* terrainDataSoftPtr =
            static_cast<void*>(
                &devSettings->TerrainData);

        SDK::UObject* loadedObject =
            native->asset.loadSynchronous(
                terrainDataSoftPtr);

        if (loadedObject == nullptr)
        {
            LOG_ERROR(
                "MiniMap: TerrainData could not be loaded");

            return false;
        }

        g_terrainData =
            static_cast<SDK::UCrMapMenuTerrainData*>(
                loadedObject);

        return true;
    }


    bool TryGetCurrentRadiationLevel(
        int& outRadiationLevel)
    {
        outRadiationLevel = 0;

        SDK::UWorld* world =
            MiniMapMap::GetWorld();

        if (world == nullptr)
        {
            LOG_ERROR(
                "MiniMap: terrain source cannot read radiation level "
                "because the active world is unavailable");

            return false;
        }

        SDK::AGameStateBase* baseGameState =
            world->GameState;

        if (baseGameState == nullptr)
        {
            LOG_ERROR(
                "MiniMap: terrain source cannot read radiation level "
                "because GameState is unavailable");

            return false;
        }

        auto* gameState =
            static_cast<SDK::ACrGameStateBase*>(
                baseGameState);

        SDK::ACrMapMenuDataReplicationHelper* helper =
            gameState->MapMenuDataReplicationHelper;

        if (helper == nullptr)
        {
            LOG_ERROR(
                "MiniMap: terrain source cannot read radiation level "
                "because MapMenuDataReplicationHelper is unavailable");

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
        g_sourceSelf = self;
        g_terrainData = nullptr;

        if (g_sourceSelf == nullptr ||
            g_sourceSelf->hooks == nullptr)
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
        g_terrainData = nullptr;
        g_sourceSelf = nullptr;
    }


    bool TryResolveSourceTile(
        const SDK::FVector& worldPosition,
        SourceTile& outSourceTile)
    {
        outSourceTile = {};

        if (!EnsureTerrainDataLoaded())
        {
            return false;
        }

        const MiniMapNative::NativeApi* native =
            MiniMapNative::Get();

        if (native == nullptr ||
            native->texture.getBrushTexture == nullptr)
        {
            LOG_ERROR(
                "MiniMap: terrain source texture API is unavailable");

            return false;
        }

        int radiationLevel = 0;

        if (!TryGetCurrentRadiationLevel(
            radiationLevel))
        {
            return false;
        }

        LOG_INFO(
            "MiniMap: F8 diagnostic: "
            "current radiation level=%d",
            radiationLevel);

        const auto& pivot =
            g_terrainData->MapTerrainTopLeftPivotPoint;

        const auto& segmentSize =
            g_terrainData->MapTerrainSegmentSize;

        const int segmentCount =
            g_terrainData->TerrainSegmentsData.Num();

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
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "invalid terrain world tile size "
                "(%.3f x %.3f)",
                worldTileWidth,
                worldTileHeight);

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

        LOG_INFO(
            "MiniMap: F8 diagnostic: "
            "player terrain grid=(%d, %d) "
            "local-uv=(%.6f, %.6f)",
            gridX,
            gridY,
            localU,
            localV);

        const SDK::FCrTerrainSegmentData* sourceSegment =
            nullptr;

        for (int i = 0; i < segmentCount; ++i)
        {
            const auto& segment =
                g_terrainData->
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
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "player position maps outside available terrain "
                "or grid=(%d, %d) has no terrain record",
                gridX,
                gridY);

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
                LOG_INFO(
                    "MiniMap: F8 diagnostic: "
                    "terrain grid=(%d, %d) has no usable "
                    "Radiation2 texture; falling back to ordinary",
                    gridX,
                    gridY);
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

        if (sourceTexture == nullptr)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "selected terrain brush did not provide "
                "a UTexture2D");

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
