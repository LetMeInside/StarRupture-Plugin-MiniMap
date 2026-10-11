#pragma once
#if defined(MINIMAP_STAGE_R2_SMELTER_CAPTURE) && defined(MODLOADER_CLIENT_BUILD)
namespace MiniMapStaticAssembly { struct Recipe; }
namespace MiniMapSpriteCapture { struct Candidate;struct Decision; }
namespace MiniMapBuildingCollector { struct Record; }
namespace MiniMapSmelterCapture { bool ResolvePrerequisites(); void Initialize(); void Shutdown(); }
namespace MiniMapSmelterCapture
{
    bool CanAcceptCandidate(); // game-thread, no native work; one active owner
    const char* CandidateServiceStatus(); // game-thread diagnostics only
    // Synchronous game-thread handoff; on success acquire independent native
    // strong references before the representation Context releases its roots.
    // Candidate-based front door to the existing owner. Borrowed recipe pointers
    // are synchronously retained before this returns; no second capture backend.
    MiniMapSpriteCapture::Decision OfferStaticAssembly(const MiniMapSpriteCapture::Candidate&,
        const MiniMapBuildingCollector::Record&,const MiniMapStaticAssembly::Recipe&,void* world);
}
#endif
