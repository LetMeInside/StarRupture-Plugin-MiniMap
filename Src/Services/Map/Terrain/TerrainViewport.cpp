// Implements disconnected multi-image BC1 presentation using existing texture receipts and canvas contracts.
// CPU decoding is temporary; backend resource retirement remains entirely with the supplied ledger.
#include "TerrainViewport.h"
#include "Processing/TerrainBc1Codec.h"
#include <cmath>
#include <exception>
#include <limits>
#include <utility>

namespace Services::Map::Terrain
{
    /** Validates finite pixel endpoints in double precision before converting to canvas coordinates. */
    bool TerrainViewport::View::IsValid() const noexcept
    {
        const double right = static_cast<double>(X) + Width;
        const double bottom = static_cast<double>(Y) + Height;
        const double maximum = (std::numeric_limits<float>::max)();
        return WorldFromViewport.IsValid() && std::isfinite(X) && std::isfinite(Y) &&
            std::isfinite(Width) && std::isfinite(Height) && Width > 0.0f && Height > 0.0f &&
            std::abs(right) <= maximum && std::abs(bottom) <= maximum &&
            static_cast<float>(right) > X && static_cast<float>(bottom) > Y;
    }

    /** Retains only the generic application capability; construction is inert. */
    TerrainViewport::TerrainViewport(MiniMap::Services::ITextures& textures) noexcept : textures_(textures) {}

    /** Fails explicitly on a violated lifetime precondition rather than discarding unresolved debt. */
    TerrainViewport::~TerrainViewport()
    {
        if (nativeOperationInProgress_ || HasReceipts()) std::terminate();
    }

    /** Uses existing service observation identity verbatim. */
    bool TerrainViewport::SameStamp(const TerrainImageDelivery::Stamp& left,
        const TerrainImageDelivery::Stamp& right) noexcept
    {
        return left.WorldId == right.WorldId && left.SessionGeneration == right.SessionGeneration &&
            left.SelectionRevision == right.SelectionRevision && left.RequestGeneration == right.RequestGeneration;
    }

    /** Allows immutable content reuse while treating changed placement or color as a replacement. */
    bool TerrainViewport::SameImage(const TerrainImage& left, const TerrainImage& right) noexcept
    {
        const auto& a = left.GetRegistration();
        const auto& b = right.GetRegistration();
        return left.GetIdentity() == right.GetIdentity() && left.GetWidthPixels() == right.GetWidthPixels() &&
            left.GetHeightPixels() == right.GetHeightPixels() && left.IsSRgb() == right.IsSRgb() &&
            a.Origin.X == b.Origin.X && a.Origin.Y == b.Origin.Y && a.U.X == b.U.X && a.U.Y == b.U.Y &&
            a.V.X == b.V.X && a.V.Y == b.V.Y;
    }

    /** Uses only the existing authoritative selection fields; requests remain independent per slot. */
    bool TerrainViewport::SameSelection(const TerrainImageDelivery::Stamp& left,
        const TerrainImageDelivery::Stamp& right) noexcept
    {
        return left.WorldId == right.WorldId && left.SessionGeneration == right.SessionGeneration &&
            left.SelectionRevision == right.SelectionRevision;
    }

    /** Keeps presentation order independent of upload completion and delivery arrival. */
    bool TerrainViewport::SlotPrecedes(const std::shared_ptr<ImageSlot>& left,
        const std::shared_ptr<ImageSlot>& right) noexcept
    {
        return left->Order != right->Order ? left->Order < right->Order : left->Id < right->Id;
    }

    /** Returns only currently declared demand, without allocating or resurrecting a slot. */
    std::shared_ptr<TerrainViewport::ImageSlot> TerrainViewport::FindSlot(const std::string& slotId) const noexcept
    {
        for (const auto& slot : slots_)
            if (slot->Id == slotId) return slot;
        return {};
    }

    /** Geometry updates never invalidate same-selection images or initiate uploads. */
    bool TerrainViewport::SetDemand(const TerrainImageDelivery::Stamp& selection, const View& view)
    {
        if (selection.WorldId.empty() || !selection.SessionGeneration || !selection.SelectionRevision ||
            !view.IsValid()) return false;
        auto nextSelection = selection;
        nextSelection.RequestGeneration = 0;
        if (!demand_ || !SameSelection(*demand_, selection)) Withdraw();
        demand_ = std::move(nextSelection);
        view_ = view;
        return true;
    }

    /** Updates one exact request while preserving unrelated slots and same-region completed imagery. */
    bool TerrainViewport::DeclareSlot(std::string slotId, const TerrainImageDelivery::Stamp& context,
        std::string regionId, std::int64_t order)
    {
        if (slotId.empty() || !demand_ || !SameSelection(*demand_, context) ||
            !context.RequestGeneration) return false;
        auto nextContext = context;
        auto slot = FindSlot(slotId);
        const bool orderChanged = !slot || slot->Order != order;
        if (!slot)
        {
            slot = std::make_shared<ImageSlot>();
            slot->Id = std::move(slotId);
            slots_.push_back(slot);
        }
        if (!SameStamp(slot->Context, context) || slot->RegionId != regionId)
        {
            if (slot->RegionId != regionId) RetirePresentation(*slot);
            slot->DesiredImage.reset();
            slot->UploadAttempted = false;
        }
        slot->Context = std::move(nextContext);
        slot->RegionId = std::move(regionId);
        slot->Order = order;
        if (orderChanged) slots_.sort(SlotPrecedes);
        return true;
    }

    /** Rejects stale snapshots before interpreting availability or retaining an image. */
    TerrainViewport::DeliveryResult TerrainViewport::AcceptDelivery(
        const std::string& slotId, const TerrainImageDelivery& delivery)
    {
        const auto slot = FindSlot(slotId);
        if (!slot || !SameStamp(slot->Context, delivery.Context) || delivery.RegionId != slot->RegionId)
            return DeliveryResult::Stale;
        switch (delivery.State)
        {
        case TerrainImageDelivery::Availability::Pending:
            return DeliveryResult::Pending;
        case TerrainImageDelivery::Availability::Failed:
            return DeliveryResult::Failed;
        case TerrainImageDelivery::Availability::Unavailable:
            WithdrawSlot(slotId);
            return DeliveryResult::Withdrawn;
        case TerrainImageDelivery::Availability::Ready:
            if (!delivery.HasReadyImage()) return DeliveryResult::Invalid;
            if (!slot->DesiredImage || !SameImage(*slot->DesiredImage, *delivery.Image)) slot->UploadAttempted = false;
            slot->DesiredImage = delivery.Image;
            return DeliveryResult::Accepted;
        }
        return DeliveryResult::Invalid;
    }

    /** Creates a stable receipt slot before native entry and publishes only a validated matching extent. */
    TerrainViewport::UploadResult TerrainViewport::Upload(const std::string& slotId)
    {
        if (nativeOperationInProgress_) return UploadResult::Busy;
        const auto slot = FindSlot(slotId);
        if (!slot || !slot->DesiredImage) return UploadResult::NoImage;
        if (slot->Current && slot->Current->Owner.IsValidated() && SameImage(*slot->Current->Image, *slot->DesiredImage))
            return UploadResult::Unchanged;
        if (slot->UploadAttempted) return UploadResult::AlreadyAttempted;
        if (!textures_.Available()) return UploadResult::Unavailable;
        const auto context = slot->Context;
        const auto regionId = slot->RegionId;
        std::vector<std::uint8_t> pixels;
        const auto& image = *slot->DesiredImage;
        if (!Processing::TerrainBc1Codec::Decode(image.GetCompressedBytes(), image.GetWidthPixels(),
            image.GetHeightPixels(), pixels, true))
        {
            slot->UploadAttempted = true;
            return UploadResult::Rejected;
        }
        presentations_.emplace_back();
        auto& candidate = presentations_.back();
        candidate.Image = slot->DesiredImage;
        slot->UploadAttempted = true;
        const MiniMap::Services::TextureSize size{image.GetWidthPixels(), image.GetHeightPixels()};
        nativeOperationInProgress_ = true;
        const auto result = textures_.UploadRGBA(pixels, size, {{}, size}, candidate.Owner);
        nativeOperationInProgress_ = false;
        return FinishUpload(candidate, result, slot, context, regionId);
    }

    /** Native completion alone does not authorize publication after a reentrant withdrawal/replacement. */
    TerrainViewport::UploadResult TerrainViewport::FinishUpload(Presentation& candidate,
        MiniMap::Services::UploadState result, const std::shared_ptr<ImageSlot>& slot,
        const TerrainImageDelivery::Stamp& context,
        const std::string& regionId) noexcept
    {
        const auto actualSize = candidate.Owner.Size();
        const bool validated = result == MiniMap::Services::UploadState::Complete &&
            candidate.Owner.IsValidated() && actualSize &&
            actualSize->width == candidate.Image->GetWidthPixels() &&
            actualSize->height == candidate.Image->GetHeightPixels();
        const bool current = FindSlot(slot->Id) == slot && SameStamp(slot->Context, context) &&
            slot->RegionId == regionId &&
            slot->DesiredImage && SameImage(*slot->DesiredImage, *candidate.Image);
        if (validated && current)
        {
            RetirePresentation(*slot);
            slot->Current = &candidate;
            return UploadResult::Published;
        }
        candidate.Retired = true;
        candidate.Uncertain = !validated && candidate.Owner.HasRecord();
        if (candidate.Uncertain) return UploadResult::Retained;
        if (candidate.Owner.HasRecord()) return UploadResult::Superseded;
        presentations_.pop_back();
        return result == MiniMap::Services::UploadState::Unavailable ?
            UploadResult::Unavailable : UploadResult::Rejected;
    }

    /** Removes publication immediately while retaining receipt ownership through CPU draw grace. */
    void TerrainViewport::RetirePresentation(ImageSlot& slot) noexcept
    {
        if (!slot.Current) return;
        slot.Current->Retired = true;
        slot.Current->RetireAfterRenderFrame = completedRenderFrames_.load(std::memory_order_relaxed) + 3;
        slot.Current = nullptr;
    }

    /** Composes image registration with inverse viewport registration, preserving all affine corners. */
    bool TerrainViewport::BuildQuad(const TerrainImage& image, MiniMap::Services::Quad& positions) const noexcept
    {
        if (!view_.IsValid()) return false;
        const auto& registration = image.GetRegistration();
        constexpr std::array<TerrainImage::Point, 4> corners{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
        const double maximum = (std::numeric_limits<float>::max)();
        for (std::size_t index = 0; index < corners.size(); ++index)
        {
            const auto world = registration.ImageToWorld(corners[index].X, corners[index].Y);
            const auto local = view_.WorldFromViewport.WorldToImage(world);
            if (!local) return false;
            const double x = view_.X + local->X * view_.Width;
            const double y = view_.Y + local->Y * view_.Height;
            if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > maximum || std::abs(y) > maximum)
                return false;
            positions[index] = {static_cast<float>(x), static_cast<float>(y)};
        }
        return true;
    }

    /** Draws each independently registered image under one shared, exception-balanced canvas clip. */
    bool TerrainViewport::Draw(MiniMap::Services::ICanvas& canvas) const
    {
        if (nativeOperationInProgress_ || !demand_ || slots_.empty()) return false;
        bool drawn = false;
        const auto layer = MiniMap::Services::DrawLayer::Window;
        const MiniMap::Services::Quad uv{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
        canvas.PushClip(layer, {view_.X, view_.Y}, {view_.X + view_.Width, view_.Y + view_.Height}, true);
        try
        {
            for (const auto& slot : slots_)
            {
                if (!slot->Current || !slot->Current->Owner.IsValidated()) continue;
                MiniMap::Services::Quad positions{};
                if (!BuildQuad(*slot->Current->Image, positions)) continue;
                canvas.ImageQuad(layer, slot->Current->Owner.View(), positions, uv, 0xFFFFFFFFu);
                drawn = true;
            }
        }
        catch (...)
        {
            canvas.PopClip(layer);
            throw;
        }
        canvas.PopClip(layer);
        return drawn;
    }

    /** Counts completed CPU render callbacks only; the texture backend owns actual GPU fences. */
    void TerrainViewport::CompleteRenderFrame() noexcept
    {
        completedRenderFrames_.fetch_add(1, std::memory_order_relaxed);
    }

    /** Removes demand before any later delivery can recreate the withdrawn presentation. */
    void TerrainViewport::WithdrawSlot(const std::string& slotId) noexcept
    {
        for (auto entry = slots_.begin(); entry != slots_.end(); ++entry)
        {
            if ((*entry)->Id != slotId) continue;
            RetirePresentation(**entry);
            slots_.erase(entry);
            return;
        }
    }

    /** Invalidates every slot while retaining the single existing receipt ledger for draining. */
    void TerrainViewport::Withdraw() noexcept
    {
        demand_.reset();
        for (const auto& slot : slots_) RetirePresentation(*slot);
        slots_.clear();
    }

    /** Retains unavailable receipts; blocks replay of uncertain ownership and erases only returned debt. */
    bool TerrainViewport::DrainRetirements(bool drawsDetached) noexcept
    {
        if (nativeOperationInProgress_) return false;
        nativeOperationInProgress_ = true;
        for (auto entry = presentations_.begin(); entry != presentations_.end();)
        {
            if (!entry->Retired || entry->Uncertain || (!drawsDetached &&
                completedRenderFrames_.load(std::memory_order_relaxed) < entry->RetireAfterRenderFrame))
            {
                ++entry;
                continue;
            }
            const auto result = textures_.Release(entry->Owner);
            if ((result == MiniMap::Services::ReleaseState::Returned ||
                result == MiniMap::Services::ReleaseState::Empty) && !entry->Owner.HasRecord())
                entry = presentations_.erase(entry);
            else
            {
                entry->Uncertain = result == MiniMap::Services::ReleaseState::OwnershipUncertain;
                ++entry;
            }
        }
        nativeOperationInProgress_ = false;
        return !HasReceipts();
    }

    /** Includes unresolved debt even after explicit withdrawal or backend draw-identity invalidation. */
    bool TerrainViewport::HasReceipts() const noexcept
    {
        for (const auto& presentation : presentations_)
            if (presentation.Owner.HasRecord()) return true;
        return false;
    }
}
