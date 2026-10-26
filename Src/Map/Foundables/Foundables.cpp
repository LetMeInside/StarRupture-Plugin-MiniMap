#include "CallbackTiming.h"
#if defined(MODLOADER_CLIENT_BUILD)

#include "Foundables.h"
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
#include "Native/MapMenuDevSettingsLookup.h"

#include "SDK/Chimera_classes.hpp"
#include "SDK/ChimeraUI_classes.hpp"
#include "SDK/Engine_classes.hpp"
#include "SDK/AuItems_classes.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    constexpr float kPollIntervalSeconds = 3.0f;
    constexpr float kOrdinaryContainerDotRadiusPixels = 2.5f;
    constexpr uint32_t kOrdinaryContainerDotColor = 0xFF00FFFFu; // Opaque yellow in ImGui ABGR.
    constexpr float kBlueprintContainerDotRadiusPixels = 5.0f;
    constexpr uint32_t kBlueprintContainerDotColor = 0xFFFF8626u; // Opaque #2686FF blue in ImGui ABGR.

    constexpr uint8_t kDeadBodyType = static_cast<uint8_t>(SDK::ECrFoundableType::DeadBody);
    constexpr uint8_t kDroneType = static_cast<uint8_t>(SDK::ECrFoundableType::Drone);
    constexpr uint8_t kUndefinedType = static_cast<uint8_t>(SDK::ECrFoundableType::Undefined);

    constexpr std::ptrdiff_t kMassSubsystemManagerOffset = 0x38;

    constexpr std::size_t kMassQuerySize = 0x350;
    constexpr std::size_t kMassQueryAlignment = 0x10;

    struct FoundableRecord
    {
        uint8_t Type = 0;
        double WorldX = 0.0;
        double WorldY = 0.0;
        PluginTextureHandle Icon = nullptr;
    };

    struct FoundableTypeState
    {
        PluginTextureHandle Icon = nullptr;
        bool Enabled = false;
    };

    bool g_initialized = false;
    bool g_pluginSelfAvailable = false;
    bool g_tickRegistered = false;
    bool g_experienceReady = false;
    float g_pollAccumulator = kPollIntervalSeconds;

    SDK::UCrMapMenuDevSettings* g_devSettings = nullptr;

    SDK::UWorld* g_queryWorld = nullptr;
    SDK::UMassEntitySubsystem* g_massSubsystem = nullptr;
    const void* g_massManager = nullptr;

    alignas(kMassQueryAlignment)
        std::array<std::byte, kMassQuerySize> g_queryStorage = {};

    bool g_queryConstructed = false;

    std::mutex g_snapshotMutex;
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
    std::vector<ContainerDotRecord> g_containerDots;
    // AlienX currently has no fence-aware texture retirement.
    std::unordered_map<uint8_t, PluginTextureHandle> g_iconCache;

    SDK::UCrPlayerMapMenuDataComponent* GetLocalMapDataComponent()
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
        auto* self=GetSelf();
        if (texture == nullptr ||
            name == nullptr ||
            !g_pluginSelfAvailable ||
            self == nullptr ||
            self->hooks == nullptr ||
            self->hooks->ImGuiTextures == nullptr)
        {
            return nullptr;
        }

        MiniMapNative::TextureAccess::PlatformData platformData = {};
        if (!MiniMapNative::TextureAccess::QueryPlatformData(
            texture,
            platformData))
        {
            return nullptr;
        }

        MiniMapNative::TextureAccess::MipData mipData = {};
        if (!MiniMapNative::TextureAccess::QueryMipData(
            platformData,
            0,
            mipData))
        {
            return nullptr;
        }

        void* bulkCopy = nullptr;
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
                static_cast<const uint8_t*>(bulkCopy),
                static_cast<int>(mipData.SizeX),
                static_cast<int>(mipData.SizeY),
                static_cast<size_t>(mipData.BulkSize),
                rgba);

        MiniMapNative::TextureAccess::FreeBulkDataCopy(bulkCopy);

        if (!decoded)
        {
            return nullptr;
        }

        auto* textures = self->hooks->ImGuiTextures;

        if (textures->GetFreeSlotCount() <= 0)
        {
            return nullptr;
        }

        PluginTextureHandle handle =
            textures->LoadFromRGBA(
                rgba.data(),
                static_cast<int>(mipData.SizeX),
                static_cast<int>(mipData.SizeY),
                name);

        if (handle == nullptr)
        {
            return nullptr;
        }

        LOG_INFO(
            "MiniMap: cached foundable icon "
            "name=%s size=%ux%u source=CPU-BC3",
            name,
            static_cast<unsigned int>(mipData.SizeX),
            static_cast<unsigned int>(mipData.SizeY));

        return handle;
    }

    PluginTextureHandle ResolveFoundableIcon(
        const SDK::UCrMapMenuCategoryData* category,
        uint8_t foundableType)
    {
        const auto existing = g_iconCache.find(foundableType);
        if (existing != g_iconCache.end())
        {
            return existing->second;
        }

        const MiniMapNative::NativeApi* native = MiniMapNative::Get();

        if (category == nullptr ||
            native == nullptr ||
            native->texture.getBrushTexture == nullptr)
        {
            return nullptr;
        }

        SDK::UTexture2D* texture =
            native->texture.getBrushTexture(category->Icon);

        if (texture == nullptr)
        {
            return nullptr;
        }

        char name[64] = {};
        std::snprintf(
            name,
            sizeof(name),
            "MiniMap_Foundable_%u",
            static_cast<unsigned int>(foundableType));

        PluginTextureHandle handle =
            LoadIconFromBulkData(texture, name);

        if (handle == nullptr)
        {
            return nullptr;
        }

        g_iconCache.emplace(foundableType, handle);
        return handle;
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

        auto* self = GetSelf();
        auto* engine = self != nullptr && self->hooks != nullptr
            ? self->hooks->Engine
            : nullptr;
        if (engine == nullptr ||
            engine->GetStaticFindObjectByNameAddress == nullptr)
        {
            return nullptr;
        }

        const uintptr_t findAddress =
            engine->GetStaticFindObjectByNameAddress();
        if (findAddress == 0)
        {
            return nullptr;
        }

        uintptr_t scriptStructClass = 0;
        if (!MiniMapResidentMetadata::FindResident(
                findAddress,
                0,
                L"/Script/CoreUObject.ScriptStruct",
                false,
                scriptStructClass) ||
            scriptStructClass == 0)
        {
            return nullptr;
        }

        uintptr_t droppedItemsTag = 0;
        if (!MiniMapResidentMetadata::FindResident(
                findAddress,
                scriptStructClass,
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

        const void* manager = nullptr;
        std::memcpy(
            &manager,
            subsystemBytes + kMassSubsystemManagerOffset,
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

        const void* sharedPtrStorage =
            subsystemBytes + kMassSubsystemManagerOffset;

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

        LOG_INFO("MiniMap: Mass foundable query constructed");
        return true;
    }

    bool InventoryHasItems(const void* inventoryFragment)
    {
        if (inventoryFragment == nullptr)
        {
            return false;
        }

        const auto& inventory = static_cast<const SDK::FCrInventoryFragment*>(inventoryFragment)->MainInventoryContainer.Items;
        const int32_t count = inventory.Num();

        if (count <= 0)
        {
            return false;
        }

        for (int32_t index = 0; index < count; ++index)
        {
            const int32_t itemCount = inventory[index].Item.Count;

            if (itemCount > 0)
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
        const auto inspect = [&](const SDK::FCrItemsStorageContainer& container)
        {
            const int32_t count = container.Items.Num();
            for (int32_t index = 0; index < count; ++index)
            {
                const auto& item = container.Items[index].Item;
                if (item.ItemDataBase == nullptr ||
                    std::memcmp(&item.ItemId.Handle, &zeroGuid, sizeof(zeroGuid)) == 0 ||
                    item.Count <= 0)
                {
                    continue;
                }

                if (item.ItemDataBase->UIItemType == SDK::EUIItemType::BlueprintItem)
                {
                    return true;
                }

                hasOrdinaryItem = true;
            }

            return false;
        };

        if (inspect(inventory.InputInventoryContainer) ||
            inspect(inventory.MainInventoryContainer))
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

        const auto& translation = static_cast<const SDK::FTransformFragment*>(transformFragment)->Transform.Translation;
        worldX = translation.X;
        worldY = translation.Y;

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

        if (category->CategoryBlocked)
        {
            return false;
        }

        const uint8_t filter =
            static_cast<uint8_t>(category->LegendFilter);

        if (!native->pointsOfInterest.getMarkerFilterStatus(
            mapData,
            filter))
        {
            return false;
        }

        PluginTextureHandle icon =
            ResolveFoundableIcon(category, foundableType);

        if (icon == nullptr)
        {
            return false;
        }

        state.Icon = icon;
        state.Enabled = true;
        return true;
    }

    void ClearSnapshot()
    {
        std::scoped_lock lock(g_snapshotMutex);
        g_records.clear();
        g_containerDots.clear();
    }

    bool PollFoundables(const MiniMapConfig::FoundableRenderSettings& settings)
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
            ClearSnapshot();
            return false;
        }

        SDK::UCrPlayerMapMenuDataComponent* mapData = GetLocalMapDataComponent();
        if (mapData != nullptr && g_devSettings == nullptr)
            if (auto* self = GetSelf()) g_devSettings = MiniMapNative::FindMapMenuDevSettingsCDO(self, false);

        FoundableTypeState deadBody = {};
        FoundableTypeState drone = {};
        BuildTypeState(mapData, kDeadBodyType, deadBody);
        BuildTypeState(mapData, kDroneType, drone);

        auto* transformStruct = native->mass.transformFragmentStaticStruct();
        auto* inventoryStruct = native->mass.inventoryFragmentStaticStruct();
        auto* parametersStruct = native->mass.foundableParametersStaticStruct();
        if (transformStruct == nullptr || inventoryStruct == nullptr || parametersStruct == nullptr)
        {
            ClearSnapshot();
            return false;
        }

        MiniMapNative::MassEntityHandleArray handles = {};
        native->mass.getMatchingEntityHandles(g_queryStorage.data(), &handles);

        std::vector<ContainerDotRecord> nextContainerDots;
        std::vector<FoundableRecord> nextRecords;
        nextRecords.reserve(static_cast<std::size_t>(handles.Num));
        for (int32_t index = 0; index < handles.Num; ++index)
        {
            const auto id = handles.Data[index];
            const auto* parameters = ResolveFoundableParametersData(
                native->mass.getConstSharedFragmentPtr(g_massManager, id, parametersStruct),
                parametersStruct);
            if (parameters == nullptr) continue;

            const auto foundableType = static_cast<uint8_t>(parameters->FoundableType);
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

            if (!typeState->Enabled || typeState->Icon == nullptr ||
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

        if (handles.Data != nullptr)
            native->texture.memoryFree(handles.Data);

        {
            std::scoped_lock lock(g_snapshotMutex);
            g_records = std::move(nextRecords);
            g_containerDots = std::move(nextContainerDots);
        }

        return true;
    }
    void OnTick(float deltaSeconds)
    {
        MiniMapCallbackTiming::Scope callbackTiming(MiniMapCallbackTiming::Id::FoundablesTick,true);
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

        if (g_pollAccumulator < kPollIntervalSeconds)
        {
            return;
        }

        if (PollFoundables(settings))
        {
            g_pollAccumulator = 0.0f;
        }
        else
        {
            g_pollAccumulator = kPollIntervalSeconds;
        }
    }
}

namespace MiniMapFoundables
{
    bool Initialize(IPluginSelf* self)
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

        self->hooks->Engine->RegisterOnTick(&OnTick);

        g_tickRegistered = true;
        g_initialized = true;

        LOG_INFO("MiniMap: Mass foundable subsystem initialized");
        return true;
    }

    void Reset()
    {
        {
            std::scoped_lock lock(g_snapshotMutex);
            g_records.clear();
            g_containerDots.clear();
        }

        DestroyQuery();

        g_devSettings = nullptr;
        g_experienceReady = false;
        g_pollAccumulator = kPollIntervalSeconds;
    }

    void OnExperienceLoadComplete()
    {
        if (!g_initialized)
        {
            return;
        }

        g_experienceReady = true;
        g_pollAccumulator = kPollIntervalSeconds;

        LOG_INFO("MiniMap: Mass foundable acquisition enabled");
    }

    void Render(
        IModLoaderImGui* ui,
        const MiniMapMap::Transform& transform,
        const MiniMapConfig::FoundableRenderSettings& settings,
        const MiniMapConfig::FogOfWarSettings& fog)
    {
        if (ui == nullptr ||
            !transform.Valid)
        {
            return;
        }

        std::vector<FoundableRecord> records;
        std::vector<ContainerDotRecord> containerDots;

        {
            std::scoped_lock lock(g_snapshotMutex);
            records = g_records;
            containerDots = g_containerDots;
        }

        PluginDrawList drawList = ui->GetWindowDrawList();

        if (drawList == nullptr)
        {
            return;
        }

        const float viewportX =
            transform.CenterX - transform.Width * 0.5f;

        const float viewportY =
            transform.CenterY - transform.Height * 0.5f;

        const float iconSize =
            MiniMapMap::CalculateMapMarkerIconSize(
                transform.MetersPerPixel, settings.MarkerScale);

        const float halfIcon = iconSize * 0.5f;

        ui->DL_PushClipRect(
            drawList,
            viewportX,
            viewportY,
            viewportX + transform.Width,
            viewportY + transform.Height,
            true);

        // MiniMapUI renders Foundables after terrain and Fog of War, then POIs.
        // Submit the existing icon snapshot first; container dots follow below.
        for (const FoundableRecord& record : records)
        {
            if (record.Icon == nullptr)
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

            ui->DL_AddImage(
                drawList,
                record.Icon,
                point.X - halfIcon,
                point.Y - halfIcon,
                point.X + halfIcon,
                point.Y + halfIcon,
                0.0f,
                0.0f,
                1.0f,
                1.0f,
                0xFFFFFFFFu);
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
                ui->DL_AddCircleFilled(
                    drawList,
                    point.X,
                    point.Y,
                    (isBlueprint ? kBlueprintContainerDotRadiusPixels : kOrdinaryContainerDotRadiusPixels) * settings.MarkerScale,
                    isBlueprint ? kBlueprintContainerDotColor : kOrdinaryContainerDotColor,
                    12);
            }
        };

        if (settings.ShowLootInBases)
        {
            renderContainerDots(ContainerContents::Ordinary);
            renderContainerDots(ContainerContents::Blueprint);
        }

        ui->DL_PopClipRect(drawList);
    }

    void Shutdown()
    {
        if (!g_initialized)
        {
            return;
        }

        auto* self=GetSelf();
        if (g_tickRegistered &&
            self != nullptr &&
            self->hooks != nullptr &&
            self->hooks->Engine != nullptr)
        {
            self->hooks->Engine->UnregisterOnTick(&OnTick);
        }

        g_tickRegistered = false;

        Reset();

        g_pluginSelfAvailable = false;
        g_initialized = false;

        LOG_INFO("MiniMap: Mass foundable subsystem shut down");
    }
}

#endif
