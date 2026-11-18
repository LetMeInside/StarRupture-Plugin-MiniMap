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
    /** Resolved offsets and runtime strides for inspected game array elements. */
    struct ArrayElementContracts
    {
        int32_t StorageItemValue = -1;
        uint32_t StorageItemSize = 0;
        int32_t AbandonBaseGuid = -1;
        int32_t AbandonBaseStatuses = -1;
        uint32_t AbandonBaseSize = 0;
        int32_t CollectedStatusGuid = -1;
        int32_t CollectedStatusValue = -1;
        uint32_t CollectedStatusSize = 0;
    };

    /** Publishes the storage-item field offset and reflected element size. */
    void SetStorageItemContract(int32_t valueOffset, uint32_t elementSize);
    /** Publishes abandoned-base and collected-status offsets and reflected sizes. */
    void SetStoryArrayContracts(const ArrayElementContracts& contracts);
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
