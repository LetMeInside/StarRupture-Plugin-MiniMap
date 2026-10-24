#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include <algorithm>
#include <array>
#include <cmath>
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
    // User-sampled representative sRGB values, not authenticated shader output.
    // Packed as 0xRRGGBB; application to the ImGui vertex color happens below.
    inline constexpr uint32_t kNeutralWhiteRgb =0xFFFFFF;
    inline constexpr uint32_t kWalkwayRgb = 0x1F1E1C;
    inline constexpr uint32_t kWalkwayInsetRgb = 0x080808;
    inline constexpr double kWalkwayInsetFractionPerSide = 0.05;
    inline constexpr uint32_t kZipRailRgb = 0x80EBDD;
    inline constexpr uint32_t kDroneRailRgb = 0x000000;
    inline bool IsDroneRailDefinition(bool definitionValid,uint32_t buildingId)
    {
        if(!definitionValid)return false;
        switch(static_cast<SDK::ECrBuildingID>(buildingId))
        {
        case SDK::ECrBuildingID::DroneRailT1:
        case SDK::ECrBuildingID::DroneRailT2:
        case SDK::ECrBuildingID::DroneRailT3:
        case SDK::ECrBuildingID::DroneRailT4:
        case SDK::ECrBuildingID::DroneRailT5:return true;
        default:return false;
        }
    }

    inline bool IsWalkwayDefinition(bool definitionValid,uint32_t buildingId)
    {
        return definitionValid && static_cast<SDK::ECrBuildingID>(buildingId)==SDK::ECrBuildingID::Walkway;
    }

    // Resolve representative color from the exact validated definition ID.
    // Broad Rails/Ziplines categories are intentionally insufficient: other
    // rail and zipline definitions retain the neutral-white rendering.
    inline uint32_t RepresentativeRgb(bool definitionValid,uint32_t buildingId)
    {
        if(!definitionValid)return kNeutralWhiteRgb;
        if(IsDroneRailDefinition(definitionValid,buildingId))return kDroneRailRgb;
        switch(static_cast<SDK::ECrBuildingID>(buildingId))
        {
        // The existing Walkways category identifies these placement families.
        case SDK::ECrBuildingID::Walkway:
        case SDK::ECrBuildingID::RailingWalkway:return kWalkwayRgb;
        case SDK::ECrBuildingID::ZipRail:return kZipRailRgb;
        default:return kNeutralWhiteRgb;
        }
    }

    // ImGui packing: R in the low byte, then G, B, alpha.
    inline uint32_t Color(uint32_t rgb,uint8_t alpha)
    {return ((rgb>>16)&255)|(rgb&0xFF00)|((rgb&255)<<16)|(uint32_t(alpha)<<24);}
}
#endif
