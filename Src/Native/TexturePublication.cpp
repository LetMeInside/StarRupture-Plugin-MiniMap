#if defined(MODLOADER_CLIENT_BUILD)
#include "TexturePublication.h"
#include "MassLifecycle.h"
#include "NativeApi.h"
#include "TextureAccess.h"
#include "CaptureRetirementCore.h"
#include <SDK/Engine_classes.hpp>
#include <Windows.h>
#include <cstring>
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
        R::FenceOwner Fence;
        bool Uncertain = false, Detached = true, Cancelled = false;
        bool Failed = false;
        // After release, Unreal's UObject/resource lifecycle owns retirement;
        // this record remains occupied until the ordered fence and release finish.
    };
    std::array<Owner, 3> Owners;
    uint64_t NextId = 1;
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
    { if (id) for (auto& owner : Owners) if (owner.Id == id) return &owner; return nullptr; }

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
        if (width < 1 || height < 1 || width > 2048 || height > 2048 ||
            uint64_t(width) * uint64_t(height) * 8 != bytes.size() || bytes.capacity() > 32 * MiB) return std::nullopt;
        for (size_t i = 0; i < bytes.size(); i += 2)
        {
            const uint16_t half = uint16_t(bytes[i]) | uint16_t(uint16_t(bytes[i + 1]) << 8);
            if ((half & 0x7c00) == 0x7c00) return std::nullopt; // Inf/NaN in any channel.
            if (i % 8 == 6 && ((half & 0x7fff) > 0x3c00 || ((half & 0x8000) && (half & 0x7fff))))
                return std::nullopt; // T in [0,1], including signed zero.
        }
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
        Owner* owner = nullptr;
        uint64_t reservedGpu = 0;
        for (auto& candidate : Owners)
        {
            if (!candidate.Id && !owner) owner = &candidate;
            if (candidate.Id) reservedGpu += candidate.Bytes + 8 * MiB;
        }
        if (!owner || !NextId || reservedGpu + image.Pixels.size() + 8 * MiB > 128 * MiB) return 0;
        owner->Id = NextId++; owner->Token = generation; owner->Bytes = image.Pixels.size();
        owner->Phase = State::Prepared;
        HasOwners.store(true, std::memory_order_release);
        uintptr_t texture = 0, ignored = 0;
        // Reserve capacity before Create/Retain. Never expose an unrooted candidate.
        owner->Uncertain = true;
        if (!Call(Op::Create, nullptr, texture, image.Width, image.Height)) { owner->Failed = true; owner->Phase = State::Failed; return owner->Id; }
        owner->Texture = reinterpret_cast<SDK::UTexture2D*>(texture);
        if (!owner->Texture)
        {
            owner->Uncertain = false; owner->Phase = State::Empty;
            owner->Id = owner->Bytes = 0; // Native call returned null: no allocation to retire.
            return 0;
        }
        if (!Call(Op::Retain, owner->Texture, ignored)) { owner->Failed = true; owner->Phase = State::Failed; return owner->Id; }
        owner->Uncertain = false;
        if (!Fill(owner->Texture, image.Width, image.Height, image.Pixels.data(), image.Pixels.size(), owner->Uncertain))
        {
            owner->Failed = true;
            owner->Phase = State::Failed;
            if (!owner->Uncertain) Retiring(*owner);
            return owner->Id;
        }
        owner->Uncertain = true;
        if (!Call(Op::Update, owner->Texture, ignored)) { owner->Failed = true; owner->Phase = State::Failed; return owner->Id; }
        owner->Uncertain = false;
        owner->Phase = State::ResourceInitializing;
        if (!StartFence(*owner)) owner->Phase = State::Failed;
        // Engine has its own synchronous BulkData copy. No worker memory is
        // retained by UpdateResource; release the consumed canonical CPU owner.
        image = Image{};
        return owner->Id;
    }
    SDK::UTexture2D* Ready(uint64_t id, Generation current)
    {
        if (!B::InAdmittedService() || !NativeAdmission()) return nullptr;
        auto* owner = Find(id);
        const auto lifecycle = B::Status();
        if (!owner || owner->Phase != State::Ready || owner->Cancelled || owner->Uncertain ||
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
        if (!owner || owner->Uncertain) return false;
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
            if (owner.Id && !owner.Uncertain) { pending = true; break; }
        if (!pending) { HasOwners.store(false, std::memory_order_release); return; }
        if (!NativeAdmission()) return;
        const auto lifecycle = B::Status();
        for (auto& owner : Owners)
        {
            if (!owner.Id || owner.Uncertain) continue;
            if (Closed || owner.Token.Epoch != lifecycle.Epoch || lifecycle.Cancelled || lifecycle.ObservationLost)
                owner.Cancelled = true;
            if (owner.Phase == State::ResourceInitializing && CompleteFence(owner))
            {
                uintptr_t resource = 0;
                if (!Call(Op::Resource, owner.Texture, resource)) { owner.Uncertain = true; owner.Phase = State::Failed; continue; }
                if (!resource) { owner.Cancelled = true; owner.Failed = true; }
                // Future request/dependency generation is rechecked by Ready.
                if (owner.Cancelled) Retiring(owner);
                else owner.Phase = State::Ready;
            }
            else if (owner.Phase == State::Ready && owner.Cancelled && owner.Detached) Retiring(owner);
            else if (owner.Phase == State::Retiring && owner.Detached && CompleteFence(owner))
            {
                uintptr_t ignored = 0;
                owner.Uncertain = true;
                if (!Call(Op::Release, owner.Texture, ignored)) { owner.Phase = State::Failed; continue; }
                owner.Uncertain = false;
                // All issued uses were detached and the ordered RHI fence was
                // destroyed before releasing our strong reference. UObject and
                // resource destruction now follow Unreal's normal GC lifecycle;
                // this fence is not a GPU-idle certificate. Recycle bookkeeping
                // without deleting the UObject or carrying historical debt.
                owner.Id = owner.Bytes = 0;
                owner.Token = {};
                owner.Texture = nullptr;
                owner.Phase = T::State::Empty;
                owner.Detached = true;
                owner.Cancelled = owner.Failed = false;
            }
        }
    }
    void CancelWorld() noexcept
    {
        if (!GameThread()) return;
        for (auto& owner : Owners) if (owner.Id) owner.Cancelled = true;
    }
    void Shutdown() noexcept
    {
        if (!GameThread()) return;
        Closed = true; CancelWorld();
        try { Service(); } catch (...) {} // Pending/uncertain roots and storage survive.
    }
}
#endif
