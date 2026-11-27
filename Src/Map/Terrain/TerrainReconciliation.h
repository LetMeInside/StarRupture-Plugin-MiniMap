// Internal terrain demand/publication helpers; all mutation stays on the game thread.
#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "RegisteredTerrainImage.h"
#include "Map/Map.h"
#include "Map/MapTransform.h"
#include "Services/Viewport.h"
#include "SDK/CoreUObject_structs.hpp"
#include "Services/Textures.h"
#include <memory>
#include <vector>

namespace MiniMapTerrainSource
{
    struct SourceTile;
    /** Coordinates the existing resident owner slots and one selected terrain presentation.
     * Helpers share the existing render mutex and retirement policy in Terrain.cpp.
     */
    class TerrainReconciliation
    {
    public:
        /** Stable chunk identity; absence of RegisteredImage preserves the legacy key. */
        struct ChunkIdentity
        {
            int RadiationLevel = 0;
            int GridX = 0;
            int GridY = 0;
            int ChunkX = 0;
            int ChunkY = 0;
            std::optional<RegisteredTerrainImage::Identity> RegisteredImage;
        };
        /** Non-owning draw descriptor. World corners belong to registered images only. */
        struct LoadedChunk
        {
            ChunkIdentity Identity;
            MiniMap::Services::Texture Texture;
            int GlobalChunkX = 0;
            int GlobalChunkY = 0;
            float UVMaxX = 1.0f;
            float UVMaxY = 1.0f;
            std::array<RegisteredTerrainImage::Point, 4> WorldCorners{};
            uint64_t RetireAfterRenderGeneration = 0;
        };
        /** Acquisition and unresolved ownership are never eligible for forced retirement. */
        enum class ResidentState { Acquiring, Ready, Retained };
        /** Authoritative stable receipt slot, retained across reentrant source changes. */
        struct ResidentChunk
        {
            LoadedChunk Descriptor;
            MiniMap::Services::OwnedTexture Owner;
            ResidentState State = ResidentState::Acquiring;
        };
        /** Synchronous acquisition admission evidence; World is an identity-only borrow. */
        struct ReconciliationContext
        {
            SDK::UWorld* World = nullptr;
            MiniMapMap::PresentationSample Sample{};
            uint64_t PresentationRevision = 0;
            uint64_t Sequence = 0;
            uint64_t SourceRevision = 0;
            float ViewportWidth = 0.0f;
            float ViewportHeight = 0.0f;
            double MetersPerPixel = 0.0;
        };

        /** Reconciles demand and retires inactive owners, including empty registered demand. */
        static bool Reconcile(const MiniMapMap::PresentationSample& sample, uint64_t revision);
        /** Continues the existing legacy registered receipt retirement while the native route is active. */
        static void RetireRegisteredChunks();
        /** Immediately withdraws obsolete presentation after an internal selection change. */
        static void WithdrawSelection();
        /** Clears a stale session override, preserving world generation and zoom. */
        static void SynchronizeSource();
        /** Advances existing demand cadence after an admitted presentation sample. */
        static void AdvanceDemand(const MiniMapMap::PresentationSample& sample,
            uint64_t revision, float deltaSeconds);
        /** Draws current loaded chunks using the shared transform and existing image-quad API.
         * Caller holds the terrain render mutex throughout submission.
         */
        static void DrawLoadedChunks(MiniMap::Services::ICanvas& canvas,
            const MiniMapMap::Transform& transform, float x, float y, float width, float height);
    private:
        /** Owned synchronous work snapshot; contains no durable native texture pointer. */
        struct Work
        {
            ReconciliationContext Context;
            SDK::FVector PlayerLocation{};
            std::vector<LoadedChunk> Residents;
            std::vector<LoadedChunk> Next;
            std::optional<RegisteredTerrainImage> Image;
            double PlayerChunkX = 0.0;
            double PlayerChunkY = 0.0;
            std::array<int, 4> Bounds{};
        };
        /** Confirms an acquisition still matches the admitted presentation and source. */
        static bool IsCurrent(const ReconciliationContext& context);
        /** Keeps registered and game-owned resident identities in separate key spaces. */
        static bool SameIdentity(const ChunkIdentity& left, const ChunkIdentity& right);
        /** Finds reusable descriptor identities and their resident texture views. */
        static bool ContainsIdentity(const std::vector<LoadedChunk>& chunks, const ChunkIdentity& identity);
        static MiniMap::Services::Texture FindExistingTexture(
            const std::vector<LoadedChunk>& chunks, const ChunkIdentity& identity);
        /** Acquires one chunk through the existing terrain decoder and upload path. */
        static MiniMap::Services::Texture AcquireChunkTexture(const SourceTile& tile,
            const ChunkIdentity& identity, int globalChunkX, int globalChunkY);
        /** Copies current admission, geometry and resident descriptors under the render mutex. */
        static bool Capture(Work& work, const MiniMapMap::PresentationSample& sample, uint64_t revision);
        /** Checks current admission under the render mutex between native acquisitions. */
        static bool Current(const Work& work);
        /** Derives registered image intersection without requiring the tracker inside coverage. */
        static void AcquireRegistered(Work& work);
        /** Acquires one registered chunk and publishes only descriptor-derived world corners. */
        static void AcquireRegisteredChunk(Work& work, int x, int y);
        /** Commits only current work and advances existing inactive-texture retirement. */
        static bool Publish(Work& work);
        /** Schedules Ready residents against the exact new active set; caller holds render mutex. */
        static void ScheduleRetirement(const std::vector<LoadedChunk>& next);
        /** Releases eligible inactive Ready owners; retains every unresolved receipt. */
        static void RetireInactive();
        /** Attempts one existing explicit release and logs its result without replay. */
        static bool ReleaseResident(ResidentChunk& resident);
        /** Returns published registered corners or unchanged legacy anchor placement. */
        static std::array<RegisteredTerrainImage::Point, 4> WorldCorners(const LoadedChunk& chunk);
        /** Projects and submits one texture using the existing UV winding. */
        static void DrawChunk(MiniMap::Services::ICanvas& canvas,
            const MiniMapMap::Transform& transform, const LoadedChunk& chunk);
    };
}
#endif
