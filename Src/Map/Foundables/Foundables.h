#pragma once
#include "Services/Viewport.h"

#if defined(MODLOADER_CLIENT_BUILD)

#include "Map/MapTransform.h"
#include <cstdint>

struct IPluginSelf;

namespace MiniMapConfig
{
    struct FoundableRenderSettings;
    struct FogOfWarSettings;
}

namespace MiniMapFoundables
{
    /** Holds resolved member offsets consumed by Foundables acquisition. */
    struct AcquisitionFieldOffsets
    {
        int32_t InventoryInputContainer = -1;
        int32_t InventoryMainContainer = -1;
        int32_t StorageContainerItems = -1;
        int32_t SimpleItemId = -1;
        int32_t SimpleItemDataBase = -1;
        int32_t SimpleItemCount = -1;
        int32_t ItemIdHandle = -1;
        int32_t ItemDataBaseUIItemType = -1;
        int32_t FoundableType = -1;
        int32_t TransformFragmentTransform = -1;
        int32_t TransformTranslation = -1;
        int32_t VectorX = -1;
        int32_t VectorY = -1;
        int32_t VectorZ = -1;
        int32_t CategoryIcon = -1;
        int32_t CategoryBlocked = -1;
        int32_t CategoryLegendFilter = -1;
        int32_t WorldGameState = -1;
        int32_t GameStateAbandonBaseInfoReplicator = -1;
        int32_t ReplicatorAbandonBaseDataContainer = -1;
        int32_t AbandonBaseContainerData = -1;
        int32_t PoiMarkerType = -1;
        int32_t PoiMarkerGuid = -1;
        int32_t PoiStoryItemsAssigned = -1;
        int32_t DatapadGuid = -1;
        int32_t AudiologGuid = -1;
        int32_t ActorRootComponent = -1;
        int32_t ObjectFlags = -1;
        int32_t ObjectOuter = -1;
        int32_t LevelOwningWorld = -1;
        int32_t ActorBeingDestroyed = -1;
        uint8_t ActorBeingDestroyedMask = 0;
    };

    /** Resolved offsets and runtime strides for inspected game array elements. */
    struct ArrayElementContracts
    {
        int32_t StorageItemValue = -1;
        uint32_t StorageItemSize = 0;
        int32_t AbandonBaseGuid = -1;
        uint32_t AbandonBaseGuidSize = 0;
        int32_t AbandonBaseStatuses = -1;
        uint32_t AbandonBaseSize = 0;
        int32_t CollectedStatusGuid = -1;
        int32_t CollectedStatusValue = -1;
        uint32_t CollectedStatusSize = 0;
    };

    /** Publishes the storage-item field offset and reflected element size. */
    void SetStorageItemContract(int32_t valueOffset, uint32_t elementSize);
    /** Publishes offsets resolved before inventory and Mass gameplay acquisition. */
    void SetAcquisitionFieldOffsets(const AcquisitionFieldOffsets& offsets);
    /** Publishes abandoned-base and collected-status offsets and reflected sizes. */
    void SetStoryArrayContracts(const ArrayElementContracts& contracts);
    /** Returns the resolved story-array offsets and runtime element sizes. */
    ArrayElementContracts GetStoryArrayContracts();
    bool Initialize(IPluginSelf* self);
    void Reset();
    void OnExperienceLoadComplete();

    void Render(
        MiniMap::Services::ICanvas& ui,
        const MiniMapMap::Transform& transform,
        const MiniMapConfig::FoundableRenderSettings& settings,
        const MiniMapConfig::FogOfWarSettings& fog);

    void Shutdown();
}

#endif
