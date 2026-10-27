#if defined(MODLOADER_CLIENT_BUILD)

#include "PointsOfInterest.h"
#include "DiagnosticExports.h"
#include "UI/MiniMapUI.h"
#include "Config/Config.h"

#include "Map/FogOfWar/FogOfWar.h"
#include "Map/Map.h"
#include "Map/MapTransform.h"
#include "Map/MarkerIconSizing.h"

#include "Native/NativeApi.h"
#include "Native/TextureAccess.h"
#include "Native/BC3Decode.h"
#include "plugin_helpers.h"
#include "Native/MapMenuDevSettingsLookup.h"

#include "SDK/Chimera_classes.hpp"
#include "SDK/ChimeraUI_classes.hpp"
#include "SDK/Engine_classes.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    constexpr float kSnapshotIntervalSeconds =
        0.5f;

    constexpr uint8_t kStateHidden =
        0;

    constexpr uint8_t kStateVisibleUnknown =
        1;

    constexpr uint8_t kStateVisibleDiscovered =
        2;

    constexpr uint8_t kStateDisabled =
        3;

    constexpr uint8_t kUnknownFilter =
        24;

    constexpr uint8_t kAbandonedBaseType =
        2;

    constexpr int32_t kMaximumReasonableAbandonBaseRecords =
        4096;


    struct NativeActorClassStorage
    {
        SDK::UClass* Class =
            nullptr;
    };

    static_assert(
        sizeof(NativeActorClassStorage) ==
        8);


    struct NativeActorArray
    {
        SDK::AActor** Data =
            nullptr;

        int32_t Num =
            0;

        int32_t Max =
            0;
    };

    static_assert(
        sizeof(NativeActorArray) ==
        0x10);


    struct POIGuid
    {
        uint32_t A = 0;
        uint32_t B = 0;
        uint32_t C = 0;
        uint32_t D = 0;

        bool operator==(
            const POIGuid& other) const noexcept
        {
            return
                A == other.A &&
                B == other.B &&
                C == other.C &&
                D == other.D;
        }
    };


    struct POIRecord
    {
        POIGuid Guid = {};
        double WorldX = 0.0;
        double WorldY = 0.0;
        PluginTextureHandle Icon = nullptr;
        bool CanHideByFog = false;
    };


    enum class IconVariant : uint8_t
    {
        Unknown = 1,
        Discovered = 2,
        Completed = 3
    };


    bool g_initialized =
        false;

    bool g_pluginSelfAvailable =
        false;

    bool g_tickRegistered =
        false;

    bool g_experienceReady =
        false;

    float g_snapshotAccumulator =
        0.0f;

    SDK::UCrMapMenuDevSettings* g_devSettings =
        nullptr;

    std::mutex g_snapshotMutex;

    std::vector<POIRecord> g_records;

    /*
     * POI icon handles are intentionally retained for plugin/process lifetime.
     *
     * AlienX currently has no fence-aware texture retirement. The landmark
     * icon set is tiny, immutable after upload, and safe to retain.
     */
    std::unordered_map<uint32_t, PluginTextureHandle>
        g_iconCache;


    SDK::UCrPlayerMapMenuDataComponent*
    GetLocalMapDataComponent()
    {
        SDK::ACrCharacterPlayerBase* player = nullptr;
        if (!MiniMapMap::TryGetPhysicalCharacter(player)) return nullptr;

        auto* component = player->PlayerMapMenuDataComponent;
        const auto* native = MiniMapNative::Get();
        if (!native || !MiniMapNative::ValidateObjectIdentity(
            reinterpret_cast<uintptr_t>(component),
            reinterpret_cast<uintptr_t>(native->asset.objectArray), sizeof(*component)) ||
            component->Outer != player) return nullptr;
        return component;
    }


    PluginTextureHandle LoadIconFromBulkData(
        SDK::UTexture2D* texture,
        const char* name)
    {
        auto* self = GetSelf();
        if (texture == nullptr ||
            name == nullptr ||
            !g_pluginSelfAvailable ||
            self == nullptr ||
            self->hooks == nullptr ||
            self->hooks->ImGuiTextures == nullptr)
        {
            return nullptr;
        }

        MiniMapNative::TextureAccess::PlatformData
            platformData = {};

        if (!MiniMapNative::TextureAccess::QueryPlatformData(
            texture,
            platformData))
        {
            return nullptr;
        }

        MiniMapNative::TextureAccess::MipData
            mipData = {};

        if (!MiniMapNative::TextureAccess::QueryMipData(
            platformData,
            0,
            mipData))
        {
            return nullptr;
        }

        void* bulkCopy =
            nullptr;

        if (!MiniMapNative::TextureAccess::CopyBulkData(
            mipData,
            &bulkCopy,
            false))
        {
            return nullptr;
        }

        std::vector<uint8_t> rgba;

        const bool decoded =
            MiniMapBC3::DecodeBC3Texture(
                static_cast<const uint8_t*>(
                    bulkCopy),
                static_cast<int>(
                    mipData.SizeX),
                static_cast<int>(
                    mipData.SizeY),
                static_cast<size_t>(
                    mipData.BulkSize),
                rgba);

        MiniMapNative::TextureAccess::FreeBulkDataCopy(
            bulkCopy);

        if (!decoded)
        {
            return nullptr;
        }

        auto* textures =
            self->
                hooks->
                ImGuiTextures;

        if (textures->GetFreeSlotCount() <=
            0)
        {
            return nullptr;
        }

        PluginTextureHandle handle =
            textures->LoadFromRGBA(
                rgba.data(),
                static_cast<int>(
                    mipData.SizeX),
                static_cast<int>(
                    mipData.SizeY),
                name);

        if (handle ==
            nullptr)
        {
            return nullptr;
        }

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        LOG_INFO(
            "MiniMap: cached POI icon "
            "name=%s size=%ux%u source=CPU-BC3",
            name,
            static_cast<unsigned int>(
                mipData.SizeX),
            static_cast<unsigned int>(
                mipData.SizeY));
#endif

        return handle;
    }



    const SDK::FCrAbandonBaseData* FindReplicatedAbandonBaseData(
        SDK::UWorld* world,
        const POIGuid& guid)
    {
        if (world == nullptr)
        {
            return nullptr;
        }

        /*
         * These two generated fields are verified against the current PDB
         * and native UpdatePOIMarkers path:
         *
         *   UWorld::GameState                         +0x1B0
         *   ACrGameStateBase::AbandonBaseInfoReplicator +0x3A8
         *
         * The tracked MiniMap world is the gameplay world. We do not retain
         * either pointer across polls.
         */
        SDK::AGameStateBase* baseGameState =
            world->GameState;

        if (baseGameState == nullptr)
        {
            return nullptr;
        }

        auto* gameState =
            static_cast<SDK::ACrGameStateBase*>(
                baseGameState);

        SDK::ACrAbandonBaseInfoReplicator* replicator =
            gameState->
                AbandonBaseInfoReplicator;

        if (replicator == nullptr)
        {
            return nullptr;
        }

        const auto& entries = replicator->AbandonBaseDataContainer.AbandonBaseData;
        const auto* data = entries.GetDataPtr();
        const int32_t count = entries.Num();
        const int32_t capacity = entries.Max();

        if (count < 0 ||
            capacity < count ||
            count >
                kMaximumReasonableAbandonBaseRecords ||
            capacity >
                kMaximumReasonableAbandonBaseRecords)
        {
            return nullptr;
        }

        if (count == 0)
        {
            return nullptr;
        }

        if (data == nullptr)
        {
            return nullptr;
        }

        for (int32_t index = 0;
            index < count;
            ++index)
        {
            const auto& record = data[index];
            const auto& id = record.AbandonBasePOIId;
            const POIGuid recordGuid{static_cast<uint32_t>(id.A), static_cast<uint32_t>(id.B),
                static_cast<uint32_t>(id.C), static_cast<uint32_t>(id.D)};

            if (recordGuid ==
                guid)
            {
                return &record;
            }
        }

        /*
         * Do not call native FindAbandonBaseData for an absent record.
         * The native helper enters an Unreal ensure path on lookup miss.
         */
        return nullptr;
    }


    bool IsCompletedAbandonedBase(
        SDK::UWorld* world,
        const POIGuid& guid)
    {
        const MiniMapNative::NativeApi* native =
            MiniMapNative::Get();

        if (native == nullptr ||
            native->pointsOfInterest.isAbandonBaseCompleted == nullptr)
        {
            return false;
        }

        const SDK::FCrAbandonBaseData* data =
            FindReplicatedAbandonBaseData(
                world,
                guid);

        if (data == nullptr)
        {
            return false;
        }

        return
            native->
                pointsOfInterest.
                isAbandonBaseCompleted(
                    data);
    }

    PluginTextureHandle ResolveIcon(
        const SDK::UCrMapMenuPOIData* category,
        uint8_t poiType,
        IconVariant variant)
    {
        if (category == nullptr)
        {
            return nullptr;
        }

        const uint32_t cacheKey =
            (static_cast<uint32_t>(
                poiType) <<
                8) |
            static_cast<uint32_t>(
                variant);

        const auto existing =
            g_iconCache.find(
                cacheKey);

        if (existing !=
            g_iconCache.end())
        {
            return existing->second;
        }

        const MiniMapNative::NativeApi* native =
            MiniMapNative::Get();

        if (native == nullptr ||
            native->texture.getBrushTexture == nullptr)
        {
            return nullptr;
        }

        const SDK::FSlateBrush* brush = &category->Icon;

        if (variant == IconVariant::Unknown)
        {
            brush = &category->UnknownIcon;
        }
        else if (variant == IconVariant::Completed)
        {
            brush = &category->AbandonBaseCompletedIcon;
        }

        SDK::UTexture2D* texture =
            native->
                texture.
                getBrushTexture(
                    *brush);

        if (texture ==
            nullptr)
        {
            return nullptr;
        }

        char name[64] = {};

        const char* variantName =
            "Discovered";

        if (variant ==
            IconVariant::Unknown)
        {
            variantName =
                "Unknown";
        }
        else if (variant ==
            IconVariant::Completed)
        {
            variantName =
                "Completed";
        }

        std::snprintf(
            name,
            sizeof(name),
            "MiniMap_POI_%u_%s",
            static_cast<unsigned int>(
                poiType),
            variantName);

        PluginTextureHandle handle =
            LoadIconFromBulkData(
                texture,
                name);

        if (handle ==
            nullptr)
        {
            return nullptr;
        }

        g_iconCache.emplace(
            cacheKey,
            handle);

        return handle;
    }


    bool TryBuildRecord(
        SDK::AActor* actor,
        SDK::UCrPlayerMapMenuDataComponent* mapData,
        POIRecord& outRecord)
    {
        outRecord = {};

        if (actor == nullptr ||
            mapData == nullptr ||
            g_devSettings == nullptr)
        {
            return false;
        }

        const MiniMapNative::NativeApi* native =
            MiniMapNative::Get();

        if (native == nullptr ||
            native->pointsOfInterest.findPOIMarkerCategoryData == nullptr ||
            native->pointsOfInterest.getMarkerFilterStatus == nullptr ||
            native->player.getComponentLocation == nullptr)
        {
            return false;
        }

        const auto* marker = static_cast<const SDK::ACrPointOfInterestMarkerActor*>(actor);
        const uint8_t state = static_cast<uint8_t>(marker->PointOfInterestState);

        if (state ==
                kStateHidden ||
            state ==
                kStateDisabled)
        {
            return false;
        }

        if (state !=
                kStateVisibleUnknown &&
            state !=
                kStateVisibleDiscovered)
        {
            return false;
        }

        const uint8_t poiType = static_cast<uint8_t>(marker->PointOfInterestType);

        const SDK::UCrMapMenuPOIData* category =
            native->
                pointsOfInterest.
                findPOIMarkerCategoryData(
                    g_devSettings,
                    poiType);

        if (category ==
            nullptr)
        {
            return false;
        }

        const bool unknown =
            state ==
            kStateVisibleUnknown;

        const uint8_t leafFilter =
            unknown
                ? kUnknownFilter
                : static_cast<uint8_t>(category->LegendFilter);

        if (!native->
                pointsOfInterest.
                getMarkerFilterStatus(
                    mapData,
                    leafFilter))
        {
            return false;
        }

        const auto& id = marker->Guid;
        const POIGuid guid{static_cast<uint32_t>(id.A), static_cast<uint32_t>(id.B),
                static_cast<uint32_t>(id.C), static_cast<uint32_t>(id.D)};

        IconVariant iconVariant =
            unknown
                ? IconVariant::Unknown
                : IconVariant::Discovered;

        if (!unknown &&
            poiType ==
                kAbandonedBaseType)
        {
            SDK::UWorld* world =
                MiniMapMap::GetWorld();

            if (IsCompletedAbandonedBase(
                world,
                guid))
            {
                iconVariant =
                    IconVariant::Completed;
            }
        }

        PluginTextureHandle icon =
            ResolveIcon(
                category,
                poiType,
                iconVariant);

        /*
         * If the completed asset is not available yet, retain the ordinary
         * discovered marker for this poll and retry the completed variant on
         * the next poll.
         */
        if (icon == nullptr &&
            iconVariant ==
                IconVariant::Completed)
        {
            icon =
                ResolveIcon(
                    category,
                    poiType,
                    IconVariant::Discovered);
        }

        if (icon ==
            nullptr)
        {
            return false;
        }

        SDK::USceneComponent* root = actor->RootComponent;

        if (root ==
            nullptr)
        {
            return false;
        }

        SDK::FVector location = {};

        native->
            player.
            getComponentLocation(
                root,
                &location);

        // Discovery/classification stays native. Fog admission is presentation
        // state and must use the current feather at draw time, not a poll-time
        // native-mask-only exclusion.
        outRecord.CanHideByFog = category->CanBeHiddenByFogOfWar;

        outRecord.Guid =
            guid;

        outRecord.WorldX =
            location.X;

        outRecord.WorldY =
            location.Y;

        outRecord.Icon =
            icon;

        return true;
    }


    void PollPointsOfInterest()
    {
        SDK::UWorld* world =
            MiniMapMap::GetWorld();

        const MiniMapNative::NativeApi* native =
            MiniMapNative::Get();

        if (world == nullptr ||
            native == nullptr ||
            native->pointsOfInterest.getAllActorsOfClass == nullptr ||
            native->pointsOfInterest.pointOfInterestStaticClass == nullptr ||
            native->texture.memoryFree == nullptr)
        {
            return;
        }

        SDK::UCrPlayerMapMenuDataComponent* mapData =
            GetLocalMapDataComponent();

        if (mapData ==
            nullptr)
        {
            return;
        }

        if (g_devSettings ==
            nullptr)
        {
            if (auto* self = GetSelf())
                g_devSettings = MiniMapNative::FindMapMenuDevSettingsCDO(self, false);

            if (g_devSettings ==
                nullptr)
            {
                return;
            }
        }

        SDK::UClass* poiClass =
            native->
                pointsOfInterest.
                pointOfInterestStaticClass();

        if (poiClass ==
            nullptr)
        {
            return;
        }

        NativeActorClassStorage classStorage = {};

        classStorage.Class =
            poiClass;

        NativeActorArray actors = {};

        native->
            pointsOfInterest.
            getAllActorsOfClass(
                static_cast<const SDK::UObject*>(
                    world),
                &classStorage,
                &actors);

        std::vector<POIRecord> nextRecords;

        if (actors.Num >
                0 &&
            actors.Data !=
                nullptr)
        {
            nextRecords.reserve(
                static_cast<std::size_t>(
                    actors.Num));

            for (int32_t index = 0;
                index < actors.Num;
                ++index)
            {
                POIRecord record = {};

                if (TryBuildRecord(
                    actors.Data[index],
                    mapData,
                    record))
                {
                    nextRecords.push_back(
                        record);
                }
            }
        }

        if (actors.Data !=
            nullptr)
        {
            native->
                texture.
                memoryFree(
                    actors.Data);
        }

        {
            std::scoped_lock lock(
                g_snapshotMutex);

            g_records =
                std::move(
                    nextRecords);
        }
    }


    void OnTick(
        float deltaSeconds)
    {
        if(!MiniMapUI::IsVisible())return;
        if(!MiniMapMap::GameplayReady())return;
        if (!g_experienceReady ||
            !MiniMapMap::HasWorld())
        {
            return;
        }

        const auto settings = MiniMapConfig::GetRenderSettings().POI;
        if (!settings.ShowLayer) return;
        g_snapshotAccumulator +=
            deltaSeconds;

        if (g_snapshotAccumulator <
            kSnapshotIntervalSeconds)
        {
            return;
        }

        g_snapshotAccumulator =
            0.0f;

        PollPointsOfInterest();
    }
}


namespace MiniMapPointsOfInterest
{
    bool Initialize(
        IPluginSelf* self)
    {
        if (g_initialized)
        {
            return true;
        }

        if (self == nullptr ||
            self->hooks == nullptr ||
            self->hooks->Engine == nullptr)
        {
            return false;
        }

        g_pluginSelfAvailable = true;
        Reset();

        self->
            hooks->
            Engine->
            RegisterOnTick(
                &OnTick);

        g_tickRegistered =
            true;

        g_initialized =
            true;

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        LOG_INFO(
            "MiniMap: landmark POI subsystem initialized");
#endif

        return true;
    }


    void Reset()
    {
        {
            std::scoped_lock lock(
                g_snapshotMutex);

            g_records.clear();
        }

        g_devSettings =
            nullptr;

        g_experienceReady =
            false;

        g_snapshotAccumulator =
            0.0f;
    }


    void OnExperienceLoadComplete()
    {
        if (!g_initialized)
        {
            return;
        }

        /*
         * Let one ordinary snapshot interval elapse before the first scan.
         * Category soft references were observed to become usable shortly
         * after the experience-complete callback rather than synchronously
         * with it.
         */
        g_experienceReady =
            true;

        g_snapshotAccumulator =
            0.0f;

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        LOG_INFO(
            "MiniMap: landmark POI acquisition enabled");
#endif
    }


    void Render(
        IModLoaderImGui* ui,
        const MiniMapMap::Transform& transform,
        const MiniMapConfig::POIRenderSettings& settings,
        const MiniMapConfig::FogOfWarSettings& fog)
    {
        if (ui == nullptr ||
            !transform.Valid)
        {
            return;
        }

        std::vector<POIRecord> records;

        {
            std::scoped_lock lock(
                g_snapshotMutex);

            records =
                g_records;
        }

        if (records.empty())
        {
            return;
        }

        PluginDrawList drawList =
            ui->GetWindowDrawList();

        if (drawList ==
            nullptr)
        {
            return;
        }

        const float viewportX =
            transform.CenterX -
            transform.Width *
            0.5f;

        const float viewportY =
            transform.CenterY -
            transform.Height *
            0.5f;

        const float iconSize =
            MiniMapMap::CalculateMapMarkerIconSize(
                transform.MetersPerPixel, settings.MarkerScale);

        const float halfIcon =
            iconSize *
            0.5f;

        ui->DL_PushClipRect(
            drawList,
            viewportX,
            viewportY,
            viewportX +
                transform.Width,
            viewportY +
                transform.Height,
            true);

        for (const POIRecord& record :
            records)
        {
            if (record.Icon ==
                nullptr)
            {
                continue;
            }

            MiniMapMap::ScreenPoint point = {};

            if (!transform.WorldToScreen(
                record.WorldX,
                record.WorldY,
                point))
            {
                continue;
            }

            if (point.X <
                    viewportX -
                        halfIcon ||
                point.X >
                    viewportX +
                        transform.Width +
                        halfIcon ||
                point.Y <
                    viewportY -
                        halfIcon ||
                point.Y >
                    viewportY +
                        transform.Height +
                        halfIcon)
            {
                continue;
            }

            if (record.CanHideByFog && !(MiniMapFogOfWar::GetRenderedVisibilityAtWorldPosition(
                record.WorldX, record.WorldY, transform, fog) > 0.0f)) continue;

            ui->DL_AddImage(
                drawList,
                record.Icon,
                point.X -
                    halfIcon,
                point.Y -
                    halfIcon,
                point.X +
                    halfIcon,
                point.Y +
                    halfIcon,
                0.0f,
                0.0f,
                1.0f,
                1.0f,
                0xFFFFFFFFu);
        }

        ui->DL_PopClipRect(
            drawList);
    }


    void Shutdown()
    {
        if (!g_initialized)
        {
            return;
        }

        auto* self = GetSelf();
        if (g_tickRegistered &&
            self != nullptr &&
            self->hooks != nullptr &&
            self->hooks->Engine != nullptr)
        {
            self->
                hooks->
                Engine->
                UnregisterOnTick(
                    &OnTick);
        }

        g_tickRegistered =
            false;

        Reset();

        /*
         * Deliberately retain cached immutable icon textures. AlienX does not
         * currently provide fence-aware texture retirement.
         */
        g_pluginSelfAvailable =
            false;

        g_initialized =
            false;

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        LOG_INFO(
            "MiniMap: landmark POI subsystem shut down");
#endif
    }
}

#endif
