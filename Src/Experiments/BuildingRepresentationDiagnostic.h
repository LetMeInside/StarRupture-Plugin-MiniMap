#pragma once
#if defined(MODLOADER_CLIENT_BUILD) && defined(MINIMAP_STAGE_R2_SMELTER_CAPTURE)
namespace MiniMapBuildingRepresentation
{
    bool ResolvePrerequisites();
    void Initialize();
    void Shutdown();
}
#endif
