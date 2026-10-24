#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingGeometryTypes.h"
#include <cstdint>
#include <memory>
#include <vector>
namespace MiniMapBuildingSplineProfiles
{
    using Point=MiniMapBuildingGeometryTypes::Point;
    struct Bounds {Point Origin{},Extent{};double SphereRadius=0;};
    inline bool Valid(const Bounds& b)
    {
        if(!MiniMapBuildingGeometryTypes::Finite(b.Origin) || !MiniMapBuildingGeometryTypes::Finite(b.Extent) ||
            !std::isfinite(b.SphereRadius) || b.SphereRadius<0)return false;
        for(auto e:b.Extent)if(e<0)return false;
        return true;
    }
    struct RelativeTransform {Point Translation{},RotationDegrees{},Scale{1,1,1};};
    struct CrossSection
    {
        std::array<double,2> StartScale{1,1},EndScale{1,1},StartOffset{},EndOffset{};
        double StartRoll=0,EndRoll=0;Point Up{0,0,1};
    };
    struct Profile
    {
        Bounds LocalBounds;
        RelativeTransform Transform;
        CrossSection Section;
        uint8_t ForwardAxis=0;
    };
    // Alternatives are ordered once during definition preparation. The first
    // numerically drawable alternative wins for each curve/frame.
    struct Set {std::vector<Profile> Profiles;};
}
#endif
