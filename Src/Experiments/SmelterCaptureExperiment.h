#pragma once
#if defined(MINIMAP_STAGE_R2_SMELTER_CAPTURE) && defined(MODLOADER_CLIENT_BUILD)
namespace MiniMapStaticAssembly { struct Recipe; }
namespace MiniMapSmelterCapture { bool ResolvePrerequisites(); void Initialize(); void Shutdown(); }
namespace MiniMapSmelterCapture
{
    // Synchronous game-thread handoff; on success acquire independent native
    // strong references before the representation Context releases its roots.
    bool OfferStaticAssembly(const MiniMapStaticAssembly::Recipe&,void* world);
}
#endif
