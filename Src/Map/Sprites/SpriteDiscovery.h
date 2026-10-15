#pragma once
#if defined(MODLOADER_CLIENT_BUILD) && defined(MINIMAP_STAGE_R26A_SPRITE_DISCOVERY)
namespace MiniMapSpriteDiscovery { void Initialize();void Shutdown(); }
#if defined(MINIMAP_STAGE_R26B_SPRITE_VALIDATION) && defined(MINIMAP_BUILDING_SPRITES)
#include "CaptureDispatchCore.h"
namespace MiniMapSpriteDiscovery
{
    // Scheduling-thread preflight only; never acquires admission or invokes capture.
    MiniMapCaptureDispatch::Readiness QueryDispatchReadiness(const MiniMapSpriteDiscoveryCore::Key&,MiniMapCaptureWork::Revision) noexcept;
}
#endif
#endif
