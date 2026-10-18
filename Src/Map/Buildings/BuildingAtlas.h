#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include "BuildingAtlasCore.h"
#include "BuildingAtlasAlienX.h"
#include "BuildingCache.h"
#include "Map/Sprites/SpriteStatus.h"
#include "plugin_interface.h"
#include <memory>

namespace MiniMapBuildingAtlas
{
    struct Session;
    using Batch = std::shared_ptr<Session>;
    enum class AddResult { Retained, CapacityDeferred, Invalid, Unavailable };

    // Borrow the plugin dictionary and v70 API until Shutdown. Lifecycle methods
    // take their own brief LifecycleMutex scopes: do not call under that mutex.
    bool Initialize(MiniMapSpriteStatus::Dictionary&, IPluginImGuiTextures*);
    bool InitializeSession(uint64_t sessionSerial);
    // Game-thread service only. One batch at a time; no caller-held lifecycle lock.
    // EndBatch also services a pending upload retry when no sources were added.
    Batch BeginBatch(uint64_t sessionSerial);
    AddResult Add(const Batch&, const std::string& canonicalBlueprint,
        const MiniMapBuildingCache::Sprite&) noexcept;
    void EndBatch(const Batch&, uint64_t serviceTick) noexcept;
    // Close admission, then wait for the paired EndBatch and all backend calls
    // before release/interface shutdown. The wait releases LifecycleMutex.
    // Lifecycle callers must be outside the batch they are ending (no reentry).
    void EndSession();
    void Shutdown();

    // UI callback only; caller holds LifecycleMutex from lookup through submit.
    // Borrowed metadata/texture: never release or retain the binding separately.
    const MiniMapSpriteStatus::AtlasEntry* Find(const std::string& canonicalBlueprint) noexcept;
    bool SubmitQuad(IModLoaderImGui*, PluginDrawList, const MiniMapSpriteStatus::AtlasEntry&,
        const std::array<MiniMapBuildingAtlasAlienX::Point, 4>& positions, uint32_t color) noexcept;

    // Immutable pixels of the actual GPU-published revision, kept independently
    // of a newer CPU candidate; available without another cache read.
    std::shared_ptr<const MiniMapBuildingAtlasCore::Layout> PublishedImage();
}

#endif
