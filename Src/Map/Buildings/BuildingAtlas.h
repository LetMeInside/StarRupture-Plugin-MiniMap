#pragma once
#include "Services/Viewport.h"

#if defined(MODLOADER_CLIENT_BUILD)

#include "BuildingAtlasCore.h"
#include "BuildingCache.h"
#include "Map/Buildings/SpriteStatus.h"
#include "Services/Textures.h"
#include <memory>

namespace MiniMapBuildingAtlas
{
    struct Session;
    using Batch = std::shared_ptr<Session>;
    enum class AddResult { Retained, Invalid, Unavailable };

    // Lifecycle methods take brief LifecycleMutex scopes: do not call under it.
    bool Initialize(MiniMapSpriteStatus::Dictionary&);
    bool InitializeSession(uint64_t sessionSerial);
    // Game-thread service only. One batch at a time; no caller-held lifecycle lock.
    // EndBatch also services a pending upload retry when no sources were added.
    Batch BeginBatch(uint64_t sessionSerial);
    // Retained includes sources awaiting complete-set packing/publication;
    // their processed pixels remain eligible without another cache read/capture.
    AddResult Add(const Batch&, const std::string& canonicalBlueprint,
        const MiniMapBuildingCache::Sprite&) noexcept;
    void EndBatch(const Batch&, uint64_t serviceTick) noexcept;
    // Close admission, then wait for the paired EndBatch and all service calls
    // before release/interface shutdown. The wait releases LifecycleMutex.
    // Lifecycle callers must be outside the batch they are ending (no reentry).
    void EndSession();
    void Shutdown();

    // UI callback only; caller holds LifecycleMutex from lookup through submit.
    // Borrowed metadata/texture: never release or retain the identity separately.
    const MiniMapSpriteStatus::AtlasEntry* Find(const std::string& canonicalBlueprint) noexcept;
    // Diagnostic values only, under the SAME caller-held lifecycle lock as Find.
    void ObservePublication(uint64_t& session, uint64_t& revision) noexcept;
    bool SubmitQuad(MiniMap::Services::ICanvas&, MiniMap::Services::DrawLayer, const MiniMapSpriteStatus::AtlasEntry&,
        const MiniMap::Services::Quad& positions, uint32_t color) noexcept;

    // Immutable pixels of the actual GPU-published revision, kept independently
    // of a newer CPU candidate; available without another cache read.
    std::shared_ptr<const MiniMapBuildingAtlasCore::Layout> PublishedImage();
}

#endif
