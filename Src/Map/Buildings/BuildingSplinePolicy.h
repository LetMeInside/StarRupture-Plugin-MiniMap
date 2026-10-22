#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include <cstdint>
#include "SDK/AuActorPlacement_structs.hpp"

namespace MiniMapBuildingSplinePolicy
{
    enum class SplineCategory : uint8_t { Walkways, Rails, Ziplines, Other };
    using Category=SplineCategory;
    // Only the collector's validated placement BuildingID is a category input.
    // A category is not proof of a live actor, component type or renderability.
    inline Category Classify(bool definitionValid,uint32_t buildingId)
    {
        if(!definitionValid)return Category::Other;
        switch(static_cast<SDK::ECrBuildingID>(buildingId))
        {
        case SDK::ECrBuildingID::Walkway:
        case SDK::ECrBuildingID::RailingWalkway:return Category::Walkways;
        case SDK::ECrBuildingID::DroneRailT1:
        case SDK::ECrBuildingID::DroneRailT2:
        case SDK::ECrBuildingID::DroneRailT3:
        case SDK::ECrBuildingID::DroneRailT4:
        case SDK::ECrBuildingID::DroneRailT5:return Category::Rails;
        case SDK::ECrBuildingID::Zipline:
        case SDK::ECrBuildingID::Zipline_250:
        case SDK::ECrBuildingID::Zipline_320:
        case SDK::ECrBuildingID::Zipline_390:
        case SDK::ECrBuildingID::Zipline_460:
        case SDK::ECrBuildingID::Zipline_530:
        case SDK::ECrBuildingID::Zipline_600:
        case SDK::ECrBuildingID::Zipline_2x2:
        case SDK::ECrBuildingID::Zipline_2x2_250:
        case SDK::ECrBuildingID::Zipline_2x2_320:
        case SDK::ECrBuildingID::Zipline_2x2_390:
        case SDK::ECrBuildingID::Zipline_2x2_460:
        case SDK::ECrBuildingID::Zipline_2x2_530:
        case SDK::ECrBuildingID::Zipline_2x2_600:
        case SDK::ECrBuildingID::ZipLine_OLD:
        case SDK::ECrBuildingID::ZiplineSupport:
        case SDK::ECrBuildingID::ZiplineSupport_2x2:
        case SDK::ECrBuildingID::ZipRail:return Category::Ziplines;
        default:return Category::Other;
        }
    }
    // Native access status does not imply complete visual geometry. Missing actor access must not erase the logical Mass record.
    enum class VisualAcquisitionState : uint8_t
    { NotRequested, NativeAccessPending, NativeAccessAvailable, NativeAccessIncomplete };

}
#endif
