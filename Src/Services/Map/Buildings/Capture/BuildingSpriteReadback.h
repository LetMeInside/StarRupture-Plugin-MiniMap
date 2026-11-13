// Owns one asynchronous RGBA16F GPU readback and its render-command state.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

class FRHICommandListImmediate;
class FRHIGPUFence;
class FRHIGPUTextureReadback;
class FRHITexture;

namespace SDK
{
	class UTextureRenderTarget2D;
}

namespace Services::Map::Buildings::Capture
{
	/** Borrows the renderer-rooted target RHI texture and owns one readback until pixels are copied or retirement is uncertain.
	 * Callers advance and inspect this object on the service thread. Render-command state
	 * owns itself through callback destruction, and native readback resources retire on
	 * the render thread only after GPU completion and unmapping.
	 */
	class BuildingSpriteReadback final
	{
	public:
		/** Describes whether the readback is pending, complete, failed, or not started. */
		enum class Status : std::uint8_t
		{
			Idle,
			Pending,
			Complete,
			Failed
		};

		/** Gets the game-thread resource handoff while the renderer retains the target.
		 * @param renderTarget UObject retained and not reinitialized until readback retirement.
		 * @return True when a request is initialized for later nonblocking advancement.
		 */
		bool Start(SDK::UTextureRenderTarget2D* renderTarget);
		/** Validates and snapshots the native explicit-GPU count before capture work begins. */
		static bool InitializeNativeCapture();
		/** Queues at most one copy or completion-poll command without waiting.
		 * @return Current pending, complete, or failed status.
		 */
		Status Advance();
		/** Returns packed row-major RGBA16F words after verified readback completion. */
		const std::vector<std::uint16_t>& Pixels() const noexcept;
		/** Returns whether all submitted callbacks and native objects have safely retired. */
		bool IsRetired() const noexcept;
		/** Releases CPU callback state after native resources and callable access have retired. */
		bool Reset() noexcept;

	private:
		/** Shares callback-owned RHI/readback state until every native callable is destroyed. */
		struct State;
		/** Performs the copy or completion-poll operation inside one render callback. */
		static void ExecuteReadbackCommand(const std::shared_ptr<State>& state, FRHICommandListImmediate& commandList);
		/** Acquires the typed RHI reference and submits the asynchronous texture copy. */
		static void SubmitTextureCopy(const std::shared_ptr<State>& state, FRHICommandListImmediate& commandList);
		/** Polls the GPU fence and copies validated mapped rows into packed CPU storage. */
		static void PollAndCopyPixels(const std::shared_ptr<State>& state);
		/** Releases mapped storage and destroys the readback after verified completion. */
		static void CompleteReadback(const std::shared_ptr<State>& state);
		/** Checks the expected floating, single-sample capture texture and dimensions. */
		static bool IsCaptureTextureValid(const FRHITexture* texture);
		/** Returns the native readback fence without exposing its protected member to callers. */
		static FRHIGPUFence* GetReadbackFence(FRHIGPUTextureReadback* readback);
		/** Requires a non-null native fence before using UE's pending-write and polling checks. */
		static bool HasCompletedGpuCopy(FRHIGPUTextureReadback* readback);
		/** Applies required copy-source transitions and submits one GPU readback copy. */
		static void SubmitCopyWithTransitions(
			const std::shared_ptr<State>& state,
			FRHICommandListImmediate& commandList,
			FRHITexture* texture);
		/** Copies pitch-aware mapped rows into the owned packed RGBA16F vector. */
		static bool CopyPackedRows(
			const std::shared_ptr<State>& state,
			const std::uint8_t* mapped,
			std::int32_t rowPitchPixels,
			std::int32_t bufferHeight);
		/** Releases readback and texture references on the render thread after safe completion. */
		static void RetireReadbackObjects(const std::shared_ptr<State>& state);
		/** Release-publishes that callback execution has stopped touching shared state. */
		static void RetireCallback(const std::shared_ptr<State>& state);

		std::shared_ptr<State> state_;
	};
}
