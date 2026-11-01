#if defined(MODLOADER_CLIENT_BUILD)
#include "TexturePublication.h"
#include "MassLifecycle.h"
#include "NativeApi.h"
#include "TextureAccess.h"
#include "CaptureRetirementCore.h"
#include "../Map/Buildings/BuildingComplexWorld.h"
#include <SDK/Engine_classes.hpp>
#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <limits>
#include <set>
#include <utility>
#include <atomic>

namespace
{
    namespace N = MiniMapNative;
    namespace B = MiniMapMassLifecycle;
    namespace T = MiniMapTexturePublication;
    namespace R = MiniMapCaptureRetirement;
    static_assert(sizeof(SDK::FName) == 8);
    static_assert(sizeof(SDK::EPixelFormat) == 1);
    static_assert(uint8_t(SDK::EPixelFormat::PF_FloatRGBA) == 10);
    constexpr uint64_t MiB = 1024 * 1024;
    struct Owner
    {
        uint64_t Id = 0, Bytes = 0;
        T::Generation Token;
        T::State Phase = T::State::Empty;
        SDK::UTexture2D* Texture = nullptr;
        std::shared_ptr<const MiniMapBuildingComplexCellRasterizer::CellImage> Cell;
        MiniMapBuildingComplexCellRasterizer::CellImageIdentity CellIdentity;
        uint64_t RequestId = 0, PublicationRevision = 0;
        std::set<uint64_t> Leases;
        R::FenceOwner Fence;
        bool Uncertain = false, Detached = true, Cancelled = false, Current = true;
        bool Failed = false;
        // After release, Unreal's UObject/resource lifecycle owns retirement;
        // this record remains occupied until the ordered fence and release finish.
    };
    // Pending native fences and retained texture roots may outlive plugin
    // shutdown. Keep their owner storage at process lifetime so module static
    // destruction cannot free fence memory still reachable by the RHI thread.
    std::vector<std::unique_ptr<Owner>>& Owners = *new std::vector<std::unique_ptr<Owner>>();
    std::map<uint64_t, std::shared_ptr<const MiniMapBuildingComplexCellRasterizer::CellImage>> PendingCells;
    std::map<MiniMapBuildingComplexIndex::CellKey, uint64_t> PendingByCell;
    std::map<MiniMapBuildingComplexIndex::CellKey, uint64_t> CurrentByCell;
    std::map<uint64_t, uint64_t> LeaseOwners;
    uint64_t NextId = 1;
    uint64_t NextRequestId = 1, NextLeaseId = 1, NextPublicationRevision = 1;
    bool Closed = true, Initialized = false;
    std::atomic<bool> HasOwners{false};

    bool GameThread() noexcept
    {
        const auto* n = N::Get();
        __try { return n && n->player.isGameThread && n->player.isGameThread(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool NativeAdmission() noexcept
    {
        const auto* n = N::Get();
        __try
        {
            return GameThread() && n->asset.activeGarbageCollection && !*n->asset.activeGarbageCollection &&
                n->asset.exitPurge && !*n->asset.exitPurge &&
                !n->world.isGarbageCollectingAndLocking() && !n->world.isIncrementalPurgePending() &&
                !n->world.isIncrementalReachabilityPending();
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    Owner* Find(uint64_t id)
    {
        if (id) for (auto& owner : Owners) if (owner && owner->Id == id) return owner.get();
        return nullptr;
    }
    Owner* FindRequest(uint64_t requestId)
    {
        if (requestId) for (auto& owner : Owners) if (owner && owner->RequestId == requestId) return owner.get();
        return nullptr;
    }
    bool SameCellIdentity(const MiniMapBuildingComplexCellRasterizer::CellImage& a,
        const MiniMapBuildingComplexCellRasterizer::CellImage& b) noexcept
    {
        return a.Status == MiniMapBuildingComplexCellRasterizer::State::Complete &&
            b.Status == MiniMapBuildingComplexCellRasterizer::State::Complete &&
            a.Key == b.Key && a.WorldGeneration == b.WorldGeneration && a.RestoreEpoch == b.RestoreEpoch &&
            a.SourceRevision == b.SourceRevision && a.CellRevision == b.CellRevision &&
            a.DependencyRevision == b.DependencyRevision && a.RenderDroneRails == b.RenderDroneRails &&
            a.Width == b.Width && a.Height == b.Height && a.CaptureRevisions == b.CaptureRevisions &&
            a.Rgba16F == b.Rgba16F;
    }
    bool SameCellIdentity(const MiniMapBuildingComplexCellRasterizer::CellImageIdentity& a,
        const MiniMapBuildingComplexCellRasterizer::CellImage& b) noexcept
    {
        return b.Status == MiniMapBuildingComplexCellRasterizer::State::Complete &&
            a.Status == MiniMapBuildingComplexCellRasterizer::State::Complete &&
            a.Key == b.Key && a.WorldGeneration == b.WorldGeneration && a.RestoreEpoch == b.RestoreEpoch &&
            a.SourceRevision == b.SourceRevision && a.CellRevision == b.CellRevision &&
            a.DependencyRevision == b.DependencyRevision && a.RenderDroneRails == b.RenderDroneRails &&
            b.Width == MiniMapBuildingComplexIndex::kCellImageSizePixels &&
            b.Height == MiniMapBuildingComplexIndex::kCellImageSizePixels &&
            a.CaptureRevisions == b.CaptureRevisions;
    }
    bool ValidCanonical(int32_t width, int32_t height, const uint8_t* bytes, size_t size) noexcept
    {
        if (width < 1 || height < 1 || !bytes || uint64_t(width) * uint64_t(height) * 8 != size) return false;
        for (size_t i = 0; i < size; i += 2)
        {
            const uint16_t half = uint16_t(bytes[i]) | uint16_t(uint16_t(bytes[i + 1]) << 8);
            if ((half & 0x7c00) == 0x7c00) return false;
            if (i % 8 == 6 && ((half & 0x7fff) > 0x3c00 || ((half & 0x8000) && (half & 0x7fff)))) return false;
        }
        return true;
    }
    bool IsCurrentRequest(const Owner& owner)
    {
        if (!owner.RequestId || !owner.Current || owner.Cancelled) return false;
        const auto found = CurrentByCell.find(owner.CellIdentity.Key);
        return found != CurrentByCell.end() && found->second == owner.Id;
    }
    void Withdraw(Owner& owner)
    {
        owner.Current = false;
        owner.Cancelled = true;
        if (owner.RequestId)
        {
            const auto found = CurrentByCell.find(owner.CellIdentity.Key);
            if (found != CurrentByCell.end() && found->second == owner.Id) CurrentByCell.erase(found);
        }
    }
    bool ValidCellOwner(const Owner& owner) noexcept
    {
        return owner.RequestId && (owner.Cell ?
            MiniMapBuildingComplexWorld::ValidateCellImage(*owner.Cell) :
            MiniMapBuildingComplexWorld::ValidateCellImage(owner.CellIdentity));
    }

    enum class Op { Create, Retain, Update, Resource, Release, ConstructFence, BeginFence, PollFence, DestroyFence };
    // Trivial leaf SEH wrapper. Callers set uncertainty before native mutation
    // and never repeat an interrupted ownership operation.
    bool Call(Op op, void* object, uintptr_t& result, int32_t width = 0, int32_t height = 0) noexcept
    {
        const auto* n = N::Get();
        __try
        {
            switch (op)
            {
            case Op::Create: result = reinterpret_cast<uintptr_t>(n->texture.createTransient(width, height,
                SDK::EPixelFormat::PF_FloatRGBA, SDK::FName{}, N::ConstByteView64{})); break;
            case Op::Retain: n->asset.retainObject(static_cast<SDK::UObject*>(object)); break;
            case Op::Release: n->asset.releaseObject(static_cast<SDK::UObject*>(object)); break;
            case Op::Update: n->texture.updateResource(static_cast<SDK::UTexture*>(object)); break;
            case Op::Resource: result = reinterpret_cast<uintptr_t>(n->texture.getResource(static_cast<SDK::UTexture*>(object))); break;
            case Op::ConstructFence: n->texture.renderFenceConstruct(object); break;
            case Op::BeginFence: n->texture.renderFenceBegin(object, 1); break; // RHIThread, not GPU idle.
            case Op::PollFence: result = n->texture.renderFencePoll(object); break;
            case Op::DestroyFence: n->texture.renderFenceDestroy(object); break;
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    struct FenceOps
    {
        bool Faulted = false;
        bool Available() const { return NativeAdmission(); }
        bool Construct(void* p) { uintptr_t r{}; return !Faulted && (Faulted = !Call(Op::ConstructFence, p, r)) == false; }
        bool Begin(void* p) { uintptr_t r{}; return !Faulted && (Faulted = !Call(Op::BeginFence, p, r)) == false; }
        bool Poll(const void* p)
        { uintptr_t r{}; if (!Call(Op::PollFence, const_cast<void*>(p), r)) Faulted = true; return !Faulted && r; }
        void Destroy(void* p) { uintptr_t r{}; if (!Call(Op::DestroyFence, p, r)) { Faulted = true; throw false; } }
    };
    bool StartFence(Owner& owner)
    {
        FenceOps ops;
        if (!owner.Fence.Rearm() || !owner.Fence.Prepare(ops) || !owner.Fence.Insert(ops, true))
        { owner.Uncertain = true; return false; }
        return true;
    }
    bool CompleteFence(Owner& owner)
    {
        FenceOps ops;
        try
        {
            if (!owner.Fence.Poll(ops)) { if (ops.Faulted) owner.Uncertain = true; return false; }
            if (!owner.Fence.Release(ops)) { owner.Uncertain = true; return false; }
        }
        catch (...) { owner.Uncertain = true; return false; }
        return true;
    }

    // Exclusive transient only; no general writable asset/bulk borrow escapes.
    // Normal rejection is distinct from an interrupted/uncertain native call.
    bool Fill(SDK::UTexture2D* texture, int32_t width, int32_t height,
        const uint8_t* pixels, size_t bytes, bool& uncertain) noexcept
    {
        const auto* n = N::Get();
        void* bulk = nullptr;
        bool locked = false, copied = false;
        __try
        {
            texture->SRGB = false;
            texture->NeverStream = true;
            texture->Filter = SDK::ETextureFilter::TF_Bilinear;
            texture->AddressX = texture->AddressY = SDK::ETextureAddress::TA_Clamp;
            N::TextureAccess::PlatformData platform;
            N::TextureAccess::MipData mip;
            if (!N::TextureAccess::QueryPlatformData(texture, platform) || platform.SRGB ||
                platform.SizeX != width || platform.SizeY != height || platform.PixelFormat != 10 ||
                platform.MipCount != 1 || platform.MipCapacity < 1 || platform.VTData || platform.CPUCopy ||
                !N::TextureAccess::QueryMipData(platform, 0, mip) || mip.SizeX != width ||
                mip.SizeY != height || mip.SizeZ != 1 || mip.BulkSize != int64_t(bytes)) return false;
            bulk = const_cast<void*>(mip.BulkData);
            __try
            {
                void* destination = n->texture.bulkLock(bulk, 2); // LOCK_READ_WRITE
                locked = true;
                if (destination) { std::memcpy(destination, pixels, bytes); copied = true; }
            }
            __finally
            {
                // A completed Lock call receives exactly one Unlock, including
                // null storage/rejected copy. An interrupted call is not retried.
                if (locked) n->texture.bulkUnlock(bulk);
            }
            return copied;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { uncertain = true; return false; }
    }
    void Retiring(Owner& owner);
    bool Upload(Owner& owner, int32_t width, int32_t height,
        const uint8_t* pixels, size_t bytes) noexcept
    {
        uintptr_t texture = 0, ignored = 0;
        owner.Uncertain = true;
        if (!Call(Op::Create, nullptr, texture, width, height))
        { owner.Failed = true; owner.Phase = T::State::Failed; return false; }
        owner.Texture = reinterpret_cast<SDK::UTexture2D*>(texture);
        if (!owner.Texture)
        {
            owner.Uncertain = false; owner.Phase = T::State::Failed; owner.Failed = true;
            return false;
        }
        if (!Call(Op::Retain, owner.Texture, ignored))
        { owner.Failed = true; owner.Phase = T::State::Failed; return false; }
        owner.Uncertain = false;
        if (!Fill(owner.Texture, width, height, pixels, bytes, owner.Uncertain))
        {
            owner.Failed = true;
            owner.Phase = T::State::Failed;
            if (!owner.Uncertain) Retiring(owner);
            return false;
        }
        owner.Uncertain = true;
        if (!Call(Op::Update, owner.Texture, ignored))
        { owner.Failed = true; owner.Phase = T::State::Failed; return false; }
        owner.Uncertain = false;
        owner.Phase = T::State::ResourceInitializing;
        if (!StartFence(owner)) { owner.Phase = T::State::Failed; return false; }
        return true;
    }
    void Retiring(Owner& owner)
    {
        owner.Phase = T::State::Retiring;
        if (!StartFence(owner)) owner.Phase = T::State::Failed;
    }
}

namespace MiniMapTexturePublication
{
    bool Initialize() noexcept
    {
        if (Initialized || !N::Get()) return false;
        Initialized = true; Closed = false;
        return true;
    }
    std::optional<Image> Image::FromCanonical(int32_t width, int32_t height, std::vector<uint8_t>&& bytes)
    {
        if (width > 2048 || height > 2048 || bytes.capacity() > 32 * MiB ||
            !ValidCanonical(width, height, bytes.data(), bytes.size())) return std::nullopt;
        Image result;
        result.Width = width; result.Height = height; result.Pixels = std::move(bytes);
        return std::optional<Image>(std::move(result));
    }
    uint64_t Prepare(Image&& image, Generation generation)
    {
        const auto lifecycle = B::Status();
        if (Closed || !B::InAdmittedService() || !NativeAdmission() || !generation.Request || !generation.Revision ||
            lifecycle.SeedRequired || lifecycle.ObservationLost || lifecycle.Queued || lifecycle.InFlight ||
            (lifecycle.RestoreObserved && (!lifecycle.MassInitialized || !lifecycle.PostSaveCompleted)) ||
            generation.Epoch != lifecycle.Epoch || image.Pixels.empty() || image.Width < 1 || image.Height < 1 ||
            uint64_t(image.Width) * uint64_t(image.Height) * 8 != image.Pixels.size()) return 0;
        if (!NextId || NextId == (std::numeric_limits<uint64_t>::max)()) return 0;
        std::unique_ptr<Owner> candidate;
        try { candidate = std::make_unique<Owner>(); Owners.push_back(std::move(candidate)); }
        catch (...) { return 0; }
        Owner* owner = Owners.back().get();
        owner->Id = NextId++; owner->Token = generation; owner->Bytes = image.Pixels.size();
        owner->Phase = State::Prepared;
        HasOwners.store(true, std::memory_order_release);
        Upload(*owner, image.Width, image.Height, image.Pixels.data(), image.Pixels.size());
        // Engine has its own synchronous BulkData copy. No worker memory is
        // retained by UpdateResource; release the consumed canonical CPU owner.
        image = Image{};
        return owner->Id;
    }
    uint64_t RequestCell(std::shared_ptr<const MiniMapBuildingComplexCellRasterizer::CellImage> image) noexcept
    {
        namespace W = MiniMapBuildingComplexWorld;
        namespace R = MiniMapBuildingComplexCellRasterizer;
        namespace Bc = MiniMapBuildingComplexIndex;
        if (!GameThread() || Closed || !image || image->Status != R::State::Complete ||
            image->Width != Bc::kCellImageSizePixels || image->Height != Bc::kCellImageSizePixels ||
            !ValidCanonical(int32_t(image->Width), int32_t(image->Height),
                image->Rgba16F.data(), image->Rgba16F.size()) ||
            !W::ValidateCellImage(*image)) return 0;
        try
        {
            for (const auto& [id, pending] : PendingCells)
                if (pending && SameCellIdentity(*pending, *image)) return id;
            for (const auto& owner : Owners)
                if (owner && owner->RequestId && owner->Current && !owner->Cancelled &&
                    SameCellIdentity(owner->CellIdentity, *image) && ValidCellOwner(*owner))
                    return owner->RequestId;

            if (!NextRequestId || NextRequestId == (std::numeric_limits<uint64_t>::max)()) return 0;
            const uint64_t requestId = NextRequestId++;
            const auto previousPending = PendingByCell.find(image->Key);
            const uint64_t previousPendingId = previousPending == PendingByCell.end() ? 0 : previousPending->second;
            const auto key = image->Key;
            PendingCells.emplace(requestId, std::move(image));
            try { PendingByCell[key] = requestId; }
            catch (...) { PendingCells.erase(requestId); return 0; }
            if (previousPendingId) PendingCells.erase(previousPendingId);
            const auto current = CurrentByCell.find(key);
            if (current != CurrentByCell.end())
                if (auto* old = Find(current->second)) Withdraw(*old);
            return requestId;
        }
        catch (...) { return 0; }
    }
    RequestStatus InspectRequest(uint64_t requestId) noexcept
    {
        namespace W = MiniMapBuildingComplexWorld;
        if (!requestId || !GameThread()) return {};
        const auto pending = PendingCells.find(requestId);
        if (pending != PendingCells.end())
        {
            const auto& image = pending->second;
            if (image && image->Status == MiniMapBuildingComplexCellRasterizer::State::Complete &&
                W::ValidateCellImage(*image))
            {
                const auto current = PendingByCell.find(image->Key);
                if (current != PendingByCell.end() && current->second == requestId)
                    return {RequestState::Pending, requestId, 0, true};
            }
            return {RequestState::Stale, requestId, 0, true};
        }
        auto* owner = FindRequest(requestId);
        if (!owner) return requestId < NextRequestId ?
            RequestStatus{RequestState::Stale, requestId, 0, true} : RequestStatus{};
        if (owner->Failed || owner->Phase == State::Failed)
            return {RequestState::Failed, requestId, 0, true};
        const bool current = owner->Current && !owner->Cancelled && IsCurrentRequest(*owner) &&
            ValidCellOwner(*owner);
        if (!current) return {RequestState::Stale, requestId, 0, true};
        if (owner->Phase == State::ResourceInitializing)
            return {RequestState::Pending, requestId, 0, true};
        if (owner->Phase == State::Ready && owner->PublicationRevision)
            return {RequestState::Ready, requestId, owner->PublicationRevision, true};
        return {RequestState::Stale, requestId, 0, true};
    }
    void ProcessRequests() noexcept
    {
        namespace W = MiniMapBuildingComplexWorld;
        namespace R = MiniMapBuildingComplexCellRasterizer;
        try
        {
            if (Closed || !B::InAdmittedService() || !NativeAdmission()) return;
            const auto world = W::GetStatus();
            if (world.State != W::MembershipState::MembershipEstablished) return;
            for (auto it = PendingCells.begin(); it != PendingCells.end();)
            {
                const uint64_t requestId = it->first;
                auto image = it->second;
                if (!image || image->Status != R::State::Complete || !W::ValidateCellImage(*image))
                {
                    if (image)
                    {
                        const auto key = image->Key;
                        const auto pending = PendingByCell.find(key);
                        if (pending != PendingByCell.end() && pending->second == requestId) PendingByCell.erase(pending);
                    }
                    it = PendingCells.erase(it);
                    continue;
                }
                auto pending = PendingByCell.find(image->Key);
                if (pending == PendingByCell.end() || pending->second != requestId)
                { it = PendingCells.erase(it); continue; }
                if (!NextId || NextId == (std::numeric_limits<uint64_t>::max)()) return;

                auto identity = R::Identify(*image);
                std::unique_ptr<Owner> candidate;
                try { candidate = std::make_unique<Owner>(); Owners.push_back(std::move(candidate)); }
                catch (...) { return; }
                Owner* owner = Owners.back().get();
                owner->Id = NextId++;
                owner->RequestId = requestId;
                owner->Cell = image; // Immutable exact source identity; no image-byte copy.
                owner->CellIdentity = std::move(identity);
                owner->Bytes = image->Rgba16F.size();
                owner->Phase = State::Prepared;
                try { CurrentByCell[image->Key] = owner->Id; }
                catch (...)
                {
                    owner->Current = false; owner->Cancelled = owner->Failed = true;
                    HasOwners.store(true, std::memory_order_release);
                    return;
                }
                PendingByCell.erase(pending);
                it = PendingCells.erase(it);
                HasOwners.store(true, std::memory_order_release);
                if (!Upload(*owner, int32_t(image->Width), int32_t(image->Height),
                    image->Rgba16F.data(), image->Rgba16F.size()))
                {
                    owner->Current = false;
                    owner->Cancelled = true;
                    const auto current = CurrentByCell.find(image->Key);
                    if (current != CurrentByCell.end() && current->second == owner->Id) CurrentByCell.erase(current);
                }
            }
        }
        catch (...) {}
    }
    bool Acquire(uint64_t requestId, Lease& output) noexcept
    {
        namespace W = MiniMapBuildingComplexWorld;
        if (!GameThread() || !NativeAdmission() || output.LeaseId) return false;
        auto* owner = FindRequest(requestId);
        if (!owner || !owner->RequestId || owner->Phase != State::Ready || owner->Cancelled ||
            !owner->Current || !owner->PublicationRevision || !IsCurrentRequest(*owner) ||
            !W::ValidateCellImage(owner->CellIdentity)) return false;
        uintptr_t resource = 0;
        if (!Call(Op::Resource, owner->Texture, resource))
        {
            owner->Uncertain = owner->Failed = true;
            owner->Phase = State::Failed;
            Withdraw(*owner);
            return false;
        }
        if (!resource)
        {
            owner->Failed = true;
            Withdraw(*owner);
            return false;
        }
        if (!NextLeaseId || NextLeaseId == (std::numeric_limits<uint64_t>::max)()) return false;
        const uint64_t leaseId = NextLeaseId++;
        try
        {
            owner->Leases.insert(leaseId);
            try
            {
                const auto inserted = LeaseOwners.emplace(leaseId, owner->Id);
                if (!inserted.second) { owner->Leases.erase(leaseId); return false; }
            }
            catch (...) { owner->Leases.erase(leaseId); return false; }
        }
        catch (...) { return false; }
        output.LeaseId = leaseId;
        output.RequestId = requestId;
        output.PublicationRevision = owner->PublicationRevision;
        output.Texture = owner->Texture;
        return true;
    }
    bool Release(uint64_t leaseId) noexcept
    {
        if (!leaseId || !GameThread()) return false;
        const auto found = LeaseOwners.find(leaseId);
        if (found == LeaseOwners.end()) return false;
        const uint64_t ownerId = found->second;
        LeaseOwners.erase(found);
        if (auto* owner = Find(ownerId)) owner->Leases.erase(leaseId);
        return true;
    }
    void InvalidateCells(const std::vector<MiniMapBuildingComplexIndex::CellKey>& cells) noexcept
    {
        if (!GameThread()) return;
        for (const auto key : cells)
        {
            const auto pending = PendingByCell.find(key);
            if (pending != PendingByCell.end())
            {
                PendingCells.erase(pending->second);
                PendingByCell.erase(pending);
            }
            const auto current = CurrentByCell.find(key);
            if (current != CurrentByCell.end())
            {
                if (auto* owner = Find(current->second)) Withdraw(*owner);
                CurrentByCell.erase(current);
            }
        }
    }
    void InvalidateCapture(MiniMapBuildingComplexIndex::MassKey building) noexcept
    {
        if (!GameThread()) return;
        for (auto it = PendingCells.begin(); it != PendingCells.end();)
        {
            const auto& image = it->second;
            if (image && std::any_of(image->CaptureRevisions.begin(), image->CaptureRevisions.end(),
                [&](const auto& capture) { return capture.Building == building; }))
            {
                const auto pending = PendingByCell.find(image->Key);
                if (pending != PendingByCell.end() && pending->second == it->first) PendingByCell.erase(pending);
                it = PendingCells.erase(it);
            }
            else ++it;
        }
        for (auto& owner : Owners)
            if (owner && owner->RequestId && std::any_of(owner->CellIdentity.CaptureRevisions.begin(),
                owner->CellIdentity.CaptureRevisions.end(),
                [&](const auto& capture) { return capture.Building == building; }))
                Withdraw(*owner);
    }
    void InvalidateAll() noexcept
    {
        if (!GameThread()) return;
        PendingCells.clear(); PendingByCell.clear(); CurrentByCell.clear();
        for (auto& owner : Owners) if (owner && owner->RequestId) Withdraw(*owner);
    }
    SDK::UTexture2D* Ready(uint64_t id, Generation current)
    {
        if (!B::InAdmittedService() || !NativeAdmission()) return nullptr;
        auto* owner = Find(id);
        const auto lifecycle = B::Status();
        if (!owner || owner->RequestId || owner->Phase != State::Ready || owner->Cancelled || owner->Uncertain ||
            current != owner->Token || current.Epoch != lifecycle.Epoch || lifecycle.ObservationLost || lifecycle.SeedRequired ||
            lifecycle.Queued || lifecycle.InFlight ||
            (lifecycle.RestoreObserved && (!lifecycle.MassInitialized || !lifecycle.PostSaveCompleted)))
            return nullptr;
        uintptr_t resource = 0;
        if (!Call(Op::Resource, owner->Texture, resource))
        { owner->Uncertain = owner->Failed = true; owner->Phase = State::Failed; return nullptr; }
        if (!resource) { owner->Cancelled = owner->Failed = true; return nullptr; }
        owner->Detached = false; // Issued use now needs explicit detach acknowledgement.
        return owner->Texture;
    }
    Status Inspect(uint64_t id)
    {
        if (!GameThread()) return {};
        auto* owner = Find(id);
        if (!owner) return {};
        return {owner->Phase, owner->Uncertain, owner->Failed,
            owner->Bytes * 2, owner->Bytes + 8 * MiB};
    }
    bool Retire(uint64_t id, bool detached)
    {
        if (!GameThread() || !detached) return false;
        auto* owner = Find(id);
        if (!owner || owner->RequestId || owner->Uncertain) return false;
        owner->Detached = true; owner->Cancelled = true;
        // Service handles pending initialization before beginning retirement.
        if (owner->Phase == State::Ready && NativeAdmission()) Retiring(*owner);
        return true;
    }
    void Service()
    {
        if (!HasOwners.load(std::memory_order_acquire) || !GameThread()) return;
        bool pending = false;
        for (const auto& owner : Owners)
            if (owner && owner->Id && !owner->Uncertain) { pending = true; break; }
        if (!pending) { HasOwners.store(false, std::memory_order_release); return; }
        if (!NativeAdmission()) return;
        const auto lifecycle = B::Status();
        for (auto it = Owners.begin(); it != Owners.end();)
        {
            Owner& owner = **it;
            if (!owner.Id || owner.Uncertain) { ++it; continue; }
            if (owner.RequestId)
            {
                if (Closed || !ValidCellOwner(owner) || !IsCurrentRequest(owner)) Withdraw(owner);
            }
            else if (Closed || owner.Token.Epoch != lifecycle.Epoch || lifecycle.Cancelled || lifecycle.ObservationLost)
                owner.Cancelled = true;
            if (owner.Phase == State::ResourceInitializing && CompleteFence(owner))
            {
                uintptr_t resource = 0;
                if (!Call(Op::Resource, owner.Texture, resource))
                { owner.Uncertain = true; owner.Phase = State::Failed; ++it; continue; }
                if (!resource) { owner.Failed = true; Withdraw(owner); }
                if (owner.RequestId && (!ValidCellOwner(owner) || !IsCurrentRequest(owner))) Withdraw(owner);
                if (owner.Cancelled)
                {
                    owner.Cell.reset();
                    if (owner.Leases.empty()) Retiring(owner);
                    ++it;
                }
                else
                {
                    if (owner.RequestId)
                    {
                        if (!NextPublicationRevision || NextPublicationRevision == (std::numeric_limits<uint64_t>::max)())
                        { Withdraw(owner); owner.Failed = true; Retiring(owner); ++it; continue; }
                        owner.PublicationRevision = NextPublicationRevision++;
                        owner.Cell.reset(); // BulkData owns the upload copy; retain only compact identity metadata.
                    }
                    owner.Phase = State::Ready;
                    ++it;
                }
            }
            else if (owner.Phase == State::ResourceInitializing)
                ++it;
            else if (owner.Phase == State::Ready && owner.Cancelled &&
                (owner.RequestId ? owner.Leases.empty() : owner.Detached))
            { Retiring(owner); ++it; }
            else if (owner.Phase == State::Retiring &&
                (owner.RequestId ? owner.Leases.empty() : owner.Detached) && CompleteFence(owner))
            {
                uintptr_t ignored = 0;
                owner.Uncertain = true;
                if (!Call(Op::Release, owner.Texture, ignored)) { owner.Phase = State::Failed; ++it; continue; }
                owner.Uncertain = false;
                // Every cell lease was individually released before this ordered
                // RHI fence and final strong-reference release.
                it = Owners.erase(it);
            }
            else if (owner.Phase == State::Failed && !owner.Texture && !owner.Uncertain)
                it = Owners.erase(it);
            else ++it;
        }
    }
    void CancelWorld() noexcept
    {
        if (!GameThread()) return;
        InvalidateAll();
        for (auto& owner : Owners) if (owner) owner->Cancelled = true;
    }
    void Shutdown() noexcept
    {
        if (!GameThread()) return;
        Closed = true; CancelWorld();
        try
        {
            Service(); // One nonblocking retirement service pass while NativeApi is live.
            Service(); // Recheck only work made immediately pollable by the first pass.
        }
        catch (...) {}
        // Any still-pending/uncertain fence storage and retained UObject root
        // remain in process-lifetime Owners. This preserves memory safety, but
        // cannot guarantee that those native resources are drained on unload.
    }
}
#endif
