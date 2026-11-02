#pragma once
#if defined(MODLOADER_CLIENT_BUILD)

#include "BuildingComplexIndex.h"
#include "BuildingCollector.h"

namespace MiniMapBuildingComplexSourceAcquisition
{
    // Pure CPU preparation from an immutable collector record into reusable spatial sections.
    bool PrepareSplineGeometry(MiniMapBuildingComplexIndex::MassKey key,
        const MiniMapBuildingCollector::Record& record,
        MiniMapBuildingComplexIndex::SourcePayload& source) noexcept;
}
#endif
