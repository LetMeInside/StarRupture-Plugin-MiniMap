#pragma once
#if defined(MINIMAP_STAGE_R2_SMELTER_CAPTURE) && defined(MODLOADER_CLIENT_BUILD)
namespace MiniMapSmelterCapture { bool ResolvePrerequisites(); void Initialize(); void Shutdown(); }
#endif
