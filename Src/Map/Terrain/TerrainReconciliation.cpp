// Preserves registered-image demand, acquisition and resident rendering; ordinary native terrain uses the service route.
#include "TerrainReconciliation.h"
#if defined(MODLOADER_CLIENT_BUILD)
#include "TerrainReconciliationState.h"
#include "NativeTerrainPresentation.h"
#include "TerrainChunkAcquisition.h"
#include "TerrainSource.h"
#include "TerrainChunks.h"
#include "TerrainProfile.h"
#include "RegisteredTerrainSource.h"
#include "Services/Runtime.h"
#include "Services/Textures.h"
#include "Map/MapTransform.h"
#include "UI/MiniMapUI.h"
#include "plugin_helpers.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace MiniMapTerrainSource::TerrainReconciliationState
{
    bool g_initialized = false;
    float g_updateAccumulator = 0.0f;
    std::mutex g_renderMutex;
    std::vector<TerrainReconciliation::LoadedChunk> g_loadedChunks;
    std::vector<std::unique_ptr<TerrainReconciliation::ResidentChunk>> g_residentChunks;
    uint64_t g_renderGeneration = 0;
    float g_viewportPixelWidth = kDefaultViewportPixelWidth;
    float g_viewportPixelHeight = kDefaultViewportPixelHeight;
    double g_metersPerPixel = kDefaultMetersPerPixel;
    std::atomic<bool> g_reconcileRequested = false;
    bool g_hasViewportAnchor = false;
    bool g_registeredPresentation = false;
    SDK::FVector g_viewportAnchorWorldPosition = {};
    double g_viewportAnchorGlobalChunkX = 0.0;
    double g_viewportAnchorGlobalChunkY = 0.0;
    double g_playerGlobalChunkX = 0.0;
    double g_playerGlobalChunkY = 0.0;
    MiniMapMap::PresentationSample g_presentation = {};
    uint64_t g_presentationRevision = 0;
    uint64_t g_reconciliationSequence = 0;
}

namespace MiniMapTerrainSource
{
    using namespace TerrainReconciliationState;
    namespace
    {
        constexpr uint64_t kRetirementRenderGenerations = 3;
    }

    bool TerrainReconciliation::IsCurrent(const ReconciliationContext& context)
    {
        return g_initialized && context.PresentationRevision == g_presentationRevision &&
            context.Sequence == g_reconciliationSequence &&
            context.SourceRevision == RegisteredTerrainSource::Revision() && g_presentation.Valid &&
            context.World != nullptr && context.World == MiniMapMap::GetWorld() &&
            context.Sample.WorldGeneration == MiniMapMap::GetWorldGeneration() &&
            context.Sample.WorldGeneration == g_presentation.WorldGeneration &&
            MiniMapMap::GameplayReady() && MiniMapUI::IsVisible() &&
            context.Sample.TrackingObjectIndex == g_presentation.TrackingObjectIndex &&
            context.Sample.DroneActive == g_presentation.DroneActive &&
            context.Sample.Tracking.WorldX == g_presentation.Tracking.WorldX &&
            context.Sample.Tracking.WorldY == g_presentation.Tracking.WorldY &&
            context.Sample.Tracking.WorldZ == g_presentation.Tracking.WorldZ &&
            context.ViewportWidth == g_viewportPixelWidth &&
            context.ViewportHeight == g_viewportPixelHeight &&
            context.MetersPerPixel == g_metersPerPixel;
    }

    bool TerrainReconciliation::SameIdentity(const ChunkIdentity& left, const ChunkIdentity& right)
    {
        if (left.RegisteredImage || right.RegisteredImage)
            return left.RegisteredImage == right.RegisteredImage && left.ChunkX == right.ChunkX &&
                left.ChunkY == right.ChunkY;
        return left.RadiationLevel == right.RadiationLevel && left.GridX == right.GridX &&
            left.GridY == right.GridY && left.ChunkX == right.ChunkX && left.ChunkY == right.ChunkY;
    }

    bool TerrainReconciliation::ContainsIdentity(const std::vector<LoadedChunk>& chunks,
        const ChunkIdentity& identity)
    {
        for (const auto& chunk : chunks)
            if (SameIdentity(chunk.Identity, identity)) return true;
        return false;
    }

    MiniMap::Services::Texture TerrainReconciliation::FindExistingTexture(
        const std::vector<LoadedChunk>& chunks, const ChunkIdentity& identity)
    {
        for (const auto& chunk : chunks)
            if (chunk.Texture.id != 0 && SameIdentity(chunk.Identity, identity)) return chunk.Texture;
        return {};
    }

    MiniMap::Services::Texture TerrainReconciliation::AcquireChunkTexture(const SourceTile& tile,
        const ChunkIdentity& identity, int globalChunkX, int globalChunkY)
    {
        if (!tile.RegisteredImage) return {};
        ResidentChunk* resident = nullptr;
        {
            std::lock_guard lock(g_renderMutex);
            for (const auto& existing : g_residentChunks)
            {
                if (!SameIdentity(existing->Descriptor.Identity, identity)) continue;
                if (existing->State != ResidentState::Ready) return {};
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                MiniMapTerrainProfile::Count(MiniMapTerrainProfile::Counter::Reuse);
#endif
                return existing->Descriptor.Texture;
            }
            auto prepared = std::make_unique<ResidentChunk>();
            prepared->Descriptor.Identity = identity;
            prepared->Descriptor.GlobalChunkX = globalChunkX;
            prepared->Descriptor.GlobalChunkY = globalChunkY;
            resident = prepared.get();
            g_residentChunks.push_back(std::move(prepared));
        }
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        MiniMapTerrainProfile::Count(MiniMapTerrainProfile::Counter::Attempts);
#endif
        bool ownershipUncertain = false;
        bool loadedFromCache = false;
        MiniMap::Services::Texture texture{};
        try
        {
            texture = TerrainChunkAcquisition::Load(tile, identity.ChunkX, identity.ChunkY,
                resident->Owner, ownershipUncertain, loadedFromCache);
        }
        catch (...)
        {
            std::lock_guard lock(g_renderMutex);
            if (ownershipUncertain || resident->Owner.HasRecord())
            {
                resident->State = ResidentState::Retained;
                resident->Descriptor.Texture = {};
                LOG_ERROR("MiniMap: terrain: acquisition retained without publication "
                    "R%d grid=(%d, %d) chunk=(%d, %d) receipt=%d", identity.RadiationLevel,
                    identity.GridX, identity.GridY, identity.ChunkX, identity.ChunkY,
                    resident->Owner.HasRecord() ? 1 : 0);
                throw;
            }
            const auto found = std::find_if(g_residentChunks.begin(), g_residentChunks.end(),
                [&](const auto& entry) { return entry.get() == resident; });
            g_residentChunks.erase(found);
            throw;
        }
        std::lock_guard lock(g_renderMutex);
        if (ownershipUncertain || (resident->Owner.HasRecord() && texture.id == 0))
        {
            resident->Descriptor.Texture = {};
            resident->State = ResidentState::Retained;
            LOG_ERROR("MiniMap: terrain: acquisition retained without publication R%d grid=(%d, %d) "
                "chunk=(%d, %d) receipt=%d", identity.RadiationLevel, identity.GridX, identity.GridY,
                identity.ChunkX, identity.ChunkY, resident->Owner.HasRecord() ? 1 : 0);
            return {};
        }
        if (texture.id != 0)
        {
            resident->Descriptor.Texture = texture;
            resident->State = ResidentState::Ready;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            MiniMapTerrainProfile::Count(MiniMapTerrainProfile::Counter::New);
#endif
            return texture;
        }
        const auto found = std::find_if(g_residentChunks.begin(), g_residentChunks.end(),
            [&](const auto& entry) { return entry.get() == resident; });
        g_residentChunks.erase(found);
        return {};
    }

    bool TerrainReconciliation::Capture(Work& work, const MiniMapMap::PresentationSample& sample,
        uint64_t revision)
    {
        auto* textures = MiniMap::Services::TryTextures();
        if (!textures || !textures->Available() || !MiniMapMap::HasWorld()) return false;
        std::lock_guard lock(g_renderMutex);
        work.Context.World = MiniMapMap::GetWorld();
        work.Context.Sample = sample;
        work.Context.PresentationRevision = revision;
        work.Context.Sequence = ++g_reconciliationSequence;
        work.Context.SourceRevision = RegisteredTerrainSource::Revision();
        work.Context.ViewportWidth = g_viewportPixelWidth;
        work.Context.ViewportHeight = g_viewportPixelHeight;
        work.Context.MetersPerPixel = g_metersPerPixel;
        if (!sample.Valid || !IsCurrent(work.Context)) return false;
        work.PlayerLocation.X = sample.Tracking.WorldX;
        work.PlayerLocation.Y = sample.Tracking.WorldY;
        work.PlayerLocation.Z = sample.Tracking.WorldZ;
        work.Image = RegisteredTerrainSource::Snapshot();
        work.Residents.reserve(g_residentChunks.size());
        for (const auto& resident : g_residentChunks) work.Residents.push_back(resident->Descriptor);
        return true;
    }

    bool TerrainReconciliation::Current(const Work& work)
    {
        std::lock_guard lock(g_renderMutex);
        return IsCurrent(work.Context);
    }

    bool TerrainReconciliation::Reconcile(const MiniMapMap::PresentationSample& sample, uint64_t revision)
    {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        MiniMapTerrainProfile::Batch profile;
#endif
        Work work;
        if (!Capture(work, sample, revision)) return false;
        if (!work.Image) return false;
        AcquireRegistered(work);
        return Publish(work);
    }

    void TerrainReconciliation::AcquireRegistered(Work& work)
    {
        const double halfWidth = work.Context.ViewportWidth * work.Context.MetersPerPixel * 50.0;
        const double halfHeight = work.Context.ViewportHeight * work.Context.MetersPerPixel * 50.0;
        const double radius = std::hypot(halfWidth, halfHeight);
        const auto bounds = work.Image->VisibleChunks({work.PlayerLocation.X, work.PlayerLocation.Y}, radius);
        for (int y = bounds[1]; y <= bounds[3]; ++y)
            for (int x = bounds[0]; x <= bounds[2]; ++x)
            {
                if (!Current(work)) return;
                AcquireRegisteredChunk(work, x, y);
            }
    }

    void TerrainReconciliation::AcquireRegisteredChunk(Work& work, int x, int y)
    {
        int pixelX = 0;
        int pixelY = 0;
        int validWidth = 0;
        int validHeight = 0;
        if (!work.Image->GetChunkPixelBounds(x, y, pixelX, pixelY, validWidth, validHeight)) return;
        const double centerU = (pixelX + validWidth * 0.5) / work.Image->WidthPixels;
        const double centerV = (pixelY + validHeight * 0.5) / work.Image->HeightPixels;
        const auto center = work.Image->ImageToWorld(centerU, centerV);
        SDK::FVector position{};
        position.X = center.X;
        position.Y = center.Y;
        SourceTile tile;
        if (!RegisteredTerrainSource::Resolve(position, tile) || !Current(work) ||
            !tile.RegisteredImage || tile.RegisteredImage->ResidentIdentity() != work.Image->ResidentIdentity()) return;
        ChunkIdentity identity;
        identity.RegisteredImage = work.Image->ResidentIdentity();
        identity.ChunkX = x;
        identity.ChunkY = y;
        auto texture = FindExistingTexture(work.Residents, identity);
        if (texture.id == 0) texture = AcquireChunkTexture(tile, identity, x, y);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        else MiniMapTerrainProfile::Count(MiniMapTerrainProfile::Counter::Reuse);
#endif
        if (texture.id == 0) return;
        LoadedChunk chunk;
        chunk.Identity = std::move(identity);
        chunk.Texture = texture;
        chunk.WorldCorners = work.Image->ChunkCorners(x, y);
        chunk.UVMaxX = static_cast<float>(validWidth) / 256.0f;
        chunk.UVMaxY = static_cast<float>(validHeight) / 256.0f;
        work.Next.push_back(std::move(chunk));
    }

    void TerrainReconciliation::ScheduleRetirement(const std::vector<LoadedChunk>& next)
    {
        for (const auto& resident : g_residentChunks)
        {
            if (resident->State != ResidentState::Ready) continue;
            auto& chunk = resident->Descriptor;
            if (ContainsIdentity(next, chunk.Identity)) chunk.RetireAfterRenderGeneration = 0;
            else if (chunk.RetireAfterRenderGeneration == 0)
                chunk.RetireAfterRenderGeneration = g_renderGeneration + kRetirementRenderGenerations;
        }
    }

    bool TerrainReconciliation::ReleaseResident(ResidentChunk& resident)
    {
        auto* textures = MiniMap::Services::TryTextures();
        if (!textures) return false;
        resident.State = ResidentState::Retained;
        resident.Descriptor.Texture = {};
        const auto release = textures->Release(resident.Owner);
        const auto& identity = resident.Descriptor.Identity;
        if ((release == MiniMap::Services::ReleaseState::Returned ||
            release == MiniMap::Services::ReleaseState::Empty) && !resident.Owner.HasRecord())
        {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            MiniMapTerrainProfile::Count(MiniMapTerrainProfile::Counter::Retired);
            LOG_INFO("MiniMap: terrain: retired R%d grid=(%d, %d) chunk=(%d, %d) at render generation %llu",
                identity.RadiationLevel, identity.GridX, identity.GridY, identity.ChunkX, identity.ChunkY,
                static_cast<unsigned long long>(g_renderGeneration));
#endif
            return true;
        }
        LOG_ERROR("MiniMap: terrain: release retained R%d grid=(%d, %d) chunk=(%d, %d) state=%u receipt=%d",
            identity.RadiationLevel, identity.GridX, identity.GridY, identity.ChunkX, identity.ChunkY,
            static_cast<unsigned>(release), resident.Owner.HasRecord());
        return false;
    }

    void TerrainReconciliation::RetireInactive()
    {
        auto entry = g_residentChunks.begin();
        while (entry != g_residentChunks.end())
        {
            auto& resident = **entry;
            const auto retirement = resident.Descriptor.RetireAfterRenderGeneration;
            if (resident.State == ResidentState::Ready && retirement != 0 &&
                g_renderGeneration >= retirement && ReleaseResident(resident))
                entry = g_residentChunks.erase(entry);
            else ++entry;
        }
    }

    bool TerrainReconciliation::Publish(Work& work)
    {
        std::lock_guard lock(g_renderMutex);
        if (!IsCurrent(work.Context))
        {
            g_reconcileRequested.store(true, std::memory_order_release);
            return false;
        }
        {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            MiniMapTerrainProfile::Scope profile(MiniMapTerrainProfile::Phase::Retirement);
#endif
            ScheduleRetirement(work.Next);
            RetireInactive();
        }
        g_loadedChunks = std::move(work.Next);
        g_registeredPresentation = work.Image.has_value();
        g_viewportAnchorWorldPosition = work.PlayerLocation;
        g_viewportAnchorGlobalChunkX = work.PlayerChunkX;
        g_viewportAnchorGlobalChunkY = work.PlayerChunkY;
        g_playerGlobalChunkX = work.PlayerChunkX;
        g_playerGlobalChunkY = work.PlayerChunkY;
        g_hasViewportAnchor = true;
        return true;
    }

    /** An exclusive native route has no active legacy registered descriptors, but still owes retirement. */
    void TerrainReconciliation::RetireRegisteredChunks()
    {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        std::optional<MiniMapTerrainProfile::Batch> profile;
#endif
        std::lock_guard lock(g_renderMutex);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        if (!g_residentChunks.empty()) profile.emplace();
#endif
        ScheduleRetirement({});
        RetireInactive();
    }

    void TerrainReconciliation::WithdrawSelection()
    {
        std::lock_guard lock(g_renderMutex);
        g_loadedChunks.clear();
        if (auto* native = NativeTerrainPresentation::Active()) native->Withdraw();
        g_presentation = {};
        ++g_presentationRevision;
        g_hasViewportAnchor = false;
        g_registeredPresentation = false;
        g_reconcileRequested.store(true, std::memory_order_release);
    }

    void TerrainReconciliation::AdvanceDemand(const MiniMapMap::PresentationSample& sample,
        uint64_t revision, float deltaSeconds)
    {
        g_updateAccumulator += deltaSeconds;
        const bool requested = g_reconcileRequested.exchange(false, std::memory_order_acq_rel);
        if (!requested && g_updateAccumulator < kUpdateIntervalSeconds) return;
        g_updateAccumulator = 0.0f;
        Reconcile(sample, revision);
    }

    std::array<RegisteredTerrainImage::Point, 4> TerrainReconciliation::WorldCorners(const LoadedChunk& chunk)
    {
        if (chunk.Identity.RegisteredImage) return chunk.WorldCorners;
        const double x0 = g_viewportAnchorWorldPosition.X +
            (static_cast<double>(chunk.GlobalChunkX) - g_viewportAnchorGlobalChunkX) * kChunkWorldUnits;
        const double y0 = g_viewportAnchorWorldPosition.Y +
            (static_cast<double>(chunk.GlobalChunkY) - g_viewportAnchorGlobalChunkY) * kChunkWorldUnits;
        const double x1 = x0 + kChunkWorldUnits;
        const double y1 = y0 + kChunkWorldUnits;
        return {{{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}};
    }

    void TerrainReconciliation::DrawChunk(MiniMap::Services::ICanvas& canvas,
        const MiniMapMap::Transform& transform, const LoadedChunk& chunk)
    {
        if (chunk.Texture.id == 0) return;
        const auto corners = WorldCorners(chunk);
        std::array<MiniMapMap::ScreenPoint, 4> screen{};
        for (size_t corner = 0; corner < corners.size(); ++corner)
            if (!transform.WorldToScreen(corners[corner].X, corners[corner].Y, screen[corner])) return;
        canvas.ImageQuad(MiniMap::Services::DrawLayer::Window, chunk.Texture,
            {{{screen[0].X, screen[0].Y}, {screen[1].X, screen[1].Y},
                {screen[2].X, screen[2].Y}, {screen[3].X, screen[3].Y}}},
            {{{0.0f, 0.0f}, {chunk.UVMaxX, 0.0f},
                {chunk.UVMaxX, chunk.UVMaxY}, {0.0f, chunk.UVMaxY}}}, 0xFFFFFFFFu);
    }

    void TerrainReconciliation::DrawLoadedChunks(MiniMap::Services::ICanvas& canvas,
        const MiniMapMap::Transform& transform, float x, float y, float width, float height)
    {
        if (g_loadedChunks.empty()) return;
        const auto layer = MiniMap::Services::DrawLayer::Window;
        canvas.PushClip(layer, {x, y}, {x + width, y + height}, true);
        for (const auto& chunk : g_loadedChunks) DrawChunk(canvas, transform, chunk);
        canvas.PopClip(layer);
    }

    void TerrainReconciliation::SynchronizeSource()
    {
        RegisteredTerrainSource::SynchronizeWorld();
    }
}
#endif
