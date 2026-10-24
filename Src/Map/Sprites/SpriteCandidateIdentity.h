#pragma once
#include "SpriteDiscoveryCore.h"
#include "Map/Buildings/BuildingCollector.h"

namespace MiniMapSpriteCandidates
{
    // One mapping for discovery and capture. Copied metadata only; no native read.
    inline MiniMapSpriteDiscoveryCore::Input Copy(const MiniMapBuildingCollector::Record& r)
    {
        namespace D=MiniMapSpriteDiscoveryCore;
        D::Input out;auto& k=out.Identity;k.Generation=r.Generation;k.Building=r.BuildingId;
        k.DefinitionObject=r.DefinitionObjectIndex;k.NameIndex=r.DefinitionIndex;k.NameNumber=r.DefinitionNumber;
        k.Spline=r.HasSpline;out.DefinitionValid=r.DefinitionValid;out.VisualInputsValid=true;
        out.TransformValid=r.TransformValid;out.Scale=r.Scale;
        return out;
    }
}
