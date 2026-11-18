#include "Services/Viewport.h"
#if defined(MODLOADER_CLIENT_BUILD)

#include "Foundables.h"
#include "plugin.h"
#include "DiagnosticExports.h"
#include "UI/MiniMapUI.h"
#include "Config/Config.h"

#include "Map/FogOfWar/FogOfWar.h"
#include "Map/Map.h"
#include "Map/MarkerIconSizing.h"

#include "Native/NativeApi.h"
#include "Native/ResidentMetadata.h"
#include "Native/TextureAccess.h"
#include "Native/BC3Decode.h"
#include "plugin_helpers.h"
#include "Services/Runtime.h"
#include "Services/RuntimeEvents.h"
#include "Native/MapMenuDevSettingsLookup.h"

#include "SDK/CoreUObject_classes.hpp"
#include "SDK/Chimera_classes.hpp"
#include "SDK/Chimera_structs.hpp"
#include "SDK/ChimeraUI_classes.hpp"
#include "SDK/Engine_classes.hpp"
#include "SDK/AuItems_classes.hpp"
#include "SDK/MassEntity_structs.hpp"
#include "plugin_map.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    PluginField<SDK::FCrItemsStorageContainer> g_inputInventoryContainerField{};
    PluginField<SDK::FCrItemsStorageContainer> g_mainInventoryContainerField{};
    PluginField<SDK::TArray<SDK::FCrStorageItem>> g_storageItemsField{};
    PluginField<SDK::FAuSimpleItem> g_storageItemValueField = SR_FIELD(
        SDK::FAuSimpleItem, FCrStorageItem, Item, PLUGIN_FIELD_FLAG_NONE);
    PluginField<SDK::FAuItemId> g_simpleItemIdField{};
    PluginField<SDK::UAuItemDataBase*> g_simpleItemDataBaseField{};
    PluginField<int32_t> g_simpleItemCountField{};
    PluginField<SDK::FGuid> g_itemIdHandleField{};
    PluginField<SDK::EUIItemType> g_itemDataBaseUIItemTypeField{};
    PluginField<SDK::ECrFoundableType> g_foundableTypeField{};
    PluginField<SDK::FTransform> g_transformFragmentTransformField{};
    PluginField<SDK::FVector> g_transformTranslationField{};
    PluginField<decltype(SDK::FVector::X)> g_vectorXField{};
    PluginField<decltype(SDK::FVector::Y)> g_vectorYField{};
    PluginField<decltype(SDK::FVector::Z)> g_vectorZField{};
    PluginField<SDK::FSlateBrush> g_categoryIconField{};
    PluginField<bool> g_categoryBlockedField{};
    PluginField<SDK::ECrMapMenuMarkerFilter> g_categoryLegendFilterField{};
    PluginField<SDK::AGameStateBase*> g_worldGameStateField{};
    PluginField<SDK::ACrAbandonBaseInfoReplicator*> g_gameStateAbandonBaseReplicatorField{};
    PluginField<SDK::FCrAbandonBaseDataContainer> g_replicatorAbandonBaseContainerField{};
    PluginField<SDK::TArray<SDK::FCrAbandonBaseData>> g_abandonBaseContainerDataField{};
    PluginField<decltype(SDK::ACrPointOfInterestMarkerActor::PointOfInterestType)> g_poiMarkerTypeField{};
    PluginField<SDK::FGuid> g_poiMarkerGuidField{};
    PluginField<SDK::TArray<SDK::FGuid>> g_poiStoryItemsAssignedField{};
    PluginField<SDK::FGuid> g_datapadGuidField{};
    PluginField<SDK::FGuid> g_audiologGuidField{};
    PluginField<SDK::USceneComponent*> g_actorRootComponentField{};
    PluginField<decltype(SDK::UObject::Flags)> g_objectFlagsField{};
    PluginField<SDK::UObject*> g_objectOuterField{};
    PluginField<SDK::UWorld*> g_levelOwningWorldField{};
    PluginBitField g_actorBeingDestroyedField{};
    PluginField<SDK::FGuid> g_abandonBaseGuidField = SR_FIELD(
        SDK::FGuid, FCrAbandonBaseData, AbandonBasePOIId, PLUGIN_FIELD_FLAG_NONE);
    PluginField<SDK::TArray<SDK::FCrAbandonBaseDataCollectedStatus>> g_abandonBaseStatusesField = SR_FIELD(
        SDK::TArray<SDK::FCrAbandonBaseDataCollectedStatus>, FCrAbandonBaseData,
        StoryItemsCollectedStatus, PLUGIN_FIELD_FLAG_NONE);
    PluginField<SDK::FGuid> g_collectedStatusGuidField = SR_FIELD(
        SDK::FGuid, FCrAbandonBaseDataCollectedStatus, Item, PLUGIN_FIELD_FLAG_NONE);
    PluginField<uint8_t> g_collectedStatusValueField = SR_FIELD(
        uint8_t, FCrAbandonBaseDataCollectedStatus, Status, PLUGIN_FIELD_FLAG_NONE);
    PluginFieldSpec g_storageItemSize = SR_SIZE(FCrStorageItem);
    PluginFieldSpec g_abandonBaseSize = SR_SIZE(FCrAbandonBaseData);
    PluginFieldSpec g_collectedStatusSize = SR_SIZE(FCrAbandonBaseDataCollectedStatus);

    MiniMap::Services::Registration g_tickRegistration{};
    constexpr float kPollIntervalSeconds = 3.0f;
    constexpr float kOrdinaryContainerDotRadiusPixels = 2.5f;
    constexpr uint32_t kOrdinaryContainerDotColor = 0xFF00FFFFu; // Opaque yellow in ImGui ABGR.
    constexpr float kBlueprintContainerDotRadiusPixels = 5.0f;
    constexpr uint32_t kBlueprintContainerDotColor = 0xFFFF8626u; // Opaque #2686FF blue in ImGui ABGR.

    constexpr uint8_t kDeadBodyType = static_cast<uint8_t>(SDK::ECrFoundableType::DeadBody);
    constexpr uint8_t kDroneType = static_cast<uint8_t>(SDK::ECrFoundableType::Drone);
    constexpr uint8_t kUndefinedType = static_cast<uint8_t>(SDK::ECrFoundableType::Undefined);

    constexpr std::size_t kMassQuerySize = 0x350;
    constexpr std::size_t kMassQueryAlignment = 0x10;

    static_assert(kMassQuerySize == sizeof(SDK::FMassEntityQuery),
        "Foundables fixed query storage must match the generated Client SDK query size");
    static_assert(kMassQueryAlignment == alignof(SDK::FMassEntityQuery),
        "Foundables fixed query storage must match the generated Client SDK query alignment");
    static_assert(sizeof(void*) == 0x08);
    static_assert(sizeof(decltype(SDK::FCrItemsStorageContainer::Items)) == 0x10);
    static_assert(alignof(decltype(SDK::FCrItemsStorageContainer::Items)) == 0x08);
    static_assert(sizeof(SDK::FCrInventoryFragment) == 0x250);
    static_assert(alignof(SDK::FCrInventoryFragment) == 0x08);
    static_assert(offsetof(SDK::FCrInventoryFragment, InputInventoryContainer) == 0x00);
    static_assert(sizeof(decltype(SDK::FCrInventoryFragment::InputInventoryContainer)) == 0x120);
    static_assert(offsetof(SDK::FCrInventoryFragment, MainInventoryContainer) == 0x120);
    static_assert(sizeof(decltype(SDK::FCrInventoryFragment::MainInventoryContainer)) == 0x120);
    static_assert(sizeof(SDK::FCrItemsStorageContainer) == 0x120);
    static_assert(alignof(SDK::FCrItemsStorageContainer) == 0x08);
    static_assert(offsetof(SDK::FCrItemsStorageContainer, Items) == 0x108);
    static_assert(sizeof(SDK::FCrStorageItem) == 0x68);
    static_assert(alignof(SDK::FCrStorageItem) == 0x08);
    static_assert(offsetof(SDK::FCrStorageItem, Item) == 0x10);
    static_assert(sizeof(decltype(SDK::FCrStorageItem::Item)) == 0x30);
    static_assert(sizeof(SDK::FAuSimpleItem) == 0x30);
    static_assert(alignof(SDK::FAuSimpleItem) == 0x08);
    static_assert(offsetof(SDK::FAuSimpleItem, ItemId) == 0x0C);
    static_assert(sizeof(decltype(SDK::FAuSimpleItem::ItemId)) == 0x10);
    static_assert(offsetof(SDK::FAuSimpleItem, ItemDataBase) == 0x20);
    static_assert(sizeof(decltype(SDK::FAuSimpleItem::ItemDataBase)) == sizeof(void*));
    static_assert(std::is_same_v<decltype(SDK::FAuSimpleItem::ItemDataBase), SDK::UAuItemDataBase*>);
    static_assert(offsetof(SDK::FAuSimpleItem, Count) == 0x28);
    static_assert(sizeof(decltype(SDK::FAuSimpleItem::Count)) == 0x04);
    static_assert(std::is_same_v<decltype(SDK::FAuSimpleItem::Count), int32_t>);
    static_assert(sizeof(SDK::FAuItemId) == 0x10);
    static_assert(alignof(SDK::FAuItemId) == 0x04);
    static_assert(offsetof(SDK::FAuItemId, Handle) == 0x00);
    static_assert(sizeof(decltype(SDK::FAuItemId::Handle)) == 0x10);
    static_assert(std::is_same_v<decltype(SDK::FAuItemId::Handle), SDK::FGuid>);
    static_assert(sizeof(SDK::FGuid) == 0x10);
    static_assert(alignof(SDK::FGuid) == 0x04);
    static_assert(offsetof(SDK::FGuid, A) == 0x00);
    static_assert(offsetof(SDK::FGuid, B) == 0x04);
    static_assert(offsetof(SDK::FGuid, C) == 0x08);
    static_assert(offsetof(SDK::FGuid, D) == 0x0C);
    static_assert(sizeof(decltype(SDK::FGuid::A)) == 0x04);
    static_assert(sizeof(decltype(SDK::FGuid::B)) == 0x04);
    static_assert(sizeof(decltype(SDK::FGuid::C)) == 0x04);
    static_assert(sizeof(decltype(SDK::FGuid::D)) == 0x04);
    static_assert(sizeof(SDK::TArray<SDK::FCrAbandonBaseData>) == 0x10);
    static_assert(alignof(SDK::TArray<SDK::FCrAbandonBaseData>) == 0x08);
    static_assert(sizeof(SDK::TArray<SDK::FCrAbandonBaseDataCollectedStatus>) == 0x10);
    static_assert(alignof(SDK::TArray<SDK::FCrAbandonBaseDataCollectedStatus>) == 0x08);
    static_assert(sizeof(SDK::TArray<SDK::FGuid>) == 0x10);
    static_assert(alignof(SDK::TArray<SDK::FGuid>) == 0x08);
    static_assert(sizeof(SDK::FCrAbandonBaseDataContainer) == 0x120);
    static_assert(alignof(SDK::FCrAbandonBaseDataContainer) == 0x08);
    static_assert(offsetof(SDK::FCrAbandonBaseDataContainer, AbandonBaseData) == 0x108);
    static_assert(sizeof(decltype(SDK::FCrAbandonBaseDataContainer::AbandonBaseData)) == 0x10);
    static_assert(std::is_same_v<
        decltype(SDK::FCrAbandonBaseDataContainer::AbandonBaseData),
        SDK::TArray<SDK::FCrAbandonBaseData>>);
    static_assert(offsetof(SDK::ACrAbandonBaseInfoReplicator, AbandonBaseDataContainer) == 0x2F0);
    static_assert(sizeof(decltype(SDK::ACrAbandonBaseInfoReplicator::AbandonBaseDataContainer)) == 0x120);
    static_assert(offsetof(SDK::ACrGameStateBase, AbandonBaseInfoReplicator) == 0x3A8);
    static_assert(sizeof(decltype(SDK::ACrGameStateBase::AbandonBaseInfoReplicator)) == sizeof(void*));
    static_assert(sizeof(SDK::FCrAbandonBaseData) == 0x78);
    static_assert(alignof(SDK::FCrAbandonBaseData) == 0x08);
    static_assert(offsetof(SDK::FCrAbandonBaseData, AbandonBasePOIId) == 0x0C);
    static_assert(sizeof(decltype(SDK::FCrAbandonBaseData::AbandonBasePOIId)) == 0x10);
    static_assert(std::is_same_v<decltype(SDK::FCrAbandonBaseData::AbandonBasePOIId), SDK::FGuid>);
    static_assert(offsetof(SDK::FCrAbandonBaseData, StoryItemsCollectedStatus) == 0x50);
    static_assert(sizeof(decltype(SDK::FCrAbandonBaseData::StoryItemsCollectedStatus)) == 0x10);
    static_assert(std::is_same_v<
        decltype(SDK::FCrAbandonBaseData::StoryItemsCollectedStatus),
        SDK::TArray<SDK::FCrAbandonBaseDataCollectedStatus>>);
    static_assert(sizeof(SDK::FCrAbandonBaseDataCollectedStatus) == 0x14);
    static_assert(alignof(SDK::FCrAbandonBaseDataCollectedStatus) == 0x04);
    static_assert(offsetof(SDK::FCrAbandonBaseDataCollectedStatus, Item) == 0x00);
    static_assert(sizeof(decltype(SDK::FCrAbandonBaseDataCollectedStatus::Item)) == 0x10);
    static_assert(std::is_same_v<
        decltype(SDK::FCrAbandonBaseDataCollectedStatus::Item), SDK::FGuid>);
    static_assert(offsetof(SDK::FCrAbandonBaseDataCollectedStatus, Status) == 0x10);
    static_assert(sizeof(bool) == 0x01);
    static_assert(std::is_same_v<
        decltype(SDK::FCrAbandonBaseDataCollectedStatus::Status), bool>);
    static_assert(offsetof(SDK::ACrDatapadBase, Guid) == 0x2B8);
    static_assert(offsetof(SDK::ACrAudiologBase, Guid) == 0x2B8);
    static_assert(std::is_same_v<decltype(SDK::ACrDatapadBase::Guid), SDK::FGuid>);
    static_assert(std::is_same_v<decltype(SDK::ACrAudiologBase::Guid), SDK::FGuid>);
    static_assert(offsetof(SDK::ULevel, OwningWorld) == 0xC0);
    static_assert(sizeof(decltype(SDK::ULevel::OwningWorld)) == sizeof(void*));
    static_assert(std::is_same_v<decltype(SDK::ULevel::OwningWorld), SDK::UWorld*>);
    static_assert(offsetof(SDK::UObject, Flags) == 0x08);
    static_assert(sizeof(decltype(SDK::UObject::Flags)) == 0x04);
    static_assert(offsetof(SDK::UObject, Outer) == 0x20);
    static_assert(sizeof(decltype(SDK::UObject::Outer)) == sizeof(void*));
    static_assert(offsetof(SDK::UCrMapMenuCategoryData, CategoryBlocked) == 0x30);
    static_assert(sizeof(decltype(SDK::UCrMapMenuCategoryData::CategoryBlocked)) == 0x01);
    static_assert(std::is_same_v<decltype(SDK::UCrMapMenuCategoryData::CategoryBlocked), bool>);
    static_assert(offsetof(SDK::UAuItemDataBase, UIItemType) == 0x329);
    static_assert(sizeof(decltype(SDK::UAuItemDataBase::UIItemType)) == 0x01);
    static_assert(std::is_same_v<decltype(SDK::UAuItemDataBase::UIItemType), SDK::EUIItemType>);
    static_assert(sizeof(SDK::FCrMassFoundableParameters) == 0x0C);
    static_assert(alignof(SDK::FCrMassFoundableParameters) == 0x04);
    static_assert(offsetof(SDK::FCrMassFoundableParameters, FoundableType) == 0x00);
    static_assert(sizeof(decltype(SDK::FCrMassFoundableParameters::FoundableType)) == 0x01);
    static_assert(std::is_same_v<decltype(SDK::FCrMassFoundableParameters::FoundableType), SDK::ECrFoundableType>);
    static_assert(std::is_same_v<std::underlying_type_t<SDK::ECrFoundableType>, uint8_t>);
    static_assert(kUndefinedType == 0x00);
    static_assert(kDeadBodyType == 0x01);
    static_assert(kDroneType == 0x02);
    static_assert(sizeof(std::underlying_type_t<SDK::EUIItemType>) == 0x01);
    static_assert(std::is_same_v<std::underlying_type_t<SDK::EUIItemType>, uint8_t>);
    static_assert(static_cast<uint8_t>(SDK::EUIItemType::BlueprintItem) == 0x0B);

    struct FoundableRecord
    {
        uint8_t Type = 0;
        double WorldX = 0.0;
        double WorldY = 0.0;
        MiniMap::Services::Texture Icon{};
    };

    struct FoundableTypeState
    {
        MiniMap::Services::Texture Icon{};
        bool Enabled = false;
    };

    struct CachedFoundableIcon
    {
        MiniMap::Services::OwnedTexture Owner;
        MiniMap::Services::Texture View{};
        bool OwnershipUncertain = false;
    };

    bool g_initialized = false;
    bool g_tickRegistered = false;
    bool g_experienceReady = false;
    float g_pollAccumulator = kPollIntervalSeconds;
    float g_storyPollAccumulator = kPollIntervalSeconds;
    uint64_t g_storyPollGeneration = 0;

    SDK::UCrMapMenuDevSettings* g_devSettings = nullptr;

    SDK::UWorld* g_queryWorld = nullptr;
    SDK::UMassEntitySubsystem* g_massSubsystem = nullptr;
    const void* g_massManager = nullptr;

    alignas(kMassQueryAlignment)
        std::array<std::byte, kMassQuerySize> g_queryStorage = {};

    bool g_queryConstructed = false;

    std::mutex g_snapshotMutex;
    uint64_t g_snapshotWorldGeneration = 0;
    std::vector<FoundableRecord> g_records;
    enum class ContainerContents : uint8_t
    {
        Empty,
        Ordinary,
        Blueprint
    };

    struct ContainerDotRecord
    {
        double WorldX = 0.0;
        double WorldY = 0.0;
        ContainerContents Classification = ContainerContents::Empty;
    };
    std::vector<ContainerDotRecord> g_massContainerDots;
    std::vector<ContainerDotRecord> g_storyDots;
    std::vector<ContainerDotRecord> g_containerDots;
    // The cache is the sole owner of each immutable acquisition. Records and
    // render snapshots copy only the non-owning neutral identity.
    std::unordered_map<uint8_t, CachedFoundableIcon> g_iconCache;

    bool LoadIconFromBulkData(
        SDK::UTexture2D* texture,
        const char* name,
        MiniMap::Services::OwnedTexture& owner,
        MiniMap::Services::Texture& view,
        bool& ownershipUncertain)
    {
        ownershipUncertain = false;
        auto* textures = MiniMap::Services::TryTextures();
        if (texture == nullptr ||
            name == nullptr ||
            textures == nullptr ||
            !textures->Available())
        {
            return false;
        }

        MiniMapNative::TextureAccess::PlatformData platformData = {};
        if (!MiniMapNative::TextureAccess::QueryPlatformData(
            texture,
            platformData))
        {
            return false;
        }

        MiniMapNative::TextureAccess::MipData mipData = {};
        if (!MiniMapNative::TextureAccess::QueryMipData(
            platformData,
            0,
            mipData))
        {
            return false;
        }

        void* bulkCopy = nullptr;
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
                static_cast<const uint8_t*>(bulkCopy),
                static_cast<int>(mipData.SizeX),
                static_cast<int>(mipData.SizeY),
                static_cast<size_t>(mipData.BulkSize),
                rgba);

        MiniMapNative::TextureAccess::FreeBulkDataCopy(bulkCopy);

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
            std::span<const uint8_t>(rgba.data(), rgba.size()), size, request, owner);

        if (upload == MiniMap::Services::UploadState::OwnershipUncertain)
        {
            ownershipUncertain = true;
            return false;
        }

        if (upload == MiniMap::Services::UploadState::Complete)
        {
            view = owner.View();
            const auto validatedSize = owner.Size();
            if (owner.IsValidated() && view.id != 0 && validatedSize &&
                validatedSize->width == size.width && validatedSize->height == size.height)
            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                LOG_INFO(
                    "MiniMap: cached foundable icon "
                    "name=%s size=%ux%u source=CPU-BC3",
                    name,
                    static_cast<unsigned int>(mipData.SizeX),
                    static_cast<unsigned int>(mipData.SizeY));
#endif
                return true;
            }

            // Complete must include a validated token and matching dimensions.
            // Retain the receipt and fail closed if that contract is violated.
            ownershipUncertain = true;
        }

        return false;
    }

    MiniMap::Services::Texture ResolveFoundableIcon(
        const SDK::UCrMapMenuCategoryData* category,
        uint8_t foundableType)
    {
        const auto existing = g_iconCache.find(foundableType);
        if (existing != g_iconCache.end())
        {
            return existing->second.OwnershipUncertain
                ? MiniMap::Services::Texture{}
                : existing->second.View;
        }

        const MiniMapNative::NativeApi* native = MiniMapNative::Get();

        if (category == nullptr ||
            native == nullptr ||
            native->texture.getBrushTexture == nullptr)
        {
            return {};
        }

        const SDK::FSlateBrush* icon =
            g_categoryIconField.In(category);
        if (icon == nullptr)
        {
            return {};
        }

        SDK::UTexture2D* texture =
            native->texture.getBrushTexture(*icon);

        if (texture == nullptr)
        {
            return {};
        }

        char name[64] = {};
        std::snprintf(
            name,
            sizeof(name),
            "MiniMap_Foundable_%u",
            static_cast<unsigned int>(foundableType));

        // Allocate the final cache node before any backend acquisition. The
        // upload writes its move-only receipt directly into this owner slot.
        auto entry = g_iconCache.end();
        try
        {
            const auto prepared = g_iconCache.try_emplace(foundableType);
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
            // Cache allocation failed before any acquisition; retry on the
            // next ordinary foundable poll.
            return {};
        }

        bool acquired = false;
        bool ownershipUncertain = false;
        try
        {
            acquired = LoadIconFromBulkData(
                texture, name, entry->second.Owner, entry->second.View,
                ownershipUncertain);
        }
        catch (...)
        {
            if (entry->second.Owner.HasRecord())
            {
                // Preserve acquisition debt and prevent publication
                // or retry after an interrupted acquisition.
                entry->second.OwnershipUncertain = true;
                entry->second.View = {};

                LOG_ERROR(
                    "MiniMap: foundable icon acquisition interrupted "
                    "for name=%s; receipt retained and retry disabled",
                    name);
            }
            else
            {
                // No acquisition debt: ordinary polling may retry.
                g_iconCache.erase(entry);
            }

            return {};
        }

        if (acquired)
        {
            return entry->second.View;
        }

        if (ownershipUncertain || entry->second.Owner.HasRecord())
        {
            entry->second.OwnershipUncertain = true;
            entry->second.View = {};
            LOG_ERROR("MiniMap: foundable icon ownership uncertain for name=%s; acquisition retained and retry disabled for this type", name);
            return {};
        }

        g_iconCache.erase(entry);
        return {};
    }

    void DestroyQuery()
    {
        if (g_queryConstructed)
        {
            const MiniMapNative::NativeApi* native = MiniMapNative::Get();

            if (native != nullptr &&
                native->mass.queryDestruct != nullptr)
            {
                native->mass.queryDestruct(g_queryStorage.data());
            }

            g_queryStorage.fill(std::byte{ 0 });
        }

        g_queryConstructed = false;
        g_queryWorld = nullptr;
        g_massSubsystem = nullptr;
        g_massManager = nullptr;
    }

    SDK::UScriptStruct* ResolveDroppedItemsTag()
    {
        if (!MiniMapResidentMetadata::Available())
        {
            return nullptr;
        }

        uintptr_t scriptStructClass = 0;
        if (!MiniMapResidentMetadata::FindResident(
                nullptr,
                L"/Script/CoreUObject.ScriptStruct",
                false,
                scriptStructClass) ||
            scriptStructClass == 0)
        {
            return nullptr;
        }

        uintptr_t droppedItemsTag = 0;
        if (!MiniMapResidentMetadata::FindResident(
                reinterpret_cast<SDK::UClass*>(scriptStructClass),
                L"/Script/Chimera.CrMassDroppedItemsTag",
                true,
                droppedItemsTag) ||
            droppedItemsTag == 0)
        {
            return nullptr;
        }

        return reinterpret_cast<SDK::UScriptStruct*>(droppedItemsTag);
    }

    bool EnsureQuery()
    {
        SDK::UWorld* world = MiniMapMap::GetWorld();
        const MiniMapNative::NativeApi* native = MiniMapNative::Get();

        if (world == nullptr ||
            native == nullptr ||
            native->mass.entityManagerFieldOffset < 0 ||
            native->mass.getMassEntitySubsystem == nullptr ||
            native->mass.queryConstruct == nullptr ||
            native->mass.addInventoryRequirement == nullptr ||
            native->mass.addTransformRequirement == nullptr ||
            native->mass.addFoundableParametersRequirement == nullptr ||
            native->mass.addFoundableTagRequirement == nullptr ||
            native->mass.addTagRequirement == nullptr)
        {
            return false;
        }

        SDK::UMassEntitySubsystem* subsystem =
            native->mass.getMassEntitySubsystem(world);

        if (subsystem == nullptr)
        {
            return false;
        }

        const uint8_t* subsystemBytes =
            reinterpret_cast<const uint8_t*>(subsystem);
        const uint8_t* sharedPtrStorage =
            subsystemBytes + native->mass.entityManagerFieldOffset;

        const void* manager = nullptr;
        std::memcpy(
            &manager,
            sharedPtrStorage,
            sizeof(manager));

        if (manager == nullptr)
        {
            return false;
        }

        if (g_queryConstructed &&
            g_queryWorld == world &&
            g_massSubsystem == subsystem &&
            g_massManager == manager)
        {
            return true;
        }

        SDK::UScriptStruct* droppedItemsTag =
            ResolveDroppedItemsTag();
        if (droppedItemsTag == nullptr)
        {
            return false;
        }

        DestroyQuery();

        g_queryStorage.fill(std::byte{ 0 });

        native->mass.queryConstruct(
            g_queryStorage.data(),
            sharedPtrStorage);

        g_queryConstructed = true;

        constexpr uint8_t kReadOnlyAccess = 1;
        constexpr uint8_t kPresenceAll = 0;
        constexpr uint8_t kPresenceAny = 1;

        native->mass.addInventoryRequirement(
            g_queryStorage.data(),
            kReadOnlyAccess,
            kPresenceAll);

        native->mass.addTransformRequirement(
            g_queryStorage.data(),
            kReadOnlyAccess,
            kPresenceAll);

        native->mass.addFoundableParametersRequirement(
            g_queryStorage.data(),
            kPresenceAll);

        native->mass.addFoundableTagRequirement(
            g_queryStorage.data(),
            kPresenceAny);

        native->mass.addTagRequirement(
            g_queryStorage.data(),
            droppedItemsTag,
            SDK::EMassFragmentPresence::None);

        g_queryWorld = world;
        g_massSubsystem = subsystem;
        g_massManager = manager;

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        LOG_INFO("MiniMap: Mass foundable query constructed");
#endif
        return true;
    }

    template<class T>
    bool ValidSpan(const T* data, int32_t count, int32_t capacity, size_t elementSize = sizeof(T))
    {
        if (count < 0 || capacity < count || elementSize == 0 || (count && !data))
            return false;

        const size_t maxSize = (std::numeric_limits<size_t>::max)();
        if (size_t(count) > maxSize / elementSize)
            return false;

        const size_t spanBytes = size_t(count) * elementSize;
        return reinterpret_cast<uintptr_t>(data) <= UINTPTR_MAX - spanBytes;
    }

    template<class T>
    bool ReadArray(const SDK::TArray<T>& array, const T*& data, int32_t& count,
        size_t elementSize = sizeof(T))
    {
        data = array.GetDataPtr();
        count = array.Num();
        return ValidSpan(data, count, array.Max(), elementSize);
    }

    bool InventoryHasItems(const void* inventoryFragment)
    {
        if (inventoryFragment == nullptr)
        {
            return false;
        }

        const SDK::FCrItemsStorageContainer* mainContainer =
            g_mainInventoryContainerField.In(inventoryFragment);
        if (mainContainer == nullptr)
        {
            return false;
        }

        const SDK::TArray<SDK::FCrStorageItem>* inventory =
            g_storageItemsField.In(mainContainer);
        if (inventory == nullptr)
        {
            return false;
        }

        const SDK::FCrStorageItem* data = nullptr;
        int32_t count = 0;
        if (!ReadArray(*inventory, data, count, g_storageItemSize.resolvedSize) || count <= 0)
        {
            return false;
        }

        for (int32_t index = 0; index < count; ++index)
        {
            const uint8_t* element = PluginMap::ElementAt(data, count, index, g_storageItemSize);
            const SDK::FAuSimpleItem* item = g_storageItemValueField.In(element);
            if (!item)
                return false;
            const int32_t* itemCount = g_simpleItemCountField.In(item);
            if (itemCount == nullptr)
            {
                return false;
            }

            if (*itemCount > 0)
            {
                return true;
            }
        }

        return false;
    }

    ContainerContents ClassifyContainerContents(
        const SDK::FCrInventoryFragment& inventory)
    {
        const SDK::FGuid zeroGuid = {};

        bool hasOrdinaryItem = false;
        const auto inspect = [&](const SDK::FCrItemsStorageContainer* container)
        {
            if (container == nullptr)
            {
                return false;
            }

            const SDK::TArray<SDK::FCrStorageItem>* items =
                g_storageItemsField.In(container);
            if (items == nullptr)
            {
                return false;
            }

            const SDK::FCrStorageItem* data = nullptr;
            int32_t count = 0;
            if (!ReadArray(*items, data, count, g_storageItemSize.resolvedSize))
                return false;
            for (int32_t index = 0; index < count; ++index)
            {
                const uint8_t* element = PluginMap::ElementAt(data, count, index, g_storageItemSize);
                const SDK::FAuSimpleItem* item = g_storageItemValueField.In(element);
                if (!item)
                    return false;

                SDK::UAuItemDataBase* const* itemDataBase =
                    g_simpleItemDataBaseField.In(item);
                const SDK::FAuItemId* itemId =
                    g_simpleItemIdField.In(item);
                const int32_t* itemCount =
                    g_simpleItemCountField.In(item);
                if (itemDataBase == nullptr ||
                    itemId == nullptr ||
                    itemCount == nullptr)
                {
                    return false;
                }

                if (*itemDataBase == nullptr)
                {
                    continue;
                }

                const SDK::FGuid* itemHandle =
                    g_itemIdHandleField.In(itemId);
                if (itemHandle == nullptr)
                {
                    return false;
                }

                if (std::memcmp(itemHandle, &zeroGuid, sizeof(zeroGuid)) == 0 ||
                    *itemCount <= 0)
                {
                    continue;
                }

                const SDK::EUIItemType* itemType =
                    g_itemDataBaseUIItemTypeField.In(*itemDataBase);
                if (itemType == nullptr)
                {
                    return false;
                }

                if (*itemType == SDK::EUIItemType::BlueprintItem)
                {
                    return true;
                }

                hasOrdinaryItem = true;
            }

            return false;
        };

        const SDK::FCrItemsStorageContainer* inputContainer =
            g_inputInventoryContainerField.In(&inventory);
        const SDK::FCrItemsStorageContainer* mainContainer =
            g_mainInventoryContainerField.In(&inventory);
        if (inputContainer == nullptr || mainContainer == nullptr)
        {
            return ContainerContents::Empty;
        }

        if (inspect(inputContainer) ||
            inspect(mainContainer))
        {
            return ContainerContents::Blueprint;
        }

        return hasOrdinaryItem
            ? ContainerContents::Ordinary
            : ContainerContents::Empty;
    }

    const SDK::FCrMassFoundableParameters* ResolveFoundableParametersData(
        const void* constSharedWrapper,
        const SDK::UScriptStruct* parametersStruct)
    {
        if (constSharedWrapper == nullptr ||
            parametersStruct == nullptr)
        {
            return nullptr;
        }

        const uint8_t* sharedMemory = nullptr;
        std::memcpy(
            &sharedMemory,
            constSharedWrapper,
            sizeof(sharedMemory));

        if (sharedMemory == nullptr)
        {
            return nullptr;
        }

        const int16_t alignment = parametersStruct->MinAlignment;

        if (alignment <= 0 ||
            (alignment & (alignment - 1)) != 0)
        {
            return nullptr;
        }

        const uintptr_t base =
            reinterpret_cast<uintptr_t>(
                sharedMemory + sizeof(void*));

        const uintptr_t aligned =
            (base + static_cast<uintptr_t>(alignment - 1)) &
            ~static_cast<uintptr_t>(alignment - 1);

        return reinterpret_cast<const SDK::FCrMassFoundableParameters*>(aligned);
    }

    bool ReadWorldPosition(
        const void* transformFragment,
        double& worldX,
        double& worldY)
    {
        if (transformFragment == nullptr)
        {
            return false;
        }

        const SDK::FTransform* transform =
            g_transformFragmentTransformField.In(transformFragment);
        if (transform == nullptr)
        {
            return false;
        }

        const SDK::FVector* translation =
            g_transformTranslationField.In(transform);
        if (translation == nullptr)
        {
            return false;
        }

        const auto* x = g_vectorXField.In(translation);
        const auto* y = g_vectorYField.In(translation);
        if (x == nullptr || y == nullptr)
        {
            return false;
        }

        worldX = *x;
        worldY = *y;

        return true;
    }

    bool BuildTypeState(
        SDK::UCrPlayerMapMenuDataComponent* mapData,
        uint8_t foundableType,
        FoundableTypeState& state)
    {
        state = {};

        const MiniMapNative::NativeApi* native = MiniMapNative::Get();

        if (native == nullptr ||
            g_devSettings == nullptr ||
            mapData == nullptr ||
            native->pointsOfInterest.findFoundableMarkerCategoryData == nullptr ||
            native->pointsOfInterest.getMarkerFilterStatus == nullptr)
        {
            return false;
        }

        const SDK::UCrMapMenuCategoryData* category =
            native->pointsOfInterest.findFoundableMarkerCategoryData(
                g_devSettings,
                foundableType);

        if (category == nullptr)
        {
            return false;
        }

        const bool* categoryBlocked =
            g_categoryBlockedField.In(category);
        if (categoryBlocked == nullptr || *categoryBlocked)
        {
            return false;
        }

        const SDK::ECrMapMenuMarkerFilter* legendFilter =
            g_categoryLegendFilterField.In(category);
        if (legendFilter == nullptr)
        {
            return false;
        }

        const uint8_t filter = static_cast<uint8_t>(*legendFilter);

        if (!native->pointsOfInterest.getMarkerFilterStatus(
            mapData,
            filter))
        {
            return false;
        }

        const MiniMap::Services::Texture icon =
            ResolveFoundableIcon(category, foundableType);

        if (icon.id == 0)
        {
            return false;
        }

        state.Icon = icon;
        state.Enabled = true;
        return true;
    }

    namespace StoryItems
    {
        using Guid = std::array<uint32_t, 4>;

        struct GuidHash
        {
            size_t operator()(const Guid& guid) const noexcept
            {
                size_t hash = 0;
                for (auto word : guid) hash ^= size_t(word) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
                return hash;
            }
        };

        Guid CopyGuid(const SDK::FGuid& guid)
        {
            return {uint32_t(guid.A), uint32_t(guid.B), uint32_t(guid.C), uint32_t(guid.D)};
        }

        bool ValidGuid(const Guid& guid)
        {
            return (guid[0] | guid[1] | guid[2] | guid[3]) != 0;
        }

        // All metadata and actor pointers below are borrowed only inside one
        // quiet, synchronous game-thread poll. Nothing is cached or rendered.
        struct Types
        {
            const MiniMapNative::NativeApi& Native;
            SDK::UClass* Core = nullptr;
            SDK::UClass* World = nullptr;
            SDK::UClass* Level = nullptr;
            SDK::UClass* Scene = nullptr;
            SDK::UClass* GameState = nullptr;
            SDK::UClass* Replicator = nullptr;
            SDK::UClass* POI = nullptr;
            SDK::UClass* Blueprint = nullptr;
            SDK::UClass* Datapad = nullptr;
            SDK::UClass* Audiolog = nullptr;

            bool IsClass(const SDK::UClass* type) const
            {
                if (!type) return false;
                auto* meta = type->Class;
                for (int depth = 0; meta && depth < 64; ++depth)
                {
                    if (!meta->Class || meta->Class != Core) return false;
                    if (meta == Core) return true;
                    meta = static_cast<SDK::UClass*>(meta->SuperStruct);
                }
                return false;
            }

            bool Derives(const SDK::UClass* type, const SDK::UClass* base) const
            {
                if (!base || !IsClass(type)) return false;
                for (int depth = 0; type && depth < 128; ++depth)
                {
                    if (depth != 0 && (!type->Class || type->Class->Class != Core)) return false;
                    if (type == base) return true;
                    type = static_cast<const SDK::UClass*>(type->SuperStruct);
                }
                return false;
            }

            SDK::UClass* FindClass(const wchar_t* path, size_t minimumSize) const
            {
                uintptr_t result = 0;
                if (!MiniMapResidentMetadata::FindResident(Core,
                        path, false, result)) return nullptr;
                auto* type = reinterpret_cast<SDK::UClass*>(result);
                if (!IsClass(type) || type->Size < 0 || size_t(type->Size) < minimumSize) return nullptr;
                const std::wstring fullPath(path);
                const auto separator = fullPath.rfind(L'.');
                if (separator == std::wstring::npos || !type->Outer ||
                    type->Outer->Outer != nullptr ||
                    !Native.asset.nameEqualsWide(type->Name, fullPath.c_str() + separator + 1) ||
                    !Native.asset.nameEqualsWide(type->Outer->Name, fullPath.substr(0, separator).c_str()))
                    return nullptr;
                return type;
            }

            SDK::UClass* FindNative(const wchar_t* path, size_t size) const
            {
                auto* type = FindClass(path, size);
                return type && type->Class == Core && size_t(type->Size) == size ? type : nullptr;
            }

            bool Initialize()
            {
                if (!MiniMapResidentMetadata::Available() || !Native.asset.nameEqualsWide) return false;
                uintptr_t core = 0;
                if (!MiniMapResidentMetadata::FindResident(nullptr,
                        L"/Script/CoreUObject.Class", false, core)) return false;
                Core = reinterpret_cast<SDK::UClass*>(core);
                if (!Core || Core->Class != Core) return false;
                World = FindNative(L"/Script/Engine.World", sizeof(SDK::UWorld));
                Level = FindNative(L"/Script/Engine.Level", sizeof(SDK::ULevel));
                Scene = FindNative(L"/Script/Engine.SceneComponent", sizeof(SDK::USceneComponent));
                GameState = FindNative(L"/Script/Chimera.CrGameStateBase", sizeof(SDK::ACrGameStateBase));
                Replicator = FindNative(L"/Script/Chimera.CrAbandonBaseInfoReplicator", sizeof(SDK::ACrAbandonBaseInfoReplicator));
                POI = FindNative(L"/Script/Chimera.CrPointOfInterestMarkerActor", sizeof(SDK::ACrPointOfInterestMarkerActor));
                Blueprint = FindNative(L"/Script/Engine.BlueprintGeneratedClass", sizeof(SDK::UBlueprintGeneratedClass));
                Datapad = FindNative(L"/Script/Chimera.CrDatapadBase", sizeof(SDK::ACrDatapadBase));
                Audiolog = FindNative(L"/Script/Chimera.CrAudiologBase", sizeof(SDK::ACrAudiologBase));
                return World && Level && Scene && GameState && Replicator && POI && Blueprint;
            }

            SDK::UClass* FindFamily(const wchar_t* path, SDK::UClass* nativeBase, size_t size) const
            {
                if (!nativeBase) return nullptr;
                auto* type = FindClass(path, size);
                return type && Derives(type, nativeBase) && Derives(type->Class, Blueprint) ? type : nullptr;
            }

            bool Actor(const SDK::AActor* actor, const SDK::UClass* base, size_t,
                const SDK::UWorld* world) const
            {
                if (!actor || g_actorBeingDestroyedField.Get(actor)) return false;
                const auto* flags = g_objectFlagsField.In(actor);
                if ((uint32_t(*flags) & (uint32_t(SDK::EObjectFlags::ClassDefaultObject) |
                    uint32_t(SDK::EObjectFlags::ArchetypeObject))) || !Derives(actor->Class, base)) return false;
                SDK::UObject* const* levelField = g_objectOuterField.In(actor);
                auto* level = *levelField;
                return level && Derives(level->Class, Level) &&
                    *g_levelOwningWorldField.In(static_cast<const SDK::ULevel*>(level)) == world;
            }
        };

        struct NativeActorArray
        {
            SDK::AActor** Data = nullptr;
            int32_t Num = 0;
            int32_t Max = 0;
        };
        static_assert(sizeof(NativeActorArray) == 0x10);

        struct ActorArray
        {
            NativeActorArray Values;
            void (*Free)(void*);
            explicit ActorArray(const MiniMapNative::NativeApi& native) : Free(native.texture.memoryFree) {}
            ActorArray(const ActorArray&) = delete;
            ActorArray& operator=(const ActorArray&) = delete;
            ~ActorArray() { if (Values.Data) Free(Values.Data); }

            bool Collect(const MiniMapNative::NativeApi& native, SDK::UWorld* world, SDK::UClass* type)
            {
                // Existing TSubclassOf ABI: one UClass* of caller-owned storage.
                native.pointsOfInterest.getAllActorsOfClass(world, &type, &Values);
                return ValidSpan(Values.Data, Values.Num, Values.Max);
            }
        };

        struct Progress
        {
            Guid Base = {};
            bool Ambiguous = false;
            uint8_t Status = 2; // UNKNOWN, never interpreted as uncollected.
            uint32_t Records = 0;
        };
        using ProgressMap = std::unordered_map<Guid, Progress, GuidHash>;

        struct ProgressScope
        {
            SDK::UWorld* World = nullptr;
            SDK::ACrGameStateBase* GameState = nullptr;
            SDK::ACrAbandonBaseInfoReplicator* Replicator = nullptr;

            bool Matches(const SDK::UWorld* world) const
            {
                return world && World && GameState && Replicator && world == World &&
                    *g_worldGameStateField.In(world) == GameState &&
                    *g_gameStateAbandonBaseReplicatorField.In(GameState) == Replicator;
            }
        };

        bool ReadProgress(const Types& types, SDK::UWorld* world, ProgressMap& progress, ProgressScope& scope)
        {
            if (!world || !types.Derives(world->Class, types.World)) return false;
            scope.World = world;
            auto* baseGameState = *g_worldGameStateField.In(world);
            if (!types.Actor(baseGameState, types.GameState, sizeof(SDK::ACrGameStateBase), world)) return false;
            auto* gameState = static_cast<SDK::ACrGameStateBase*>(baseGameState);
            scope.GameState = gameState;
            auto* replicator = *g_gameStateAbandonBaseReplicatorField.In(gameState);
            if (!types.Actor(replicator, types.Replicator, sizeof(*replicator), world)) return false;
            scope.Replicator = replicator;

            ActorArray actors(types.Native);
            if (!actors.Collect(types.Native, world, types.POI)) return false;
            std::unordered_set<Guid, GuidHash> assignedBases;
            std::unordered_set<Guid, GuidHash> ambiguousAssignedBases;
            for (int32_t index = 0; index < actors.Values.Num; ++index)
            {
                auto* actor = actors.Values.Data[index];
                if (!types.Actor(actor, types.POI, sizeof(SDK::ACrPointOfInterestMarkerActor), world)) continue;
                auto* poi = static_cast<SDK::ACrPointOfInterestMarkerActor*>(actor);
                if (*g_poiMarkerTypeField.In(poi) != SDK::ECrPointOfInterestType::AbandonedBase) continue;
                const auto base = CopyGuid(*g_poiMarkerGuidField.In(poi));
                if (!ValidGuid(base)) continue;
                if (!assignedBases.insert(base).second)
                {
                    ambiguousAssignedBases.insert(base);
                    for (auto& entry : progress)
                        if (entry.second.Base == base) entry.second.Ambiguous = true;
                    continue;
                }
                const SDK::FGuid* assignments = nullptr;
                int32_t count = 0;
                if (!ReadArray(*g_poiStoryItemsAssignedField.In(poi), assignments, count)) continue;
                for (int32_t item = 0; item < count; ++item)
                {
                    const auto guid = CopyGuid(assignments[item]);
                    if (!ValidGuid(guid)) continue;
                    const auto inserted = progress.emplace(guid, Progress{base});
                    if (!inserted.second) inserted.first->second.Ambiguous = true;
                }
            }

            const SDK::FCrAbandonBaseData* bases = nullptr;
            int32_t baseCount = 0;
            const SDK::FCrAbandonBaseDataContainer* container =
                g_replicatorAbandonBaseContainerField.In(replicator);
            const SDK::TArray<SDK::FCrAbandonBaseData>* baseArray =
                g_abandonBaseContainerDataField.In(container);
            if (!ReadArray(*baseArray, bases, baseCount,
                g_abandonBaseSize.resolvedSize)) return false;
            std::unordered_set<Guid, GuidHash> replicatedBases;
            std::unordered_set<Guid, GuidHash> ambiguousReplicatedBases;
            for (int32_t index = 0; index < baseCount; ++index)
            {
                const uint8_t* baseElement = PluginMap::ElementAt(bases, baseCount, index,
                    g_abandonBaseSize);
                const SDK::FGuid* baseGuid = g_abandonBaseGuidField.In(baseElement);
                const SDK::TArray<SDK::FCrAbandonBaseDataCollectedStatus>* storyStatuses =
                    g_abandonBaseStatusesField.In(baseElement);
                if (!baseGuid || !storyStatuses)
                    return false;
                const auto base = CopyGuid(*baseGuid);
                if (!ValidGuid(base)) continue;
                if (!replicatedBases.insert(base).second)
                {
                    ambiguousReplicatedBases.insert(base);
                    continue;
                }
                const SDK::FCrAbandonBaseDataCollectedStatus* statuses = nullptr;
                int32_t count = 0;
                if (!ReadArray(*storyStatuses, statuses, count, g_collectedStatusSize.resolvedSize)) continue;
                for (int32_t item = 0; item < count; ++item)
                {
                    const uint8_t* statusElement = PluginMap::ElementAt(statuses, count, item,
                        g_collectedStatusSize);
                    const SDK::FGuid* guidValue = g_collectedStatusGuidField.In(statusElement);
                    const uint8_t* statusValue = g_collectedStatusValueField.In(statusElement);
                    if (!guidValue || !statusValue)
                        return false;
                    const auto guid = CopyGuid(*guidValue);
                    if (!ValidGuid(guid)) continue;
                    const auto found = progress.find(guid);
                    if (found == progress.end() || found->second.Base != base) continue;
                    // Read the representation first; malformed bool bytes are UNKNOWN.
                    uint8_t status = 2;
                    std::memcpy(&status, statusValue, sizeof(status));
                    auto& entry = found->second;
                    ++entry.Records;
                    entry.Status = entry.Records == 1 && status <= 1 ? status : 2;
                }
            }
            for (auto& entry : progress)
                if (ambiguousAssignedBases.count(entry.second.Base) ||
                    ambiguousReplicatedBases.count(entry.second.Base)) entry.second.Ambiguous = true;
            return scope.Matches(world);
        }

        bool ReadPosition(const Types& types, SDK::AActor* actor, SDK::UWorld* world,
            SDK::UClass* family, size_t extent, SDK::FVector& location)
        {
            if (!actor) return false;
            auto* root = *g_actorRootComponentField.In(actor);
            if (!root || *g_objectOuterField.In(root) != actor ||
                !types.Derives(root->Class, types.Scene)) return false;
            types.Native.player.getComponentLocation(root, &location);
            const auto* x = g_vectorXField.In(&location);
            const auto* y = g_vectorYField.In(&location);
            const auto* z = g_vectorZField.In(&location);
            return types.Actor(actor, family, extent, world) &&
                *g_actorRootComponentField.In(actor) == root && *g_objectOuterField.In(root) == actor &&
                std::isfinite(*x) && std::isfinite(*y) && std::isfinite(*z);
        }

        bool Poll(const MiniMapNative::NativeApi& native, SDK::UWorld* world,
            std::vector<ContainerDotRecord>& dots)
        {
            if (!MiniMapResidentMetadata::Available() ||
                !native.pointsOfInterest.getAllActorsOfClass || !native.player.getComponentLocation ||
                !native.texture.memoryFree) return false;
            Types types{native};
            if (!types.Initialize()) return false;
            const uint64_t generation = MiniMapMap::GetWorldGeneration();
            ProgressMap progress;
            ProgressScope scope;
            if (!ReadProgress(types, world, progress, scope)) return false;

            struct Family { const wchar_t* Path; SDK::UClass* Base; size_t Extent; bool Audio; };
            const Family families[] = {
                {L"/Game/Chimera/Environment/Datapads/BP_DatapadComputer.BP_DatapadComputer_C",
                    types.Datapad, sizeof(SDK::ACrDatapadBase), false},
                {L"/Game/Chimera/Environment/Datapads/BP_DatapadPersonal.BP_DatapadPersonal_C",
                    types.Datapad, sizeof(SDK::ACrDatapadBase), false},
                {L"/Game/Chimera/Environment/Audiologs/BP_Audiolog.BP_Audiolog_C",
                    types.Audiolog, sizeof(SDK::ACrAudiologBase), true}
            };
            struct Candidate { ContainerDotRecord Dot; bool Ambiguous = false; };
            std::unordered_map<Guid, Candidate, GuidHash> candidates;
            for (const auto& family : families)
            {
                auto* type = types.FindFamily(family.Path, family.Base, family.Extent);
                if (!type) continue; // Unresolved resident family is UNKNOWN; retry next poll.
                ActorArray actors(native);
                if (!actors.Collect(native, world, type)) continue;
                for (int32_t index = 0; index < actors.Values.Num; ++index)
                {
                    auto* actor = actors.Values.Data[index];
                    if (!types.Actor(actor, type, family.Extent, world)) continue;
                    const auto guid = CopyGuid(*(family.Audio
                        ? g_audiologGuidField.In(actor)
                        : g_datapadGuidField.In(actor)));
                    const auto found = progress.find(guid);
                    if (!ValidGuid(guid) || found == progress.end() || found->second.Ambiguous ||
                        found->second.Records != 1 || found->second.Status != 0) continue;
                    SDK::FVector location{};
                    if (!ReadPosition(types, actor, world, type, family.Extent, location)) continue;
                    const auto currentGuid = CopyGuid(*(family.Audio
                        ? g_audiologGuidField.In(actor)
                        : g_datapadGuidField.In(actor)));
                    if (guid != currentGuid) continue;
                    Candidate candidate{{*g_vectorXField.In(&location),
                        *g_vectorYField.In(&location), ContainerContents::Ordinary}};
                    const auto inserted = candidates.emplace(guid, candidate);
                    // Conflicting actor ownership of one story is UNKNOWN, not a guessed position.
                    if (!inserted.second) inserted.first->second.Ambiguous = true;
                }
            }
            if (!MiniMapResidentMetadata::Available() || !scope.Matches(world) ||
                world != MiniMapMap::GetWorld() || generation != MiniMapMap::GetWorldGeneration()) return false;
            for (const auto& candidate : candidates)
                if (!candidate.second.Ambiguous) dots.push_back(candidate.second.Dot);
            return true;
        }
    }

    void ClearSnapshot()
    {
        std::scoped_lock lock(g_snapshotMutex);
        g_records.clear();
        g_massContainerDots.clear();
        g_storyDots.clear();
        g_containerDots.clear();
        g_snapshotWorldGeneration = 0;
    }

    bool PollMassFoundables(const MiniMapConfig::FoundableRenderSettings& settings,
        std::vector<FoundableRecord>& nextRecords, std::vector<ContainerDotRecord>& nextContainerDots)
    {
        const MiniMapNative::NativeApi* native = MiniMapNative::Get();
        SDK::UWorld* world = MiniMapMap::GetWorld();

        if (native == nullptr ||
            world == nullptr ||
            !EnsureQuery() ||
            native->mass.getMatchingEntityHandles == nullptr ||
            native->mass.getFragmentDataPtr == nullptr ||
            native->mass.getConstSharedFragmentPtr == nullptr ||
            native->mass.transformFragmentStaticStruct == nullptr ||
            native->mass.inventoryFragmentStaticStruct == nullptr ||
            native->mass.foundableParametersStaticStruct == nullptr ||
            native->texture.memoryFree == nullptr)
        {
            return false;
        }

        SDK::ACrCharacterPlayerBase* player = nullptr;
        SDK::UCrPlayerMapMenuDataComponent* mapData = nullptr;
        MiniMapMap::TryGetPlayerMapDataComponent(player, mapData);
        if (mapData != nullptr && g_devSettings == nullptr)
            g_devSettings = MiniMapNative::FindMapMenuDevSettingsCDO(false);

        FoundableTypeState deadBody = {};
        FoundableTypeState drone = {};
        BuildTypeState(mapData, kDeadBodyType, deadBody);
        BuildTypeState(mapData, kDroneType, drone);

        auto* transformStruct = native->mass.transformFragmentStaticStruct();
        auto* inventoryStruct = native->mass.inventoryFragmentStaticStruct();
        auto* parametersStruct = native->mass.foundableParametersStaticStruct();
        if (transformStruct == nullptr || inventoryStruct == nullptr || parametersStruct == nullptr)
        {
            return false;
        }

        MiniMapNative::MassEntityHandleArray handles = {};
        native->mass.getMatchingEntityHandles(g_queryStorage.data(), &handles);
        struct HandleArrayOwner
        {
            MiniMapNative::MassEntityHandleArray& Handles;
            const MiniMapNative::NativeApi& Native;
            ~HandleArrayOwner() { if (Handles.Data) Native.texture.memoryFree(Handles.Data); }
        } handleOwner{handles, *native};

        nextRecords.reserve(static_cast<std::size_t>(handles.Num));
        for (int32_t index = 0; index < handles.Num; ++index)
        {
            const auto id = handles.Data[index];
            const auto* parameters = ResolveFoundableParametersData(
                native->mass.getConstSharedFragmentPtr(g_massManager, id, parametersStruct),
                parametersStruct);
            if (parameters == nullptr) continue;

            const SDK::ECrFoundableType* foundableTypeField =
                g_foundableTypeField.In(parameters);
            if (foundableTypeField == nullptr)
            {
                continue;
            }

            const auto foundableType = static_cast<uint8_t>(*foundableTypeField);
            if (foundableType == kUndefinedType && !settings.ShowLootInBases) continue;
            const void* inventoryFragment = native->mass.getFragmentDataPtr(g_massManager, id, inventoryStruct);
            const auto* inventory = static_cast<const SDK::FCrInventoryFragment*>(inventoryFragment);
            const void* transform = native->mass.getFragmentDataPtr(g_massManager, id, transformStruct);
            double worldX = 0.0;
            double worldY = 0.0;
            if (!ReadWorldPosition(transform, worldX, worldY)) continue;

            if (foundableType == kUndefinedType)
            {
                if (inventory != nullptr)
                {
                    const ContainerContents contents = ClassifyContainerContents(*inventory);
                    if (contents != ContainerContents::Empty)
                    {
                        nextContainerDots.push_back({worldX, worldY, contents});
                    }
                }
                continue;
            }

            const FoundableTypeState* typeState = nullptr;
            if (foundableType == kDeadBodyType) typeState = &deadBody;
            else if (foundableType == kDroneType) typeState = &drone;
            else continue;

            if (!typeState->Enabled || typeState->Icon.id == 0 ||
                !InventoryHasItems(inventoryFragment))
            {
                continue;
            }

            FoundableRecord record = {};
            record.Type = foundableType;
            record.WorldX = worldX;
            record.WorldY = worldY;
            record.Icon = typeState->Icon;
            nextRecords.push_back(record);
        }

        return true;
    }

    bool PollFoundables(const MiniMapConfig::FoundableRenderSettings& settings,
        bool pollMass, bool pollStory, bool& massSucceeded)
    {
        const auto* native = MiniMapNative::Get();
        auto* world = MiniMapMap::GetWorld();
        const auto generation = MiniMapMap::GetWorldGeneration();
        massSucceeded = !pollMass;
        if (!native || !world)
        {
            ClearSnapshot();
            return false;
        }

        std::vector<FoundableRecord> records;
        std::vector<ContainerDotRecord> massDots;
        std::vector<ContainerDotRecord> storyDots;
        const bool storySucceeded = pollStory && settings.ShowLootInBases &&
            StoryItems::Poll(*native, world, storyDots);
        if (pollMass)
        {
            massSucceeded = PollMassFoundables(settings, records, massDots);
            if (!massSucceeded)
            {
                records.clear();
                massDots.clear();
            }
        }
        // Neither source's failure can erase the other source's completed data.
        if (!g_experienceReady || !MiniMapMap::GameplayReady() ||
            world != MiniMapMap::GetWorld() || generation != MiniMapMap::GetWorldGeneration())
        {
            ClearSnapshot();
            massSucceeded = false;
            return false;
        }

        {
            std::scoped_lock lock(g_snapshotMutex);
            if (pollMass)
            {
                g_records = std::move(records);
                g_massContainerDots = std::move(massDots);
            }
            if (storySucceeded) g_storyDots = std::move(storyDots);

            // No actor-to-Mass semantic link is established. Never collapse nearby loot.
            g_containerDots = g_massContainerDots;
            g_containerDots.insert(g_containerDots.end(), g_storyDots.begin(), g_storyDots.end());
            g_snapshotWorldGeneration = generation;
        }
        return true;
    }
    void OnTick(float deltaSeconds)
    {
        const uint64_t generation = MiniMapMap::GetWorldGeneration();
        if (generation != g_storyPollGeneration)
        {
            ClearSnapshot();
            g_storyPollGeneration = generation;
            g_storyPollAccumulator = kPollIntervalSeconds;
        }

        if(!MiniMapUI::IsVisible())return;
        if(!MiniMapMap::GameplayReady())return;
        if (!g_experienceReady ||
            !MiniMapMap::HasWorld())
        {
            return;
        }

        const auto settings = MiniMapConfig::GetRenderSettings().Foundables;
        if (!settings.ShowLayer) return;
        g_pollAccumulator += deltaSeconds;
        if (settings.ShowLootInBases) g_storyPollAccumulator += deltaSeconds;

        const bool pollMass = g_pollAccumulator >= kPollIntervalSeconds;
        const bool pollStory = settings.ShowLootInBases &&
            g_storyPollAccumulator >= kPollIntervalSeconds;
        if (!pollMass && !pollStory) return;

        bool massSucceeded = true;
        const bool snapshotValid = PollFoundables(settings, pollMass, pollStory, massSucceeded);
        if (pollMass && snapshotValid && massSucceeded)
        {
            g_pollAccumulator = 0.0f;
        }
        else if (pollMass)
        {
            g_pollAccumulator = kPollIntervalSeconds;
        }
        if (pollStory) g_storyPollAccumulator = 0.0f;
    }
}

namespace MiniMapFoundables
{
    void SetAcquisitionFieldOffsets(const AcquisitionFieldOffsets& offsets)
    {
        g_inputInventoryContainerField.offset = offsets.InventoryInputContainer;
        g_mainInventoryContainerField.offset = offsets.InventoryMainContainer;
        g_storageItemsField.offset = offsets.StorageContainerItems;
        g_simpleItemIdField.offset = offsets.SimpleItemId;
        g_simpleItemDataBaseField.offset = offsets.SimpleItemDataBase;
        g_simpleItemCountField.offset = offsets.SimpleItemCount;
        g_itemIdHandleField.offset = offsets.ItemIdHandle;
        g_itemDataBaseUIItemTypeField.offset = offsets.ItemDataBaseUIItemType;
        g_foundableTypeField.offset = offsets.FoundableType;
        g_transformFragmentTransformField.offset = offsets.TransformFragmentTransform;
        g_transformTranslationField.offset = offsets.TransformTranslation;
        g_vectorXField.offset = offsets.VectorX;
        g_vectorYField.offset = offsets.VectorY;
        g_vectorZField.offset = offsets.VectorZ;
        g_categoryIconField.offset = offsets.CategoryIcon;
        g_categoryBlockedField.offset = offsets.CategoryBlocked;
        g_categoryLegendFilterField.offset = offsets.CategoryLegendFilter;
        g_worldGameStateField.offset = offsets.WorldGameState;
        g_gameStateAbandonBaseReplicatorField.offset =
            offsets.GameStateAbandonBaseInfoReplicator;
        g_replicatorAbandonBaseContainerField.offset =
            offsets.ReplicatorAbandonBaseDataContainer;
        g_abandonBaseContainerDataField.offset = offsets.AbandonBaseContainerData;
        g_poiMarkerTypeField.offset = offsets.PoiMarkerType;
        g_poiMarkerGuidField.offset = offsets.PoiMarkerGuid;
        g_poiStoryItemsAssignedField.offset = offsets.PoiStoryItemsAssigned;
        g_datapadGuidField.offset = offsets.DatapadGuid;
        g_audiologGuidField.offset = offsets.AudiologGuid;
        g_actorRootComponentField.offset = offsets.ActorRootComponent;
        g_objectFlagsField.offset = offsets.ObjectFlags;
        g_objectOuterField.offset = offsets.ObjectOuter;
        g_levelOwningWorldField.offset = offsets.LevelOwningWorld;
        g_actorBeingDestroyedField.offset = offsets.ActorBeingDestroyed;
        g_actorBeingDestroyedField.boolMask = offsets.ActorBeingDestroyedMask;
        g_actorBeingDestroyedField.resolvedSize = offsets.ActorBeingDestroyed >= 0 ? 1 : 0;
    }

    void SetStorageItemContract(int32_t valueOffset, uint32_t elementSize)
    {
        g_storageItemValueField.offset = valueOffset;
        g_storageItemSize.resolvedSize = elementSize;
    }

    void SetStoryArrayContracts(const ArrayElementContracts& contracts)
    {
        g_abandonBaseGuidField.offset = contracts.AbandonBaseGuid;
        g_abandonBaseGuidField.resolvedSize = contracts.AbandonBaseGuidSize;
        g_abandonBaseStatusesField.offset = contracts.AbandonBaseStatuses;
        g_abandonBaseSize.resolvedSize = contracts.AbandonBaseSize;
        g_collectedStatusGuidField.offset = contracts.CollectedStatusGuid;
        g_collectedStatusValueField.offset = contracts.CollectedStatusValue;
        g_collectedStatusSize.resolvedSize = contracts.CollectedStatusSize;
    }

    ArrayElementContracts GetStoryArrayContracts()
    {
        ArrayElementContracts contracts{};
        contracts.StorageItemValue = g_storageItemValueField.offset;
        contracts.StorageItemSize = g_storageItemSize.resolvedSize;
        contracts.AbandonBaseGuid = g_abandonBaseGuidField.offset;
        contracts.AbandonBaseGuidSize = g_abandonBaseGuidField.resolvedSize;
        contracts.AbandonBaseStatuses = g_abandonBaseStatusesField.offset;
        contracts.AbandonBaseSize = g_abandonBaseSize.resolvedSize;
        contracts.CollectedStatusGuid = g_collectedStatusGuidField.offset;
        contracts.CollectedStatusValue = g_collectedStatusValueField.offset;
        contracts.CollectedStatusSize = g_collectedStatusSize.resolvedSize;
        return contracts;
    }

    bool Initialize(IPluginSelf* self)
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

        g_tickRegistered = true;
        g_initialized = true;

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        LOG_INFO("MiniMap: Mass foundable subsystem initialized");
#endif
        return true;
    }

    void Reset()
    {
        {
            std::scoped_lock lock(g_snapshotMutex);
            g_records.clear();
            g_massContainerDots.clear();
            g_storyDots.clear();
            g_containerDots.clear();
            g_snapshotWorldGeneration = 0;
        }

        DestroyQuery();

        g_devSettings = nullptr;
        g_experienceReady = false;
        g_pollAccumulator = kPollIntervalSeconds;
        g_storyPollAccumulator = kPollIntervalSeconds;
        g_storyPollGeneration = 0;
    }

    void OnExperienceLoadComplete()
    {
        if (!g_initialized)
        {
            return;
        }

        g_experienceReady = true;
        g_pollAccumulator = kPollIntervalSeconds;
        g_storyPollAccumulator = kPollIntervalSeconds;

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        LOG_INFO("MiniMap: Mass foundable acquisition enabled");
#endif
    }

    void Render(
        MiniMap::Services::ICanvas& ui,
        const MiniMapMap::Transform& transform,
        const MiniMapConfig::FoundableRenderSettings& settings,
        const MiniMapConfig::FogOfWarSettings& fog)
    {
        if (!transform.Valid)
        {
            return;
        }

        std::vector<FoundableRecord> records;
        std::vector<ContainerDotRecord> containerDots;

        {
            std::scoped_lock lock(g_snapshotMutex);
            if (g_snapshotWorldGeneration != MiniMapMap::GetWorldGeneration()) return;
            records = g_records;
            containerDots = g_containerDots;
        }

        MiniMap::Services::DrawLayer drawList = MiniMap::Services::DrawLayer::Window;

        const float viewportX =
            transform.CenterX - transform.Width * 0.5f;

        const float viewportY =
            transform.CenterY - transform.Height * 0.5f;

        const float iconSize =
            MiniMapMap::CalculateMapMarkerIconSize(
                transform.MetersPerPixel, settings.MarkerScale);

        const float halfIcon = iconSize * 0.5f;

        ui.PushClip(drawList, {viewportX, viewportY}, {viewportX + transform.Width, viewportY + transform.Height}, true);

        // MiniMapUI renders Foundables after terrain and Fog of War, then POIs.
        // Submit the existing icon snapshot first; container dots follow below.
        for (const FoundableRecord& record : records)
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

            if (point.X < viewportX - halfIcon ||
                point.X > viewportX + transform.Width + halfIcon ||
                point.Y < viewportY - halfIcon ||
                point.Y > viewportY + transform.Height + halfIcon)
            {
                continue;
            }

            if (!(MiniMapFogOfWar::GetRenderedVisibilityAtWorldPosition(
                record.WorldX, record.WorldY, transform, fog) > 0.0f))
            {
                continue;
            }

            ui.Image(drawList, record.Icon,
                {point.X - halfIcon, point.Y - halfIcon}, {point.X + halfIcon, point.Y + halfIcon},
                {0.0f, 0.0f}, {1.0f, 1.0f}, 0xFFFFFFFFu);
        }

        const auto renderContainerDots = [&](ContainerContents contents)
        {
            for (const ContainerDotRecord& dot : containerDots)
            {
                if (dot.Classification != contents)
                {
                    continue;
                }

                MiniMapMap::ScreenPoint point = {};
                if (!transform.WorldToScreen(dot.WorldX, dot.WorldY, point) ||
                    point.X < viewportX || point.X > viewportX + transform.Width ||
                    point.Y < viewportY || point.Y > viewportY + transform.Height)
                {
                    continue;
                }

                // Exact existing renderer exposure (native edge + local awareness feather).
                // Zero alone means fully black; partial exposure keeps the opaque dot.
                if (!(MiniMapFogOfWar::GetRenderedVisibilityAtWorldPosition(
                    dot.WorldX, dot.WorldY, transform, fog) > 0.0f))
                {
                    continue;
                }

                const bool isBlueprint = contents == ContainerContents::Blueprint;
                ui.FilledCircle(drawList, {point.X, point.Y}, (isBlueprint ? kBlueprintContainerDotRadiusPixels : kOrdinaryContainerDotRadiusPixels) * settings.MarkerScale, isBlueprint ? kBlueprintContainerDotColor : kOrdinaryContainerDotColor, 12);
            }
        };

        if (settings.ShowLootInBases)
        {
            renderContainerDots(ContainerContents::Ordinary);
            renderContainerDots(ContainerContents::Blueprint);
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

        g_tickRegistered = false;

        Reset();

        g_initialized = false;

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        LOG_INFO("MiniMap: Mass foundable subsystem shut down");
#endif
    }
}

#endif
