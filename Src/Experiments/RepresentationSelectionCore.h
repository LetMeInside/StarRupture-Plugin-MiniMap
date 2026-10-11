#pragma once
#include "RepresentationMetadataCore.h"
#include <cstddef>

namespace MiniMapRepresentationSelection
{
    inline constexpr size_t NativeWorkLimit=32768;
    enum class Route {PlacementActor=0,ConfigurationActor=1,Mass=2,None=3};
    inline Route EligibleRoute(bool placementActor,bool configurationActor,bool mass)
    {return placementActor?Route::PlacementActor:configurationActor?Route::ConfigurationActor:mass?Route::Mass:Route::None;}
    struct WorkBudget
    {
        size_t Remaining=NativeWorkLimit,SelectionVisits=0,RepresentationVisits=0;bool Exhausted=false;
        bool Spend(bool representation)
        {
            if(!Remaining){Exhausted=true;return false;}--Remaining;
            if(representation)++RepresentationVisits;else ++SelectionVisits;return true;
        }
    };
}
