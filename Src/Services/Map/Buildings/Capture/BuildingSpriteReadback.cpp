// Implements nonblocking GPU readback with typed RHI ownership and packed CPU rows.
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

#include "BuildingSpriteReadback.h"

#include "Native/NativeApi.h"
#include "Native/RHITextureAcquisition.h"
#include "Services/Map/Buildings/Capture/ReadbackFenceAccessor.h"

#include <RHIResources.h>
#include <RHITransition.h>
#include "Generated/Unreal/RHIGPUReadbackCompat.h"

#include <atomic>
#include <cstring>

namespace Services::Map::Buildings::Capture
{
	/** Stores render-thread-only readback state and release-published packed pixels. */
	struct BuildingSpriteReadback::State final
	{
		std::atomic<Status> Current{Status::Pending};
		std::atomic<bool> CommandOutstanding{false};
		std::atomic<bool> CopySubmitted{false};
		std::atomic<bool> CallbackRetired{true};
		std::atomic<bool> ResourcesRetired{false};
		SDK::UTextureRenderTarget2D* RenderTarget = nullptr;
		FTextureRHIRef Texture;
		FRHIGPUTextureReadback* Readback = nullptr;
		std::vector<std::uint16_t> PackedPixels;
		std::uint32_t CommandSpec = 0;
	};

	/** Validates and snapshots the immutable native GPU count before BM-07 RHI work. */
	bool BuildingSpriteReadback::InitializeNativeCapture()
	{
		const auto* nativeApi = MiniMapNative::Get();
		if (!nativeApi || !nativeApi->texture.explicitGpuCount)
		{
			return false;
		}
		const std::uint32_t gpuCount = *nativeApi->texture.explicitGpuCount;
		if (gpuCount == 0 || gpuCount > 8)
		{
			return false;
		}
		if (GNumExplicitGPUsForRendering == 0)
		{
			GNumExplicitGPUsForRendering = gpuCount;
			return true;
		}
		return GNumExplicitGPUsForRendering == gpuCount;
	}

	/** Starts a readback while the renderer continues retaining its render target. */
	bool BuildingSpriteReadback::Start(SDK::UTextureRenderTarget2D* renderTarget)
	{
		if (state_ || !renderTarget || !InitializeNativeCapture())
		{
			return false;
		}
		try
		{
			state_ = std::make_shared<State>();
			state_->RenderTarget = renderTarget;
			state_->PackedPixels.reserve(256u * 256u * 4u);
		}
		catch (...)
		{
			state_.reset();
			return false;
		}
		return true;
	}

	/** Submits one render callback that copies, polls, maps, packs, and retires the GPU data. */
	BuildingSpriteReadback::Status BuildingSpriteReadback::Advance()
	{
		if (!state_)
		{
			return Status::Idle;
		}
		if (state_->Current.load(std::memory_order_acquire) != Status::Pending)
		{
			return state_->Current.load(std::memory_order_acquire);
		}
		bool expected = false;
		if (!state_->CommandOutstanding.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
		{
			return Status::Pending;
		}
		state_->CallbackRetired.store(false, std::memory_order_release);
		const auto* nativeApi = MiniMapNative::Get();
		if (!nativeApi || !nativeApi->texture.submitRenderCommand || !nativeApi->texture.readbackConstruct ||
			!nativeApi->texture.readbackDestroy || !nativeApi->texture.readbackCopy ||
			!nativeApi->texture.readbackLock || !nativeApi->texture.readbackUnlock ||
			!nativeApi->texture.transitionResources || !nativeApi->texture.getRenderTargetResource ||
			!nativeApi->asset.memoryMalloc ||
			!nativeApi->texture.memoryFree)
		{
			state_->RenderTarget = nullptr;
			state_->CommandOutstanding.store(false, std::memory_order_release);
			state_->CallbackRetired.store(true, std::memory_order_release);
			state_->ResourcesRetired.store(true, std::memory_order_release);
			state_->Current.store(Status::Failed, std::memory_order_release);
			return Status::Failed;
		}
		const std::shared_ptr<State> callbackState = state_;
		try
		{
			nativeApi->texture.submitRenderCommand(
				nativeApi->texture.renderCommandPipe,
				L"BuildingMapReadback",
				state_->CommandSpec,
				TStatId{},
				TUniqueFunction<void(FRHICommandListImmediate&)>([callbackState](FRHICommandListImmediate& commandList)
				{
					ExecuteReadbackCommand(callbackState, commandList);
				}));
		}
		catch (...)
		{
			// Acceptance is uncertain, so keep the outstanding state and every native owner.
			return Status::Failed;
		}
		return Status::Pending;
	}

	/** Runs the initial copy or one later completion poll on the render thread. */
	void BuildingSpriteReadback::ExecuteReadbackCommand(
		const std::shared_ptr<BuildingSpriteReadback::State>& state,
		FRHICommandListImmediate& commandList)
	{
		const auto* nativeApi = MiniMapNative::Get();
		if (!nativeApi)
		{
			state->Current.store(Status::Failed, std::memory_order_release);
			RetireCallback(state);
			return;
		}
		try
		{
			if (!state->CopySubmitted.load(std::memory_order_acquire))
			{
				SubmitTextureCopy(state, commandList);
			}
			else if (state->Readback)
			{
				if (!GetReadbackFence(state->Readback))
				{
					// An absent fence cannot prove copy completion; retain both native owners in failed state.
					state->Current.store(Status::Failed, std::memory_order_release);
				}
				else if (HasCompletedGpuCopy(state->Readback))
				{
					PollAndCopyPixels(state);
				}
			}
		}
		catch (...)
		{
			state->Current.store(Status::Failed, std::memory_order_release);
		}
		RetireCallback(state);
	}

	/** Acquires a retained typed RHI reference and issues a fenced copy on the render thread. */
	void BuildingSpriteReadback::SubmitTextureCopy(
		const std::shared_ptr<State>& state,
		FRHICommandListImmediate& commandList)
	{
		const auto* nativeApi = MiniMapNative::Get();
		if (!MiniMapBuildingMapNative::RHITextureAcquisition::Acquire(
			state->RenderTarget, state->Texture))
		{
			state->RenderTarget = nullptr;
			RetireReadbackObjects(state);
			state->ResourcesRetired.store(true, std::memory_order_release);
			state->Current.store(Status::Failed, std::memory_order_release);
			return;
		}
		state->RenderTarget = nullptr;
		FRHITexture* texture = state->Texture.GetReference();
		if (!IsCaptureTextureValid(texture))
		{
			RetireReadbackObjects(state);
			state->ResourcesRetired.store(true, std::memory_order_release);
			state->Current.store(Status::Failed, std::memory_order_release);
			return;
		}
		void* storage = nativeApi->asset.memoryMalloc(sizeof(FRHIGPUTextureReadback), alignof(FRHIGPUTextureReadback));
		if (!storage)
		{
			RetireReadbackObjects(state);
			state->ResourcesRetired.store(true, std::memory_order_release);
			state->Current.store(Status::Failed, std::memory_order_release);
			return;
		}
		state->Readback = nativeApi->texture.readbackConstruct(
			static_cast<FRHIGPUTextureReadback*>(storage), FName{});
		if (!state->Readback)
		{
			nativeApi->texture.memoryFree(storage);
			RetireReadbackObjects(state);
			state->ResourcesRetired.store(true, std::memory_order_release);
			state->Current.store(Status::Failed, std::memory_order_release);
			return;
		}
		SubmitCopyWithTransitions(state, commandList, texture);
		state->CopySubmitted.store(true, std::memory_order_release);
	}

	/** Validates the acquired float render target against the fixed 256-square capture contract. */
	bool BuildingSpriteReadback::IsCaptureTextureValid(const FRHITexture* texture)
	{
		if (texture == nullptr)
		{
			return false;
		}
		const FRHITextureDesc& description = texture->GetDesc();
		return description.Format == PF_FloatRGBA && description.NumSamples == 1 &&
			description.Extent.X == 256 && description.Extent.Y == 256;
	}

	/** Applies the demonstrated shader-resource/copy-source transitions around the GPU copy. */
	void BuildingSpriteReadback::SubmitCopyWithTransitions(
		const std::shared_ptr<State>& state,
		FRHICommandListImmediate& commandList,
		FRHITexture* texture)
	{
		const auto* nativeApi = MiniMapNative::Get();
		const FRHITransitionInfo before(texture, ERHIAccess::SRVMask, ERHIAccess::CopySrc);
		// PDB evidence records Immediate-to-CommandList at +0; UE 5.6.1 declares the public nonvirtual base chain through ComputeCommandList.
		nativeApi->texture.transitionResources(
			reinterpret_cast<FRHIComputeCommandList*>(&commandList), MakeArrayView(&before, 1), ERHITransitionCreateFlags::None);
		nativeApi->texture.readbackCopy(
			state->Readback, reinterpret_cast<FRHICommandList&>(commandList), texture,
			FIntVector(0, 0, 0), 0, FIntVector(256, 256, 1));
		const FRHITransitionInfo after(texture, ERHIAccess::CopySrc, ERHIAccess::SRVMask);
		nativeApi->texture.transitionResources(
			reinterpret_cast<FRHIComputeCommandList*>(&commandList), MakeArrayView(&after, 1), ERHITransitionCreateFlags::None);
	}

	/** Reads the protected fence through the member pointer declared by ReadbackFenceAccessor. */
	FRHIGPUFence* BuildingSpriteReadback::GetReadbackFence(FRHIGPUTextureReadback* readback)
	{
		if (!readback)
		{
			return nullptr;
		}
		FRHIGPUMemoryReadback& memoryReadback = *readback;
		return ReadbackFenceAccessor::GetFenceReference(memoryReadback).GetReference();
	}

	/** Requires an existing native fence, then delegates pending-write and poll checks to UE IsReady(). */
	bool BuildingSpriteReadback::HasCompletedGpuCopy(FRHIGPUTextureReadback* readback)
	{
		return GetReadbackFence(readback) != nullptr && readback->IsReady();
	}

	/** Locks completed staging data, validates row geometry, copies pixels, and unmaps before retirement. */
	void BuildingSpriteReadback::PollAndCopyPixels(const std::shared_ptr<State>& state)
	{
		const auto* nativeApi = MiniMapNative::Get();
		int32_t rowPitchPixels = 0;
		int32_t bufferHeight = 0;
		const auto* mapped = static_cast<const std::uint8_t*>(
			nativeApi->texture.readbackLock(state->Readback, rowPitchPixels, &bufferHeight));
		if (!mapped)
		{
			// IsReady proved GPU copy completion; a null Lock result established that no mapping was acquired.
			RetireReadbackObjects(state);
			state->ResourcesRetired.store(true, std::memory_order_release);
			state->Current.store(Status::Failed, std::memory_order_release);
			return;
		}
		if (!CopyPackedRows(state, mapped, rowPitchPixels, bufferHeight))
		{
			nativeApi->texture.readbackUnlock(state->Readback);
			RetireReadbackObjects(state);
			state->ResourcesRetired.store(true, std::memory_order_release);
			state->Current.store(Status::Failed, std::memory_order_release);
			return;
		}
		nativeApi->texture.readbackUnlock(state->Readback);
		CompleteReadback(state);
	}

	/** Copies only the validated image rectangle from pitch-aware mapped storage. */
	bool BuildingSpriteReadback::CopyPackedRows(
		const std::shared_ptr<State>& state,
		const std::uint8_t* mapped,
		std::int32_t rowPitchPixels,
		std::int32_t bufferHeight)
	{
		if (rowPitchPixels < 256 || bufferHeight < 256 || rowPitchPixels > 16384 || bufferHeight > 16384)
		{
			return false;
		}
		try
		{
			state->PackedPixels.resize(256u * 256u * 4u);
		}
		catch (...)
		{
			return false;
		}
		for (std::size_t row = 0; row < 256; ++row)
		{
			std::memcpy(
				state->PackedPixels.data() + row * 256u * 4u,
				mapped + row * std::size_t(rowPitchPixels) * 8u,
				256u * 8u);
		}
		return true;
	}

	/** Retires completed GPU objects after unlock and publishes packed pixels to the game thread. */
	void BuildingSpriteReadback::CompleteReadback(const std::shared_ptr<State>& state)
	{
		RetireReadbackObjects(state);
		state->ResourcesRetired.store(true, std::memory_order_release);
		state->Current.store(Status::Complete, std::memory_order_release);
	}

	/** Destroys a mapped or completed readback and releases its retained RHI texture on the render thread. */
	void BuildingSpriteReadback::RetireReadbackObjects(const std::shared_ptr<State>& state)
	{
		const auto* nativeApi = MiniMapNative::Get();
		if (state->Readback)
		{
			void* storage = state->Readback;
			nativeApi->texture.readbackDestroy(state->Readback);
			nativeApi->texture.memoryFree(storage);
			state->Readback = nullptr;
		}
		state->Texture.SafeRelease();
	}

	/** Publishes callback completion after all callback reads and writes have ended. */
	void BuildingSpriteReadback::RetireCallback(const std::shared_ptr<State>& state)
	{
		state->CommandOutstanding.store(false, std::memory_order_release);
		state->CallbackRetired.store(true, std::memory_order_release);
	}

	/** Returns the owned packed samples only after the callback release-publishes completion. */
	const std::vector<std::uint16_t>& BuildingSpriteReadback::Pixels() const noexcept
	{
		static const std::vector<std::uint16_t> empty;
		return state_ ? state_->PackedPixels : empty;
	}

	/** Reports native retirement only after callback destruction and GPU object release. */
	bool BuildingSpriteReadback::IsRetired() const noexcept
	{
		return !state_ || (state_->ResourcesRetired.load(std::memory_order_acquire) &&
			state_->CallbackRetired.load(std::memory_order_acquire) &&
			!state_->CommandOutstanding.load(std::memory_order_acquire));
	}

	/** Drops the completed per-pass job only after all native resource ownership is retired. */
	bool BuildingSpriteReadback::Reset() noexcept
	{
		if (!IsRetired())
		{
			return false;
		}
		state_.reset();
		return true;
	}
}

#endif // MODLOADER_CLIENT_BUILD
