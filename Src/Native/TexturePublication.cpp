#if defined(MODLOADER_CLIENT_BUILD)
#include "TexturePublication.h"
#include "NativeApi.h"
#include "TextureAccess.h"
#include "CaptureRetirementCore.h"
#include <SDK/Engine_classes.hpp>
#include <Windows.h>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include <atomic>

namespace
{
    namespace N = MiniMapNative;
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
    // Pending native fences and retained texture roots may outlive plugin
    // shutdown. Keep their owner storage at process lifetime so module static
    // destruction cannot free fence memory still reachable by the RHI thread.
    std::vector<std::unique_ptr<Owner>>& Owners = *new std::vector<std::unique_ptr<Owner>>();
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
    {
        if (id) for (auto& owner : Owners) if (owner && owner->Id == id) return owner.get();
        return nullptr;
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
        if (Closed || !NativeAdmission() || !generation.Request || !generation.Revision ||
            image.Pixels.empty() || image.Width < 1 || image.Height < 1 ||
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
    SDK::UTexture2D* Ready(uint64_t id, Generation current)
    {
        if (!NativeAdmission()) return nullptr;
        auto* owner = Find(id);
        if (!owner || owner->Phase != State::Ready || owner->Cancelled || owner->Uncertain ||
            current != owner->Token)
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
    /** Services generic native texture owners without changing their lifetime contracts. */
    void Service()
    {
        if (!HasOwners.load(std::memory_order_acquire) || !GameThread()) return;
        bool pending = false;
        for (const auto& owner : Owners)
            if (owner && owner->Id && !owner->Uncertain) { pending = true; break; }
        if (!pending) { HasOwners.store(false, std::memory_order_release); return; }
        if (!NativeAdmission()) return;
        for (auto it = Owners.begin(); it != Owners.end();)
        {
            Owner& owner = **it;
            if (!owner.Id || owner.Uncertain) { ++it; continue; }
            if (Closed) owner.Cancelled = true;
            if (owner.Phase == State::ResourceInitializing && CompleteFence(owner))
            {
                uintptr_t resource = 0;
                if (!Call(Op::Resource, owner.Texture, resource))
                { owner.Uncertain = true; owner.Phase = State::Failed; ++it; continue; }
                if (!resource) { owner.Failed = true; owner.Cancelled = true; }
                if (owner.Cancelled)
                {
                    Retiring(owner);
                    ++it;
                }
                else
                {
                    owner.Phase = State::Ready;
                    ++it;
                }
            }
            else if (owner.Phase == State::ResourceInitializing)
                ++it;
            else if (owner.Phase == State::Ready && owner.Cancelled && owner.Detached)
            {
                Retiring(owner);
                ++it;
            }
            else if (owner.Phase == State::Retiring && owner.Detached && CompleteFence(owner))
            {
                uintptr_t ignored = 0;
                owner.Uncertain = true;
                if (!Call(Op::Release, owner.Texture, ignored)) { owner.Phase = State::Failed; ++it; continue; }
                owner.Uncertain = false;
                // The ordered RHI fence completes before releasing the final strong reference.
                it = Owners.erase(it);
            }
            else if (owner.Phase == State::Failed && !owner.Texture && !owner.Uncertain)
                it = Owners.erase(it);
            else ++it;
        }
    }
    void Shutdown() noexcept
    {
        if (!GameThread()) return;
        Closed = true;
        for (auto& owner : Owners) if (owner) owner->Cancelled = true;
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



