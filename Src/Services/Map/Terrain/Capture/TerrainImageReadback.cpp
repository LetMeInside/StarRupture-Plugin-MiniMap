// Implements pitch-aware BGRA8 GPU acquisition with the existing native Building Map RHI primitives.
// Cancellation drains genuine GPU/callback work; no RHI submission fence substitutes for GPU completion.
#if defined(MODLOADER_CLIENT_BUILD)
#include <Windows.h>
#include <strsafe.h>
#undef Yield
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#define UBT_COMPILED_PLATFORM Windows
#define UBT_COMPILED_TARGET Game
#define PLATFORM_WINDOWS 1
#define PLATFORM_64BITS 1
#define UE_BUILD_SHIPPING 1
#define WITH_EDITOR 0
#define WITH_EDITORONLY_DATA 0
#define WITH_ENGINE 1
#define WITH_COREUOBJECT 1
#define WITH_SERVER_CODE 1
#define WITH_UNREAL_DEVELOPER_TOOLS 0
#define WITH_PLUGIN_SUPPORT 0
#define WITH_PERFCOUNTERS 0
#define IS_MONOLITHIC 1
#define IS_PROGRAM 0
#define STATS 0
#define CORE_API
#define TRACELOG_API
#define RHI_API
#define RENDERCORE_API
#define WITH_MGPU 1
#define WITH_RHI_BREADCRUMBS 0
#include <Templates/Function.h>
#include <Stats/Stats.h>
#include "TerrainImageReadback.h"
#include "../TerrainImage.h"
#include "Native/NativeApi.h"
#include "Native/RHITextureAcquisition.h"
#include "Services/Map/Buildings/Capture/ReadbackFenceAccessor.h"
#include <RHITransition.h>
#include <atomic>
#include <exception>
#include <limits>
#include <utility>

namespace Services::Map::Terrain::Capture
{
    /** Callback-owned RHI references and temporary RGB; outcome publication never releases ownership early. */
    struct TerrainImageReadback::State final
    {
        std::atomic<Status> Current{Status::Pending};
        std::atomic<bool> Outstanding{false};
        std::atomic<bool> ResourcesRetired{false};
        std::atomic<bool> Discard{false};
        std::atomic<bool> RequireGpuCompletion{false};
        std::atomic<bool> WaitForGpuCompletion{false};
        std::atomic<bool> GpuCompletionAcknowledged{false};
        void* TargetResource = nullptr;
        FTextureRHIRef Texture;
        FRHIGPUTextureReadback* Readback = nullptr;
        bool CopySubmitted = false; // Accessed only by serialized render callbacks.
        std::uint32_t Width = 0;
        std::uint32_t Height = 0;
        std::uint32_t CommandSpec = 0;
        std::vector<std::uint8_t> Rgb;
    };

    /** Never abandons native or callable ownership when retirement cannot be acknowledged. */
    TerrainImageReadback::~TerrainImageReadback()
    {
        if (!IsRetired())
            std::terminate();
    }

    /** Matches Building Map's immutable GPU-count initialization used by inline RHI fence operations. */
    bool TerrainImageReadback::InitializeNativeCapture()
    {
        const auto* api = MiniMapNative::Get();
        if (!api || !api->texture.explicitGpuCount)
            return false;
        const auto count = *api->texture.explicitGpuCount;
        if (count == 0 || count > 8)
            return false;
        if (GNumExplicitGPUsForRendering == 0)
            GNumExplicitGPUsForRendering = count;
        return GNumExplicitGPUsForRendering == count;
    }

    /** Requires all allocation/copy/map/retirement operations before admitting asynchronous work. */
    bool TerrainImageReadback::HasNativeBindings()
    {
        const auto* api = MiniMapNative::Get();
        return api && api->texture.submitRenderCommand && api->texture.readbackConstruct &&
            api->texture.readbackDestroy && api->texture.readbackCopy && api->texture.readbackLock &&
            api->texture.readbackUnlock && api->texture.transitionResources && api->asset.memoryMalloc &&
            api->texture.memoryFree;
    }

    /** Allocates RGB before mapping and borrows the unchanged target using the existing game-thread handoff. */
    bool TerrainImageReadback::Start(SDK::UTextureRenderTarget2D* target,
        std::uint32_t width, std::uint32_t height, bool drainOnly)
    {
        if (state_ || !target || !TerrainImage::IsValidSourceDimensions(width, height) ||
            !HasNativeBindings() || !InitializeNativeCapture())
            return false;
        void* resource = MiniMapBuildingMapNative::RHITextureAcquisition::GetRenderTargetResourceOnGameThread(target);
        if (!resource)
            return false;
        auto state = std::make_shared<State>();
        state->Width = width;
        state->Height = height;
        state->Discard.store(drainOnly, std::memory_order_relaxed);
        state->RequireGpuCompletion.store(drainOnly, std::memory_order_relaxed);
        if (!drainOnly)
            state->Rgb.resize(static_cast<std::size_t>(width) * height * 3u);
        state->TargetResource = resource;
        state_ = std::move(state);
        return true;
    }

    /** Enqueues one callback and retains all state on an uncertain native submission outcome. */
    TerrainImageReadback::Status TerrainImageReadback::Advance()
    {
        if (!state_)
            return Status::Idle;
        const auto current = state_->Current.load(std::memory_order_acquire);
        if (current != Status::Pending)
            return current;
        // A previous callable must also be destroyed before submitting another callback.
        if (state_.use_count() != 1 || state_->Outstanding.exchange(true, std::memory_order_acq_rel))
            return Status::Pending;
        try
        {
            const auto state = state_;
            MiniMapNative::Get()->texture.submitRenderCommand(MiniMapNative::Get()->texture.renderCommandPipe,
                L"TerrainImageReadback", state_->CommandSpec, TStatId{},
                TUniqueFunction<void(FRHICommandListImmediate&)>([state](FRHICommandListImmediate& commandList)
                {
                    Execute(state, commandList);
                }));
        }
        catch (...)
        {
            // The callback may have been accepted: neither retry nor release uncertain native ownership.
            return Status::Failed;
        }
        return Status::Pending;
    }

    /** Handles cancellation before copy, otherwise waits for the real copy fence before releasing resources. */
    void TerrainImageReadback::Execute(const std::shared_ptr<State>& state, FRHICommandListImmediate& commandList)
    {
        try
        {
            if (state->WaitForGpuCompletion.load(std::memory_order_acquire))
                ExecuteSynchronousRetirement(state, commandList);
            else if (!state->CopySubmitted)
            {
                if (state->Discard.load(std::memory_order_acquire) &&
                    !state->RequireGpuCompletion.load(std::memory_order_acquire))
                    Finish(state, Status::Failed);
                else
                    SubmitCopy(state, commandList);
            }
            else
            {
                auto& memory = static_cast<FRHIGPUMemoryReadback&>(*state->Readback);
                const auto& fence = Services::Map::Buildings::Capture::ReadbackFenceAccessor::GetFenceReference(memory);
                if (!fence)
                    state->Current.store(Status::Failed, std::memory_order_release);
                else if (state->Readback->IsReady())
                {
                    state->GpuCompletionAcknowledged.store(true, std::memory_order_release);
                    if (state->Discard.load(std::memory_order_acquire))
                        Finish(state, Status::Failed);
                    else
                        CopyCompletedPixels(state);
                }
            }
        }
        catch (...)
        {
            // Completion/unmapping may be uncertain; preserve native owners and report failure.
            state->Current.store(Status::Failed, std::memory_order_release);
        }
        state->Outstanding.store(false, std::memory_order_release);
    }

    /** Submits a drain-only copy if needed, waits for its GPU fence, and releases it without mapping pixels. */
    void TerrainImageReadback::ExecuteSynchronousRetirement(const std::shared_ptr<State>& state,
        FRHICommandListImmediate& commandList)
    {
        if (!state->CopySubmitted)
            SubmitCopy(state, commandList);
        if (!state->CopySubmitted)
            return;
        if (WaitForGpuCopy(state, commandList))
            Finish(state, Status::Failed);
        else
            state->Current.store(Status::Failed, std::memory_order_release);
    }

    /** Waits on the readback's actual GPU fence from its render-thread callback. */
    bool TerrainImageReadback::WaitForGpuCopy(const std::shared_ptr<State>& state,
        FRHICommandListImmediate& commandList)
    {
        if (state->GpuCompletionAcknowledged.load(std::memory_order_acquire))
            return true;
        if (!state->Readback)
            return false;
        auto& memory = static_cast<FRHIGPUMemoryReadback&>(*state->Readback);
        const auto& fence = Services::Map::Buildings::Capture::ReadbackFenceAccessor::GetFenceReference(memory);
        if (!fence)
            return false;
        state->Readback->Wait(commandList, state->Readback->GetLastCopyGPUMask());
        if (!state->Readback->IsReady())
            return false;
        state->GpuCompletionAcknowledged.store(true, std::memory_order_release);
        return true;
    }

    /** Validates native BGRA8 geometry before allocating and submitting the GPU staging readback. */
    void TerrainImageReadback::SubmitCopy(const std::shared_ptr<State>& state, FRHICommandListImmediate& commandList)
    {
        const auto* api = MiniMapNative::Get();
        if (!MiniMapBuildingMapNative::RHITextureAcquisition::Acquire(state->TargetResource, state->Texture))
        {
            Finish(state, Status::Failed);
            return;
        }
        state->TargetResource = nullptr;
        const auto& description = state->Texture->GetDesc();
        if (description.Format != PF_B8G8R8A8 || description.NumSamples != 1 ||
            description.Extent.X != static_cast<int32>(state->Width) ||
            description.Extent.Y != static_cast<int32>(state->Height))
        {
            Finish(state, Status::Failed);
            return;
        }
        void* storage = api->asset.memoryMalloc(sizeof(FRHIGPUTextureReadback), alignof(FRHIGPUTextureReadback));
        if (!storage)
        {
            Finish(state, Status::Failed);
            return;
        }
        state->Readback = api->texture.readbackConstruct(static_cast<FRHIGPUTextureReadback*>(storage), FName{});
        if (!state->Readback)
        {
            api->texture.memoryFree(storage);
            Finish(state, Status::Failed);
            return;
        }
        CopyWithTransitions(state, commandList);
        state->CopySubmitted = true;
    }

    /** Reuses the proven native transitions and copy binding with this image's complete extent. */
    void TerrainImageReadback::CopyWithTransitions(const std::shared_ptr<State>& state,
        FRHICommandListImmediate& commandList)
    {
        const auto* api = MiniMapNative::Get();
        auto* texture = state->Texture.GetReference();
        const FRHITransitionInfo before(texture, ERHIAccess::SRVMask, ERHIAccess::CopySrc);
        api->texture.transitionResources(reinterpret_cast<FRHIComputeCommandList*>(&commandList),
            MakeArrayView(&before, 1), ERHITransitionCreateFlags::None);
        api->texture.readbackCopy(state->Readback, reinterpret_cast<FRHICommandList&>(commandList), texture,
            FIntVector(0, 0, 0), 0, FIntVector(state->Width, state->Height, 1));
        const FRHITransitionInfo after(texture, ERHIAccess::CopySrc, ERHIAccess::SRVMask);
        api->texture.transitionResources(reinterpret_cast<FRHIComputeCommandList*>(&commandList),
            MakeArrayView(&after, 1), ERHITransitionCreateFlags::None);
    }

    /** Locks only a GPU-completed copy; RGB conversion cannot allocate or throw while staging is mapped. */
    void TerrainImageReadback::CopyCompletedPixels(const std::shared_ptr<State>& state)
    {
        const auto* api = MiniMapNative::Get();
        int32 pitch = 0;
        int32 height = 0;
        const auto* mapped = static_cast<const std::uint8_t*>(api->texture.readbackLock(state->Readback, pitch, &height));
        if (!mapped)
        {
            Finish(state, Status::Failed);
            return;
        }
        const bool copied = CopyRows(state, mapped, pitch, height);
        api->texture.readbackUnlock(state->Readback);
        Finish(state, copied ? Status::Complete : Status::Failed);
    }

    /** Keeps D3D native row zero as image top: +U is world +X, +V is world +Y, with no artwork-specific flip. */
    bool TerrainImageReadback::CopyRows(const std::shared_ptr<State>& state, const std::uint8_t* mapped,
        std::int32_t pitchPixels, std::int32_t bufferHeight)
    {
        if (pitchPixels < static_cast<std::int32_t>(state->Width) ||
            bufferHeight < static_cast<std::int32_t>(state->Height))
            return false;
        const auto pitchBytes = static_cast<std::size_t>(pitchPixels) * 4u;
        if (pitchBytes > (std::numeric_limits<std::size_t>::max)() / state->Height)
            return false;
        for (std::size_t row = 0; row < state->Height; ++row)
        {
            const auto* source = mapped + row * pitchBytes;
            auto* destination = state->Rgb.data() + row * state->Width * 3u;
            for (std::size_t column = 0; column < state->Width; ++column)
            {
                destination[column * 3u] = source[column * 4u + 2u];
                destination[column * 3u + 1u] = source[column * 4u + 1u];
                destination[column * 3u + 2u] = source[column * 4u];
            }
        }
        return true; // Alpha is discarded; the authoritative encoder emits opaque BC1.
    }

    /** Called only before a copy or after genuine GPU completion and unmapping, on the render thread. */
    void TerrainImageReadback::Finish(const std::shared_ptr<State>& state, Status result)
    {
        const auto* api = MiniMapNative::Get();
        if (state->Readback)
        {
            api->texture.readbackDestroy(state->Readback);
            api->texture.memoryFree(state->Readback);
            state->Readback = nullptr;
        }
        state->Texture.SafeRelease();
        state->TargetResource = nullptr;
        state->ResourcesRetired.store(true, std::memory_order_release);
        state->Current.store(result, std::memory_order_release);
    }

    /** Signals cancellation without releasing native ownership or stopping completion polls. */
    void TerrainImageReadback::Cancel() noexcept
    {
        if (state_)
            state_->Discard.store(true, std::memory_order_release);
    }

    /** Requests actual GPU-copy completion before readback native owners can be released. */
    void TerrainImageReadback::RequestSynchronousRetirement() noexcept
    {
        RequestGpuCompletionRetirement();
        if (state_)
            state_->WaitForGpuCompletion.store(true, std::memory_order_release);
    }

    /** Keeps cancellation from treating a pre-copy resource release as GPU completion. */
    void TerrainImageReadback::RequestGpuCompletionRetirement() noexcept
    {
        if (!state_)
            return;
        state_->Discard.store(true, std::memory_order_release);
        state_->RequireGpuCompletion.store(true, std::memory_order_release);
    }

    /** Returns positive copy completion evidence, including evidence preserved across Reset. */
    bool TerrainImageReadback::HasGpuCompletionAcknowledgement() const noexcept
    {
        return gpuCompletionAcknowledged_ || (state_ &&
            state_->GpuCompletionAcknowledged.load(std::memory_order_acquire));
    }

    /** Includes callable destruction: callback-local completion flags alone do not establish this boundary. */
    bool TerrainImageReadback::IsRetired() const noexcept
    {
        return !state_ || (state_->ResourcesRetired.load(std::memory_order_acquire) &&
            !state_->Outstanding.load(std::memory_order_acquire) && state_.use_count() == 1);
    }

    /** Detects callback copies that remain alive after an ordered render-fence acknowledgement. */
    bool TerrainImageReadback::HasPendingCallbackOwner() const noexcept
    {
        return state_ && (state_->Outstanding.load(std::memory_order_acquire) || state_.use_count() != 1);
    }

    /** Returns the current immutable observation of asynchronous readback state. */
    TerrainImageReadback::Status TerrainImageReadback::GetStatus() const noexcept
    {
        return state_ ? state_->Current.load(std::memory_order_acquire) : Status::Idle;
    }

    /** Does not expose raw storage while a native callback can still read or write it. */
    const std::vector<std::uint8_t>& TerrainImageReadback::Pixels() const noexcept
    {
        static const std::vector<std::uint8_t> empty;
        return state_ && IsRetired() && state_->Current.load(std::memory_order_acquire) == Status::Complete
            ? state_->Rgb : empty;
    }

    /** Moves pixels only after complete GPU readback and native retirement have both been acknowledged. */
    std::vector<std::uint8_t> TerrainImageReadback::TakePixels() noexcept
    {
        if (!state_ || !IsRetired() || state_->Current.load(std::memory_order_acquire) != Status::Complete)
            return {};
        return std::move(state_->Rgb);
    }

    /** Drops temporary acquisition storage after every native object and callable owner is gone. */
    bool TerrainImageReadback::Reset() noexcept
    {
        if (!IsRetired())
            return false;
        if (state_ && state_->GpuCompletionAcknowledged.load(std::memory_order_acquire))
            gpuCompletionAcknowledged_ = true;
        state_.reset();
        return true;
    }
}
#endif
