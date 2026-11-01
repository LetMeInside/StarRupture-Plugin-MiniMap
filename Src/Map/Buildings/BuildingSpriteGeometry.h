#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingVisualGeometry.h"
#include "BuildingSpriteGeometryCore.h"
#include "Map/Buildings/SpriteStatus.h"
namespace MiniMapBuildingSpriteGeometry
{
    namespace C=MiniMapBuildingCollector;
    namespace G=MiniMapBuildingVisualGeometry;
    inline bool BuildWorldQuad(const C::Record& record, const MiniMapSpriteStatus::AtlasEntry& entry,
        std::array<G::XY,4>& corners)
    {
        if (!record.TransformValid) return false;
        for (double value:entry.Region) if (!std::isfinite(value) || value<0 || value>1) return false;
        if (!(entry.Region[2]>entry.Region[0] && entry.Region[3]>entry.Region[1])) return false;
        // Apply the copied transform, including tilt and signed scale. The
        // shared mapping rejects malformed geometry, not building instances.
        std::array<MiniMapBuildingSpriteGeometryCore::Point,4> world;
        if (!MiniMapBuildingSpriteGeometryCore::BuildWorldQuad(record.Position, record.Scale,
            record.Rotation, entry.Min, entry.Max, entry.SourceRect, world)) return false;
        for (size_t i=0;i<world.size();++i) corners[i]={world[i][0],world[i][1]};
        return true;
    }
}
#endif
