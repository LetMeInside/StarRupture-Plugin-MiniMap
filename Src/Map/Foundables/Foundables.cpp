#if defined(MODLOADER_CLIENT_BUILD)

#include "Foundables.h"
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

#include "SDK/Chimera_classes.hpp"
#include "SDK/ChimeraUI_classes.hpp"
#include "SDK/Engine_classes.hpp"
#include "SDK/AuItems_classes.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    MiniMap::Services::Registration g_tickRegistration{};
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

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        LOG_INFO(
            "MiniMap: cached foundable icon "
            "name=%s size=%ux%u source=CPU-BC3",
            name,
            static_cast<unsigned int>(mipData.SizeX),
            static_cast<unsigned int>(mipData.SizeY));
#endif

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

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        LOG_INFO("MiniMap: Mass foundable query constructed");
#endif
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

        template<class T>
        bool ValidSpan(const T* data, int32_t count, int32_t capacity)
        {
            return count >= 0 && capacity >= count &&
                (count == 0 || (data != nullptr && size_t(count) <= (std::numeric_limits<size_t>::max)() / sizeof(T)));
        }

        template<class T>
        bool ReadArray(const SDK::TArray<T>& array, const T*& data, int32_t& count)
        {
            data = array.GetDataPtr();
            count = array.Num();
            return ValidSpan(data, count, array.Max());
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
                if (!actor || actor->bActorIsBeingDestroyed ||
                    (uint32_t(actor->Flags) & (uint32_t(SDK::EObjectFlags::ClassDefaultObject) |
                        uint32_t(SDK::EObjectFlags::ArchetypeObject))) || !Derives(actor->Class, base)) return false;
                auto* level = actor->Outer;
                return level && Derives(level->Class, Level) &&
                    static_cast<const SDK::ULevel*>(level)->OwningWorld == world;
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
                    world->GameState == GameState && GameState->AbandonBaseInfoReplicator == Replicator;
            }
        };

        bool ReadProgress(const Types& types, SDK::UWorld* world, ProgressMap& progress, ProgressScope& scope)
        {
            if (!world || !types.Derives(world->Class, types.World)) return false;
            scope.World = world;
            auto* baseGameState = world->GameState;
            if (!types.Actor(baseGameState, types.GameState, sizeof(SDK::ACrGameStateBase), world)) return false;
            auto* gameState = static_cast<SDK::ACrGameStateBase*>(baseGameState);
            scope.GameState = gameState;
            auto* replicator = gameState->AbandonBaseInfoReplicator;
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
                if (poi->PointOfInterestType != SDK::ECrPointOfInterestType::AbandonedBase) continue;
                const auto base = CopyGuid(poi->Guid);
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
                if (!ReadArray(poi->AbandonBaseStoryItemsAssigned, assignments, count)) continue;
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
            if (!ReadArray(replicator->AbandonBaseDataContainer.AbandonBaseData, bases, baseCount)) return false;
            std::unordered_set<Guid, GuidHash> replicatedBases;
            std::unordered_set<Guid, GuidHash> ambiguousReplicatedBases;
            for (int32_t index = 0; index < baseCount; ++index)
            {
                const auto base = CopyGuid(bases[index].AbandonBasePOIId);
                if (!ValidGuid(base)) continue;
                if (!replicatedBases.insert(base).second)
                {
                    ambiguousReplicatedBases.insert(base);
                    continue;
                }
                const SDK::FCrAbandonBaseDataCollectedStatus* statuses = nullptr;
                int32_t count = 0;
                if (!ReadArray(bases[index].StoryItemsCollectedStatus, statuses, count)) continue;
                for (int32_t item = 0; item < count; ++item)
                {
                    const auto guid = CopyGuid(statuses[item].Item);
                    if (!ValidGuid(guid)) continue;
                    const auto found = progress.find(guid);
                    if (found == progress.end() || found->second.Base != base) continue;
                    // Read the representation first; malformed bool bytes are UNKNOWN.
                    uint8_t status = 2;
                    std::memcpy(&status, &statuses[item].Status, sizeof(status));
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
            auto* root = actor->RootComponent;
            if (!root || root->Outer != actor ||
                !types.Derives(root->Class, types.Scene)) return false;
            types.Native.player.getComponentLocation(root, &location);
            return types.Actor(actor, family, extent, world) &&
                actor->RootComponent == root && root->Outer == actor &&
                std::isfinite(location.X) && std::isfinite(location.Y) && std::isfinite(location.Z);
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
                    const auto guid = CopyGuid(family.Audio
                        ? static_cast<SDK::ACrAudiologBase*>(actor)->Guid
                        : static_cast<SDK::ACrDatapadBase*>(actor)->Guid);
                    const auto found = progress.find(guid);
                    if (!ValidGuid(guid) || found == progress.end() || found->second.Ambiguous ||
                        found->second.Records != 1 || found->second.Status != 0) continue;
                    SDK::FVector location{};
                    if (!ReadPosition(types, actor, world, type, family.Extent, location)) continue;
                    const auto currentGuid = CopyGuid(family.Audio
                        ? static_cast<SDK::ACrAudiologBase*>(actor)->Guid
                        : static_cast<SDK::ACrDatapadBase*>(actor)->Guid);
                    if (guid != currentGuid) continue;
                    Candidate candidate{{location.X, location.Y, ContainerContents::Ordinary}};
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

        SDK::UCrPlayerMapMenuDataComponent* mapData = GetLocalMapDataComponent();
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

        g_pluginSelfAvailable = true;
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
            if (g_snapshotWorldGeneration != MiniMapMap::GetWorldGeneration()) return;
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

        g_pluginSelfAvailable = false;
        g_initialized = false;

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        LOG_INFO("MiniMap: Mass foundable subsystem shut down");
#endif
    }
}

#endif
