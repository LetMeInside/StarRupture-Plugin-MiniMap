// Owns one BGRA8 terrain target readback and temporary packed opaque RGB acquisition data.
// Native texture/copy/callback ownership follows the established Building Map readback mechanism.
#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include <cstdint>
#include <memory>
#include <vector>
class FRHICommandListImmediate;
namespace SDK { class UTextureRenderTarget2D; }

namespace Services::Map::Terrain::Capture
{
    /** One private native readback, advanced on the game thread with execution on the render thread.
     * The caller retains the unchanged target and private world until GPU completion is positively
     * acknowledged and IsRetired/Reset establish unmapping, native destruction and callback retirement.
     */
    class TerrainImageReadback final
    {
    public:
        /** Separates logical outcome from native retirement, which must be checked independently. */
        enum class Status { Idle, Pending, Complete, Failed };
        /** Creates inert state without native work. */
        TerrainImageReadback() = default;
        /** Prevents dropping uncertain native/callback ownership. */
        ~TerrainImageReadback();
        /** Prevents aliasing the readback's owner. */
        TerrainImageReadback(const TerrainImageReadback&) = delete;
        /** Prevents replacing outstanding callback state. */
        TerrainImageReadback& operator=(const TerrainImageReadback&) = delete;
        /** Initializes existing RHI inline support from the authoritative native GPU count. */
        static bool InitializeNativeCapture();
        /** Borrows a rooted target; drainOnly omits RGB allocation but still requires a GPU copy. */
        bool Start(SDK::UTextureRenderTarget2D* target, std::uint32_t width, std::uint32_t height,
            bool drainOnly = false);
        /** Queues at most one native copy/poll callback; GPU waiting is enabled only by teardown request. */
        Status Advance();
        /** Suppresses pixel copying without abandoning an accepted GPU copy or callback. */
        void Cancel() noexcept;
        /** Requires a copy and its positive GPU completion acknowledgement before retirement. */
        void RequestGpuCompletionRetirement() noexcept;
        /** Requires render-thread GPU-fence waiting for copy completion at an explicit teardown boundary. */
        void RequestSynchronousRetirement() noexcept;
        /** Distinguishes actual GPU-copy completion from resource/callback retirement and image success. */
        bool HasGpuCompletionAcknowledgement() const noexcept;
        /** Requires native object release and destruction of all submitted callbacks. */
        bool IsRetired() const noexcept;
        /** Reports callback ownership that remains after a caller's ordered render fence. */
        bool HasPendingCallbackOwner() const noexcept;
        /** Observes the readback result without advancing or releasing any native ownership. */
        Status GetStatus() const noexcept;
        /** Returns packed RGB rows only after successful readback and full native retirement. */
        const std::vector<std::uint8_t>& Pixels() const noexcept;
        /** Transfers retired packed RGB pixels to a synchronous consumer without retaining another copy. */
        std::vector<std::uint8_t> TakePixels() noexcept;
        /** Releases temporary RGB/state only after native retirement, otherwise preserves all owners. */
        bool Reset() noexcept;

    private:
        /** Shares native ownership with submitted callbacks until their genuine destruction boundary. */
        struct State;
        /** Checks exactly the bindings required by this readback before any copy is accepted. */
        static bool HasNativeBindings();
        /** Performs one copy or GPU completion poll, preserving uncertain native ownership on failure. */
        static void Execute(const std::shared_ptr<State>& state, FRHICommandListImmediate& commandList);
        /** Waits and retires an accepted GPU copy only for explicit lifecycle teardown. */
        static void ExecuteSynchronousRetirement(const std::shared_ptr<State>& state,
            FRHICommandListImmediate& commandList);
        /** Acquires the texture, validates BGRA8/dimensions/samples and constructs one native readback. */
        static void SubmitCopy(const std::shared_ptr<State>& state, FRHICommandListImmediate& commandList);
        /** Applies established shader-resource/copy-source transitions around the native texture copy. */
        static void CopyWithTransitions(const std::shared_ptr<State>& state, FRHICommandListImmediate& commandList);
        /** Maps only after a real nonnull GPU fence acknowledges completion; always unmaps acquired storage. */
        static void CopyCompletedPixels(const std::shared_ptr<State>& state);
        /** Blocks on the accepted GPU copy fence only when synchronous teardown was explicitly requested. */
        static bool WaitForGpuCopy(const std::shared_ptr<State>& state, FRHICommandListImmediate& commandList);
        /** Converts padded native BGRA rows to packed opaque RGB in unchanged top-to-bottom row order. */
        static bool CopyRows(const std::shared_ptr<State>& state, const std::uint8_t* mapped,
            std::int32_t pitchPixels, std::int32_t bufferHeight);
        /** Releases the completed/pre-copy native objects on the render thread and publishes the result. */
        static void Finish(const std::shared_ptr<State>& state, Status result);

        std::shared_ptr<State> state_;
        bool gpuCompletionAcknowledged_ = false;
    };
}
#endif
