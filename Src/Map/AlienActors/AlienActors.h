#pragma once
#include "Services/Viewport.h"

#if defined(MODLOADER_CLIENT_BUILD)
#include "Services/Map/Terrain/TerrainService.h"

#include "plugin.h"
#include "Map/MapTransform.h"

namespace MiniMapConfig { struct FogOfWarSettings; }

namespace MiniMapAlienActors
{
    bool Initialize(IPluginSelf* self);
    void Reset();
    void OnExperienceLoadComplete();

    // Copies viewport geometry only; acquisition remains on the engine tick.
    void SetViewport(const MiniMapMap::Transform& transform);
    void Render(MiniMap::Services::ICanvas& ui, const MiniMapMap::Transform& transform,
        const MiniMapConfig::FogOfWarSettings& fog,
        const Services::Map::Terrain::TerrainService::SelectionCoverage* selection);
    void Shutdown();
}

#endif
