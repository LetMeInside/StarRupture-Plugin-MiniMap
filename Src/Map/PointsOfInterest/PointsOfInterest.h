#pragma once
#include "Services/Viewport.h"

#if defined(MODLOADER_CLIENT_BUILD)

#include "Map/MarkerIconSizing.h"

namespace MiniMapMap
{
    struct Transform;
}

struct IPluginSelf;

namespace MiniMapConfig
{
    struct POIRenderSettings;
    struct FogOfWarSettings;
}

namespace MiniMapPointsOfInterest
{
    /** Holds game-member offsets resolved before POI gameplay acquisition starts. */
    struct FieldOffsets
    {
        int32_t WorldGameState = -1;
        int32_t GameStateAbandonBaseInfoReplicator = -1;
        int32_t ReplicatorAbandonBaseDataContainer = -1;
        int32_t AbandonBaseContainerData = -1;
        int32_t MarkerState = -1;
        int32_t MarkerType = -1;
        int32_t MarkerGuid = -1;
        int32_t CategoryIcon = -1;
        int32_t CategoryLegendFilter = -1;
        int32_t CategoryCanBeHiddenByFog = -1;
        int32_t PoiUnknownIcon = -1;
        int32_t PoiAbandonBaseCompletedIcon = -1;
        int32_t ActorRootComponent = -1;
    };

    /** Publishes offsets resolved during OnPluginLoadHooks for POI gameplay reads. */
    void SetFieldOffsets(const FieldOffsets& offsets);
    bool Initialize(IPluginSelf* self);
    void Reset();

    void OnExperienceLoadComplete();

    void Render(
        MiniMap::Services::ICanvas& ui,
        const MiniMapMap::Transform& transform,
        const MiniMapConfig::POIRenderSettings& settings,
        const MiniMapConfig::FogOfWarSettings& fog);

    void Shutdown();
}

#endif
