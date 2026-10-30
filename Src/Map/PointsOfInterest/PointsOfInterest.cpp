#include "Services/Viewport.h"
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
#include "Services/Runtime.h"
#include "Services/RuntimeEvents.h"
#include "Native/MapMenuDevSettingsLookup.h"
#include "plugin_interface.h"

#include "SDK/Chimera_classes.hpp"
#include "SDK/ChimeraUI_classes.hpp"
#include "SDK/Engine_classes.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    MiniMap::Services::Registration g_tickRegistration{};
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
        MiniMap::Services::Texture Icon{};
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

    struct CachedPOIIcon
    {
        MiniMap::Services::OwnedTexture Owner;
        MiniMap::Services::Texture View{};
        bool OwnershipUncertain = false;
    };

    // The persistent cache is the sole owner of each immutable POI acquisition.
    // Records and render snapshots copy only the non-owning neutral view.
    std::unordered_map<uint32_t, CachedPOIIcon>
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


    bool LoadIconFromBulkData(
        SDK::UTexture2D* texture,
        const char* name,
        MiniMap::Services::OwnedTexture& owner,
        MiniMap::Services::Texture& view,
        bool& ownershipUncertain)
    {
        view = {};
        ownershipUncertain = false;

        auto* textures = MiniMap::Services::TryTextures();
        if (texture == nullptr ||
            name == nullptr ||
            textures == nullptr ||
            !textures->Available())
        {
            return false;
        }

        MiniMapNative::TextureAccess::PlatformData
            platformData = {};

        if (!MiniMapNative::TextureAccess::QueryPlatformData(
            texture,
            platformData))
        {
            return false;
        }

        MiniMapNative::TextureAccess::MipData
            mipData = {};

        if (!MiniMapNative::TextureAccess::QueryMipData(
            platformData,
            0,
            mipData))
        {
            return false;
        }

        void* bulkCopy =
            nullptr;

        if (!MiniMapNative::TextureAccess::CopyBulkData(
            mipData,
            &bulkCopy,
            false))
        {
            return false;
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
            return false;
        }

        const auto freeSlots = textures->ObserveFreeSlots();
        if (freeSlots.status.error != MiniMap::Services::Error::None ||
            freeSlots.count == 0)
        {
            return false;
        }

        const MiniMap::Services::TextureSize size{
            static_cast<uint32_t>(mipData.SizeX),
            static_cast<uint32_t>(mipData.SizeY)};
        const MiniMap::Services::TextureRequest request{name, size};
        const auto upload = textures->UploadRGBA(
            std::span<const uint8_t>(rgba.data(), rgba.size()),
            size,
            request,
            owner);

        switch (upload)
        {
        case MiniMap::Services::UploadState::Complete:
        {
            const auto candidateView = owner.View();
            const auto validatedSize = owner.Size();
            if (!owner.IsValidated() ||
                candidateView.id == 0 ||
                !validatedSize ||
                validatedSize->width != size.width ||
                validatedSize->height != size.height)
            {
                // A malformed completion retains its receipt but never becomes drawable.
                ownershipUncertain = true;
                return false;
            }

            view = candidateView;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            LOG_INFO(
                "MiniMap: cached POI icon "
                "name=%s size=%ux%u source=CPU-BC3",
                name,
                static_cast<unsigned int>(mipData.SizeX),
                static_cast<unsigned int>(mipData.SizeY));
#endif
            return true;
        }

        case MiniMap::Services::UploadState::Unavailable:
        case MiniMap::Services::UploadState::Rejected:
            // A failed result with a receipt is not an ordinary retryable failure.
            ownershipUncertain = owner.HasRecord();
            return false;

        case MiniMap::Services::UploadState::OwnershipUncertain:
            ownershipUncertain = true;
            return false;
        }

        return false;
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

    MiniMap::Services::Texture ResolveIcon(
        const SDK::UCrMapMenuPOIData* category,
        uint8_t poiType,
        IconVariant variant)
    {
        if (category == nullptr)
        {
            return {};
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
            return existing->second.OwnershipUncertain
                ? MiniMap::Services::Texture{}
                : existing->second.View;
        }

        const MiniMapNative::NativeApi* native =
            MiniMapNative::Get();

        if (native == nullptr ||
            native->texture.getBrushTexture == nullptr)
        {
            return {};
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
            return {};
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

        // Allocate the final owner node before decoding reaches UploadRGBA.
        auto entry = g_iconCache.end();
        try
        {
            const auto prepared = g_iconCache.try_emplace(cacheKey);
            entry = prepared.first;
            if (!prepared.second)
            {
                return entry->second.OwnershipUncertain
                    ? MiniMap::Services::Texture{}
                    : entry->second.View;
            }
        }
        catch (...)
        {
            // No backend acquisition has started, so the next eligible poll may retry.
            return {};
        }

        bool ownershipUncertain = false;
        try
        {
            const bool loaded = LoadIconFromBulkData(
                texture,
                name,
                entry->second.Owner,
                entry->second.View,
                ownershipUncertain);
            if (loaded)
            {
                return entry->second.View;
            }
        }
        catch (...)
        {
            if (entry->second.Owner.HasRecord())
            {
                // Any exception after service receipt acquisition is fail-closed.
                entry->second.View = {};
                entry->second.OwnershipUncertain = true;
                LOG_ERROR(
                    "MiniMap: POI icon ownership uncertain; "
                    "cache key latched name=%s",
                    name);
                return {};
            }

            g_iconCache.erase(entry);
            return {};
        }

        if (ownershipUncertain || entry->second.Owner.HasRecord())
        {
            entry->second.View = {};
            entry->second.OwnershipUncertain = true;
            LOG_ERROR(
                "MiniMap: POI icon ownership/result uncertain; "
                "cache key latched name=%s",
                name);
            return {};
        }

        // Unavailable/rejected with no receipt remains eligible for the normal poll retry.
        g_iconCache.erase(entry);

        return {};
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

        MiniMap::Services::Texture icon =
            ResolveIcon(
                category,
                poiType,
                iconVariant);

        /*
         * If the completed asset is not available yet, retain the ordinary
         * discovered marker for this poll and retry the completed variant on
         * the next poll.
         */
        if (icon.id == 0 &&
            iconVariant ==
                IconVariant::Completed)
        {
            icon =
                ResolveIcon(
                    category,
                    poiType,
                    IconVariant::Discovered);
        }

        if (icon.id == 0)
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
            g_devSettings = MiniMapNative::FindMapMenuDevSettingsCDO(false);

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
            MiniMap::Services::TryRuntimeEvents() == nullptr)
        {
            return false;
        }

        Reset();

        const auto tickReceipt = MiniMap::Services::TryRuntimeEvents()->OnTick(
            [](void*, float delta) { OnTick(delta); }, nullptr);
        if (tickReceipt.token.id) g_tickRegistration = tickReceipt.token;
        if (tickReceipt.acceptance == MiniMap::Services::Acceptance::Rejected ||
            tickReceipt.acceptance == MiniMap::Services::Acceptance::BackendUnknown)
        {
            if (g_tickRegistration.id) MiniMap::Services::TryRuntimeEvents()->Remove(g_tickRegistration);
            return false;
        }

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
        MiniMap::Services::ICanvas& ui,
        const MiniMapMap::Transform& transform,
        const MiniMapConfig::POIRenderSettings& settings,
        const MiniMapConfig::FogOfWarSettings& fog)
    {
        if (!transform.Valid)
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

        MiniMap::Services::DrawLayer drawList =
            MiniMap::Services::DrawLayer::Window;

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

        ui.PushClip(drawList, {viewportX, viewportY}, {viewportX + transform.Width, viewportY + transform.Height}, true);

        for (const POIRecord& record :
            records)
        {
            if (record.Icon.id == 0)
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

            ui.Image(drawList, record.Icon,
                {point.X - halfIcon, point.Y - halfIcon}, {point.X + halfIcon, point.Y + halfIcon},
                {0.0f, 0.0f}, {1.0f, 1.0f}, 0xFFFFFFFFu);
        }

        ui.PopClip(drawList);
    }


    void Shutdown()
    {
        if (!g_initialized)
        {
            return;
        }

        if (g_tickRegistered)
        {
            if (auto* events = MiniMap::Services::TryRuntimeEvents())
            {
                const auto removal = events->Remove(g_tickRegistration);
                if (removal.registry == MiniMap::Services::RegistryRemoval::RequestReturned &&
                    removal.core == MiniMap::Services::CoreDrain::Quiescent) g_tickRegistration = {};
            }
        }

        g_tickRegistered =
            false;

        Reset();

        // Deliberately retain cached immutable texture receipts across shutdown.
        g_initialized =
            false;

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        LOG_INFO(
            "MiniMap: landmark POI subsystem shut down");
#endif
    }
}

#endif
