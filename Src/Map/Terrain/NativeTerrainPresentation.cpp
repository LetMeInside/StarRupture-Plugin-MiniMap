// Connects ordinary native terrain to service acquisition and multi-image viewport presentation.
// Native grid/source decisions stay here; no duplicate image cache, GPU ledger or scheduler is created.
#include "NativeTerrainPresentation.h"
#if defined(MODLOADER_CLIENT_BUILD)
#include "StarRuptureTerrainProvider.h"
#include "StarRuptureTerrainCacheAssociation.h"
#include "TerrainSource.h"
#include "TerrainChunks.h"
#include "TerrainReconciliationState.h"
#include "RegisteredTerrainSource.h"
#include "UI/MiniMapUI.h"
#include "Services/Runtime.h"
#include "Native/NativeApi.h"
#include "Config/Config.h"
#include "SDK/Engine_classes.hpp"
#include "plugin_helpers.h"
#include <cmath>
#include <exception>
#include <limits>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace MiniMapTerrainSource
{
    using namespace TerrainReconciliationState;
    using TerrainService = Services::Map::Terrain::TerrainService;
    using TerrainViewport = Services::Map::Terrain::TerrainViewport;
    using TerrainImageDelivery = Services::Map::Terrain::TerrainImageDelivery;

    TerrainService* NativeTerrainPresentation::boundService_ = nullptr;
    NativeTerrainPresentation* NativeTerrainPresentation::active_ = nullptr;

    /** Keeps one service/cache owner; no second terrain service instance is constructed. */
    void NativeTerrainPresentation::BindService(TerrainService& service) noexcept
    {
        boundService_ = &service;
    }

    /** Uses the game's build identifier, never the SDK compile-time CL or a fabricated fallback. */
    bool NativeTerrainPresentation::PrepareSession()
    {
        auto* textures = MiniMap::Services::TryTextures();
        const auto* native = MiniMapNative::Get();
        if (!boundService_ || !textures || !native || !native->player.isGameThread ||
            !native->player.isGameThread()) return false;
        const auto generation = MiniMapMap::GetWorldGeneration();
        const auto* world = MiniMapMap::GetWorld();
        if (!world) return false;
        const auto worldId = world->GetName();
        const auto contentRevision = SDK::UKismetSystemLibrary::GetBuildVersion().ToString();
        if (world != MiniMapMap::GetWorld() || generation != MiniMapMap::GetWorldGeneration()) return false;
        if (contentRevision.empty())
        {
            LOG_ERROR("MiniMap: runtime game build identity is empty; native terrain session not admitted");
            return false;
        }
        std::lock_guard lock(g_renderMutex);
        if (!active_) active_ = new NativeTerrainPresentation(*boundService_, *textures);
        return active_->StartSession(worldId, generation, contentRevision);
    }

    /** Keeps the original quarter-second selection cadence separate from per-tick phase advancement. */
    void NativeTerrainPresentation::AdvanceTick(const MiniMapMap::PresentationSample& sample,
        std::uint64_t revision, float deltaSeconds)
    {
        if (RegisteredTerrainSource::Snapshot())
        {
            {
                std::lock_guard lock(g_renderMutex);
                if (auto* native = NativeTerrainPresentation::Active()) native->Withdraw();
            }
            NativeTerrainPresentation::ProcessWithdrawals();
            TerrainReconciliation::AdvanceDemand(sample, revision, deltaSeconds);
            return;
        }
        MiniMapMap::Transform transform;
        bool reconcile = false;
        const auto settings = MiniMapConfig::GetRenderSettings();
        {
            std::lock_guard lock(g_renderMutex);
            g_updateAccumulator += deltaSeconds;
            const bool requested = g_reconcileRequested.exchange(false, std::memory_order_acq_rel);
            reconcile = requested || g_updateAccumulator >= kUpdateIntervalSeconds;
            if (reconcile) g_updateAccumulator = 0.0f;
            const double rotation = settings.RotatingMiniMap ?
                MiniMapMap::HeadingUpMapRotationDegrees(sample.Tracking.ControlYawDegrees) : 0.0;
            if (!MiniMapMap::BuildTransform(sample, rotation, g_metersPerPixel, 0, 0,
                g_viewportPixelWidth, g_viewportPixelHeight, transform)) return;
            g_hasViewportAnchor = true;
            g_registeredPresentation = false;
        }
        if (reconcile) TerrainReconciliation::RetireRegisteredChunks();
        if (auto* native = Active()) native->Advance(sample, revision, transform, reconcile);
    }

    /** Hidden/registered ticks still retire native ownership, without requesting or uploading imagery. */
    void NativeTerrainPresentation::ProcessWithdrawals()
    {
        auto* owner = Active();
        if (!owner) return;
        ProgressScope scope(*owner);
        if (!scope.Entered) return;
        owner->ClearRequests();
        owner->viewport_.Withdraw();
        owner->selection_.reset();
        owner->viewport_.DrainRetirements();
        std::lock_guard lock(g_renderMutex);
        owner->Withdraw();
    }

    /** Lifetime changes occur only during application preparation and quiescent final teardown. */
    NativeTerrainPresentation* NativeTerrainPresentation::Active() noexcept
    {
        return active_;
    }

    /** Retention includes the viewport and issuer binding when shutdown cannot return receipt ownership. */
    bool NativeTerrainPresentation::StopActive(bool callbacksDetached)
    {
        if (!active_) return true;
        const bool drained = active_->StopAndDrain(callbacksDetached);
        if (!drained || !callbacksDetached) return false;
        std::lock_guard lock(g_renderMutex);
        delete active_;
        active_ = nullptr;
        return true;
    }

    /** Retains the two existing owners without provider construction or native work. */
    NativeTerrainPresentation::NativeTerrainPresentation(TerrainService& service,
        MiniMap::Services::ITextures& textures) : service_(service), viewport_(textures) {}

    /** Registration follows application world identity; provider coverage selection remains generic. */
    bool NativeTerrainPresentation::StartSession(std::string worldId, std::uint64_t generation,
        std::string contentRevision)
    {
        if (progressing_ || worldId.empty() || !generation || contentRevision.empty()) return false;
        const TerrainService::Session nextSession{std::move(worldId), generation};
        if (nextSession == session_ && registration_.IsRegistered()) return true;
        viewport_.Withdraw();
        ClearRequests();
        if (session_.IsValid()) service_.StopSession(session_);
        selection_.reset();
        registration_ = {};
        cacheMapping_ = {};
        session_ = nextSession;
        auto provider = std::make_shared<StarRuptureTerrainProvider>(
            StarRuptureTerrainProvider::Configuration{session_.WorldId, generation, std::move(contentRevision)});
        if (!service_.BeginSession(session_)) return false;
        registration_ = service_.RegisterProvider(provider);
        if (!registration_.IsRegistered()) return false;
        cacheMapping_ = StarRuptureTerrainCacheAssociation::CreateMapping(std::move(provider));
        withdrawRequested_ = true;
        clearRequestsPending_ = true;
        return true;
    }

    /** No render lock spans provider/cache/native operations; other entry points defer mutation. */
    NativeTerrainPresentation::ProgressScope::ProgressScope(NativeTerrainPresentation& owner) : owner_(owner)
    {
        exceptionsOnEntry_ = std::uncaught_exceptions();
        std::lock_guard lock(g_renderMutex);
        if (owner_.progressing_) return;
        NeedsRequestWithdrawal = owner_.clearRequestsPending_;
        owner_.clearRequestsPending_ = false;
        owner_.progressing_ = true;
        owner_.withdrawRequested_ = false;
        Entered = true;
    }

    /** Deferred withdrawal occurs before any later render can observe native completion. */
    NativeTerrainPresentation::ProgressScope::~ProgressScope()
    {
        if (!Entered) return;
        std::lock_guard lock(g_renderMutex);
        if (std::uncaught_exceptions() > exceptionsOnEntry_ || !MiniMapMap::GameplayReady() ||
            owner_.session_.Generation != MiniMapMap::GetWorldGeneration()) owner_.withdrawRequested_ = true;
        if (owner_.withdrawRequested_)
        {
            owner_.viewport_.Withdraw();
            owner_.selection_.reset();
            owner_.clearRequestsPending_ = true;
        }
        owner_.progressing_ = false;
    }

    /** Visibility/session/source changes invalidate in-flight application work even before CPU cleanup. */
    bool NativeTerrainPresentation::IsCurrent(const MiniMapMap::PresentationSample& sample,
        std::uint64_t revision) const
    {
        std::lock_guard lock(g_renderMutex);
        return !withdrawRequested_ && g_initialized && sample.Valid &&
            revision == g_presentationRevision && sample.WorldGeneration == MiniMapMap::GetWorldGeneration() &&
            sample.WorldGeneration == session_.Generation && MiniMapMap::GameplayReady() &&
            MiniMapUI::IsVisible() && !RegisteredTerrainSource::Snapshot();
    }

    /** CPU demand and GPU receipt lifetimes remain separate on every withdrawal. */
    void NativeTerrainPresentation::ClearRequests()
    {
        while (!requests_.empty())
        {
            auto& chunk = requests_.front();
            service_.WithdrawImageRequest(chunk.Request);
            viewport_.WithdrawSlot(chunk.SlotId);
            requests_.pop_front();
        }
    }

    /** Uses the exact current service selection stamp; request generations are assigned per submission. */
    TerrainImageDelivery::Stamp NativeTerrainPresentation::SelectionStamp() const
    {
        return {selection_->Context.WorldId, selection_->Context.Generation, selection_->Revision, 0};
    }

    /** Physical character sampling never substitutes tracking/camera/drone coordinates. */
    bool NativeTerrainPresentation::UpdateSelection(const MiniMapMap::PresentationSample& sample,
        const MiniMapMap::Transform& transform, bool& reconcile)
    {
        service_.UpdatePlayerPosition({session_,
            {sample.Character.WorldX, sample.Character.WorldY, sample.Character.WorldZ}});
        const auto selected = service_.GetSelection();
        if (!selected || selected->Provider != registration_) return false;
        if (!selection_ || selected->Revision != selection_->Revision ||
            selected->Provider != selection_->Provider || selected->RegionId != selection_->RegionId)
        {
            ClearRequests();
            viewport_.Withdraw();
            reconcile = true;
        }
        selection_ = selected;
        return viewport_.SetDemand(SelectionStamp(), BuildView(transform, 0, 0, transform.Width, transform.Height));
    }

    /** Each admitted terrain tick makes bounded incremental progress independently of building budgets. */
    void NativeTerrainPresentation::Advance(const MiniMapMap::PresentationSample& sample,
        std::uint64_t revision, const MiniMapMap::Transform& transform, bool reconcile)
    {
        ProgressScope scope(*this);
        if (!scope.Entered) return;
        if (scope.NeedsRequestWithdrawal) ClearRequests();
        reconcile = reconcile || scope.NeedsRequestWithdrawal;
        if (!IsCurrent(sample, revision) || !UpdateSelection(sample, transform, reconcile))
        {
            std::lock_guard lock(g_renderMutex);
            Withdraw();
            return;
        }
        if (reconcile) Reconcile(sample, revision, transform);
        if (!IsCurrent(sample, revision)) return;
        viewport_.DrainRetirements();
        if (!IsCurrent(sample, revision)) return;
        try
        {
            service_.AdvanceImageRequests();
        }
        catch (const std::exception& error)
        {
            LOG_WARN("MiniMap: native terrain acquisition interrupted: %s", error.what());
            throw;
        }
        catch (...)
        {
            LOG_WARN("MiniMap: native terrain acquisition interrupted by an unknown exception");
            throw;
        }
        if (!IsCurrent(sample, revision)) return;
        ObserveDeliveries();
        if (IsCurrent(sample, revision)) UploadNextImage();
    }

    /** Matches the previous native circumscribed-radius selection, including its edge epsilon. */
    void NativeTerrainPresentation::Reconcile(const MiniMapMap::PresentationSample& sample,
        std::uint64_t revision, const MiniMapMap::Transform& transform)
    {
        SDK::FVector tracking{};
        tracking.X = sample.Tracking.WorldX;
        tracking.Y = sample.Tracking.WorldY;
        tracking.Z = sample.Tracking.WorldZ;
        SourceTile tile;
        if (!TryResolveSourceTile(tracking, tile) || !IsCurrent(sample, revision)) return;
        const double playerChunkX = static_cast<double>(tile.GridY * MiniMapTerrainChunks::kChunksPerAxis) +
            tile.LocalU * MiniMapTerrainChunks::kChunksPerAxis;
        const double playerChunkY = static_cast<double>(tile.GridX * MiniMapTerrainChunks::kChunksPerAxis) +
            tile.LocalV * MiniMapTerrainChunks::kChunksPerAxis;
        const double halfWidth = transform.Width * transform.MetersPerPixel /
            MiniMapTerrainChunks::kChunkWorldSizeMeters * 0.5;
        const double halfHeight = transform.Height * transform.MetersPerPixel /
            MiniMapTerrainChunks::kChunkWorldSizeMeters * 0.5;
        const double radius = std::sqrt(halfWidth * halfWidth + halfHeight * halfHeight);
        constexpr double epsilon = 1.0e-9;
        const int left = static_cast<int>(std::floor(playerChunkX - radius));
        const int top = static_cast<int>(std::floor(playerChunkY - radius));
        const int right = static_cast<int>(std::floor(playerChunkX + radius - epsilon));
        const int bottom = static_cast<int>(std::floor(playerChunkY + radius - epsilon));
        for (auto& chunk : requests_) chunk.Demanded = false;
        std::int64_t order = 0;
        for (int y = top; y <= bottom; ++y)
            for (int x = left; x <= right; ++x)
            {
                if (!IsCurrent(sample, revision)) return;
                DemandChunk(tracking, playerChunkX, playerChunkY, x, y, order++);
            }
        for (auto entry = requests_.begin(); entry != requests_.end();)
        {
            if (entry->Demanded)
            {
                ++entry;
                continue;
            }
            service_.WithdrawImageRequest(entry->Request);
            viewport_.WithdrawSlot(entry->SlotId);
            entry = requests_.erase(entry);
        }
    }

    /** Floor mapping preserves native grid Y as world X and grid X as world Y. */
    int NativeTerrainPresentation::FloorDiv(int value)
    {
        const int divisor = MiniMapTerrainChunks::kChunksPerAxis;
        int quotient = value / divisor;
        if (value % divisor < 0) --quotient;
        return quotient;
    }

    /** The source-local pixel axes follow horizontal U/X and vertical V/Y without transposition. */
    int NativeTerrainPresentation::LocalChunk(int value)
    {
        const int divisor = MiniMapTerrainChunks::kChunksPerAxis;
        const int remainder = value % divisor;
        return remainder < 0 ? remainder + divisor : remainder;
    }

    /** The existing native center calculation also handles adjacent tiles and negative indices. */
    void NativeTerrainPresentation::DemandChunk(const SDK::FVector& tracking, double playerChunkX,
        double playerChunkY, int globalX, int globalY, std::int64_t order)
    {
        SDK::FVector sample = tracking;
        sample.X += (globalX + 0.5 - playerChunkX) * kChunkWorldUnits;
        sample.Y += (globalY + 0.5 - playerChunkY) * kChunkWorldUnits;
        SourceTile tile;
        if (!TryResolveSourceTile(sample, tile) || !tile.Texture ||
            tile.GridX != FloorDiv(globalY) || tile.GridY != FloorDiv(globalX)) return;
        RequestChunk(tile, sample, LocalChunk(globalX), LocalChunk(globalY), order);
    }

    /** Full source name plus native scalar identity prevents radiation/variant/asset aliasing. */
    void NativeTerrainPresentation::RequestChunk(const SourceTile& tile, const SDK::FVector& sample,
        int chunkX, int chunkY, std::int64_t order)
    {
        const auto asset = tile.Texture->GetFullName();
        const auto slotId = std::to_string(asset.size()) + ":" + asset + "/" +
            std::to_string(tile.GridX) + "/" + std::to_string(tile.GridY) + "/R" +
            std::to_string(tile.RadiationLevel) + "/V" + std::to_string(static_cast<int>(tile.SelectedVariant)) +
            "/" + std::to_string(chunkX) + "/" + std::to_string(chunkY);
        auto entry = requests_.begin();
        for (; entry != requests_.end(); ++entry)
            if (entry->SlotId == slotId) break;
        if (entry == requests_.end())
        {
            requests_.emplace_back();
            entry = std::prev(requests_.end());
            entry->SlotId = slotId;
        }
        entry->Demanded = true;
        auto delivery = entry->Request.GetDelivery();
        if (!delivery.Context.RequestGeneration || delivery.State == TerrainImageDelivery::Availability::Failed)
        {
            TerrainService::ImageRequestParameters parameters;
            parameters.Context = SelectionStamp();
            parameters.Provider = selection_->Provider;
            parameters.RegionId = selection_->RegionId;
            parameters.SamplePosition = {sample.X, sample.Y, sample.Z};
            parameters.PixelX = static_cast<std::uint32_t>(chunkX) * 256u;
            parameters.PixelY = static_cast<std::uint32_t>(chunkY) * 256u;
            parameters.WidthPixels = 256;
            parameters.HeightPixels = 256;
            parameters.MapNativeCacheKey = cacheMapping_;
            SubmitRequest(*entry, std::move(parameters));
            delivery = entry->Request.GetDelivery();
        }
        viewport_.DeclareSlot(entry->SlotId, delivery.Context, delivery.RegionId, order);
    }

    /** New evidence is monotonic even across visibility withdrawals within the same gameplay session. */
    void NativeTerrainPresentation::SubmitRequest(ChunkRequest& chunk,
        TerrainService::ImageRequestParameters parameters)
    {
        if (requestGeneration_ == (std::numeric_limits<std::uint64_t>::max)())
            throw std::overflow_error("Native terrain request generation exhausted.");
        parameters.Context.RequestGeneration = ++requestGeneration_;
        const auto previous = chunk.Request.GetDelivery();
        auto request = previous.Context.RequestGeneration ?
            service_.ReplaceImageRequest(chunk.Request, std::move(parameters)) :
            service_.RequestImage(std::move(parameters));
        if (previous.Context.RequestGeneration &&
            request.GetDelivery().State == TerrainImageDelivery::Availability::Failed)
        {
            LOG_WARN("MiniMap: native terrain replacement admission failed; previous handle retained");
            return;
        }
        chunk.Request = std::move(request);
        chunk.DeliveryAccepted = false;
        chunk.UploadPending = false;
        chunk.FailureReported = false;
    }

    /** Immutable Ready delivery is admitted once; unavailable request owners are explicitly withdrawn. */
    void NativeTerrainPresentation::ObserveDeliveries()
    {
        for (auto entry = requests_.begin(); entry != requests_.end();)
        {
            const auto delivery = entry->Request.GetDelivery();
            if (delivery.State == TerrainImageDelivery::Availability::Unavailable)
            {
                service_.WithdrawImageRequest(entry->Request);
                viewport_.WithdrawSlot(entry->SlotId);
                entry = requests_.erase(entry);
                continue;
            }
            if (delivery.State == TerrainImageDelivery::Availability::Failed && !entry->FailureReported)
            {
                LOG_WARN("MiniMap: native terrain request failed: %s", delivery.Error.c_str());
                entry->FailureReported = true;
            }
            if (!entry->DeliveryAccepted && delivery.State == TerrainImageDelivery::Availability::Ready)
            {
                entry->DeliveryAccepted = viewport_.AcceptDelivery(entry->SlotId, delivery) ==
                    TerrainViewport::DeliveryResult::Accepted;
                entry->UploadPending = entry->DeliveryAccepted;
                if (!delivery.Error.empty()) LOG_WARN("MiniMap: native terrain: %s", delivery.Error.c_str());
            }
            ++entry;
        }
    }

    /** CPU-only selection may scan pending handles, but exactly one decode/upload is attempted. */
    void NativeTerrainPresentation::UploadNextImage()
    {
        for (auto& chunk : requests_)
        {
            if (!chunk.UploadPending) continue;
            const auto result = viewport_.Upload(chunk.SlotId);
            chunk.UploadPending = result == TerrainViewport::UploadResult::Unavailable;
            if (result == TerrainViewport::UploadResult::Retained || result == TerrainViewport::UploadResult::Rejected)
                LOG_WARN("MiniMap: native terrain upload retained/rejected for slot %s", chunk.SlotId.c_str());
            return;
        }
    }

    /** Inverts rotation and the existing independent X/Y scales; creates no alternative camera state. */
    TerrainViewport::View NativeTerrainPresentation::BuildView(const MiniMapMap::Transform& transform,
        float x, float y, float width, float height)
    {
        TerrainViewport::View view;
        if (!transform.Valid || transform.PixelsPerWorldUnit <= 0.0) return view;
        const double scaleX = transform.PixelsPerWorldUnit;
        const double scaleY = transform.PixelsPerWorldUnitY > 0 ? transform.PixelsPerWorldUnitY : scaleX;
        const double localX = (static_cast<double>(x) - transform.CenterX) / scaleX;
        const double localY = (static_cast<double>(y) - transform.CenterY) / scaleY;
        const double cosine = transform.CosMapRotation;
        const double sine = transform.SinMapRotation;
        view.WorldFromViewport = {{transform.TrackingWorldX + cosine * localX + sine * localY,
            transform.TrackingWorldY - sine * localX + cosine * localY},
            {cosine * width / scaleX, -sine * width / scaleX},
            {sine * height / scaleY, cosine * height / scaleY}};
        view.X = x;
        view.Y = y;
        view.Width = width;
        view.Height = height;
        return view;
    }

    /** Only the gate changes during native entry; deferred viewport withdrawal precedes resumed drawing. */
    void NativeTerrainPresentation::Withdraw() noexcept
    {
        withdrawRequested_ = true;
        clearRequestsPending_ = true;
        if (!progressing_)
        {
            viewport_.Withdraw();
            selection_.reset();
        }
    }

    /** Screen geometry can change every render callback without new requests or uploads. */
    void NativeTerrainPresentation::Draw(MiniMap::Services::ICanvas& canvas,
        const MiniMapMap::Transform& transform, float x, float y, float width, float height)
    {
        if (progressing_ || withdrawRequested_ || !selection_) return;
        if (viewport_.SetDemand(SelectionStamp(), BuildView(transform, x, y, width, height))) viewport_.Draw(canvas);
    }

    /** Completion only updates the viewport's atomic CPU callback counter, including during upload. */
    void NativeTerrainPresentation::CompleteRenderFrame() noexcept
    {
        viewport_.CompleteRenderFrame();
    }

    /** Drains before backend closure and retains the entire owner on unresolved receipt debt. */
    bool NativeTerrainPresentation::StopAndDrain(bool callbacksDetached)
    {
        {
            std::lock_guard lock(g_renderMutex);
            Withdraw();
            if (progressing_) return false;
        }
        ProgressScope scope(*this);
        if (!scope.Entered) return false;
        ClearRequests();
        viewport_.Withdraw();
        selection_.reset();
        if (session_.IsValid()) service_.StopSession(session_);
        registration_ = {};
        cacheMapping_ = {};
        session_ = {};
        return viewport_.DrainRetirements(callbacksDetached);
    }
}
#endif
