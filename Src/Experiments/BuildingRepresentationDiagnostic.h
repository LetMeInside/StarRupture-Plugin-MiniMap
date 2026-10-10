#pragma once
#include <cstdint>
#include <cstddef>
#if defined(MODLOADER_CLIENT_BUILD) && defined(MINIMAP_STAGE_R2_SMELTER_CAPTURE)
namespace MiniMapBuildingRepresentation
{
    bool ResolvePrerequisites();
    void Initialize();
    void Shutdown();
    bool ValidateCaptureObject(uintptr_t);
    bool ValidateCaptureMembership(uintptr_t capture,const uintptr_t* components,size_t count);
    bool ValidateCaptureMaterialSlots(uintptr_t component,const uintptr_t* materials,size_t count,size_t& failedSlot);
}
#endif
