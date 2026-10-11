#pragma once
#include "SpriteDiscoveryCore.h"
#include "../Map/BuildingCollector.h"

namespace MiniMapSpriteCandidates
{
    // One mapping for discovery and capture. Copied metadata only; no native read.
    inline MiniMapSpriteDiscoveryCore::Input Copy(const MiniMapBuildingCollector::Record& r)
    {
        namespace D=MiniMapSpriteDiscoveryCore;
        D::Input out;auto& k=out.Identity;k.Generation=r.Generation;k.Building=r.BuildingId;
        k.DefinitionObject=r.DefinitionObjectIndex;k.NameIndex=r.DefinitionIndex;k.NameNumber=r.DefinitionNumber;
        k.Spline=r.HasSpline;out.DefinitionValid=r.DefinitionValid;D::CopyTint(out,r.Tint);
        if(r.DefinitionShape)
        {
            const auto& s=*r.DefinitionShape;out.GeometryMetadata=true;k.VisualClass=s.VisualClassObjectIndex;
            k.MeshObject=s.SplineMeshObjectIndex;k.MeshName=s.SplineMeshNameIndex;k.MeshNumber=s.SplineMeshNameNumber;
        }
        return out;
    }
}
