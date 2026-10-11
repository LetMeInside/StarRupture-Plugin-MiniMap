#pragma once
#include <cstdint>
#include <cstddef>
#if defined(MODLOADER_CLIENT_BUILD) && defined(MINIMAP_STAGE_R2_SMELTER_CAPTURE)
namespace MiniMapSpriteValidation { struct Result; }
namespace MiniMapBuildingCollector { struct Record; }
namespace MiniMapSpriteClassification { struct Observation; }
namespace MiniMapBuildingRepresentation
{
    bool ResolvePrerequisites();
    bool PrepareNativeClasses(); // one retained resident native class per service tick
    void Initialize();
    void Shutdown();
    bool ValidateCaptureObject(uintptr_t);
    // -1 unknown/invalid, 0 hidden, 1 loading. Passive bounded game-thread read.
    int LoadingScreenState(uintptr_t world);
    // Bounded fresh Mass/definition identity and classification read. No graph
    // traversal or borrowed native pointer escapes this synchronous call.
    bool RevalidateCaptureSource(const MiniMapBuildingCollector::Record&,MiniMapSpriteClassification::Observation&);
    bool ValidateCaptureMembership(uintptr_t capture,const uintptr_t* components,size_t count);
    bool ValidateCaptureMaterialSlots(uintptr_t component,const uintptr_t* materials,size_t count,size_t& failedSlot);
#if defined(MINIMAP_STAGE_R26B_SPRITE_VALIDATION)
    // Synchronous game-thread audit. Returned data owns no native references.
    MiniMapSpriteValidation::Result ValidateRepresentation(const MiniMapBuildingCollector::Record&,const MiniMapBuildingCollector::Record*);
    MiniMapSpriteValidation::Result ValidateAndCapture(const MiniMapBuildingCollector::Record&,const MiniMapBuildingCollector::Record*);
#endif
}
#endif
