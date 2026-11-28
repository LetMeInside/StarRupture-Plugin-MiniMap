// Owns one native terrain capture/readback, temporary RGB and completed canonical BC1 output.
// The initialized resident private world is borrowed; this operation never streams or retires it.
#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "../ITerrainDataProvider.h"
#include "PrivateWorldNative.h"
#include "TerrainImageReadback.h"
#include <cstdint>
#include <span>
#include <string>
#include <vector>
namespace SDK { class UWorld; class AActor; class USceneCaptureComponent2D; class UTextureRenderTarget2D; }
namespace Services::Map::Terrain { class TerrainService; }

namespace Services::Map::Terrain::Capture
{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
    /** Owns one capture's temporary RGB samples until the opt-in diagnostic exporter consumes them. */
    struct TerrainCaptureDiagnosticPixels final
    {
        std::vector<std::uint8_t> FinalRgbPixels;
        std::vector<std::uint8_t> SupersampledRgbPixels;
        std::uint32_t FinalWidthPixels = 0;
        std::uint32_t FinalHeightPixels = 0;
        std::uint32_t SupersampledWidthPixels = 0;
        std::uint32_t SupersampledHeightPixels = 0;
    };
#endif

    /** One explicit game-thread capture, with no acquisition integration or automatic execution.
     * The session owner must keep the world initialized and resident until Retired, including cancellation.
     * Calls must be serialized and nonreentrant. Destruction requires Inactive or acknowledged Retired.
     * Projection depth clipping is requested; renderer culling/Nanite slab isolation is not proven.
     */
    class TerrainImageCapture final
    {
    public:
        /** Separates ordered submission completion from resource cleanup and logical cancellation. */
        enum class Status { Inactive, Preparing, Submitted, ReadingBack, Ready, Retiring, Retired };
        /** Creates inert ownership without native operations. */
        TerrainImageCapture();
        /** Prevents dropping outstanding native owners or fence storage. */
        ~TerrainImageCapture();
        /** Keeps native ownership unique and stationary. */
        TerrainImageCapture(const TerrainImageCapture&) = delete;
        /** Prevents replacing outstanding native resources. */
        TerrainImageCapture& operator=(const TerrainImageCapture&) = delete;
        /** Creates capture resources in an already resident private GamePreview world.
         * False may leave Retiring debt after a native setup failure; continue Advance until Retired.
         * Supports positive world-X image U and positive world-Y image V, matching CaptureBounds XY.
         * Rejects rotated, reflected, sheared or differently framed registration before native allocation.
         */
        bool Submit(SDK::UWorld* residentWorld, const ITerrainDataProvider::CaptureDefinition& definition,
            bool retainDiagnosticPixels);
        /** Advances capture, genuine GPU readback, BC1 encoding and cleanup without native waits/world ticks. */
        void Advance();
        /** Withdraws image/target eligibility immediately; Advance drains readback and ordered cleanup. */
        void RequestRetirement() noexcept;
        /** Blocks only at explicit lifecycle teardown until this capture's native work and cleanup are acknowledged. */
        bool RetireSynchronously();
        /** Ready means validated BC1 after GPU copy completion and readback retirement, not visual fidelity. */
        Status GetStatus() const noexcept { return status_; }
        /** Returns rejection/failure text without implying successful retirement. */
        const std::string& GetReason() const noexcept { return reason_; }
        /** Borrows the target only while Ready. No external native uses may outlive RequestRetirement. */
        SDK::UTextureRenderTarget2D* GetRenderTarget() const noexcept;
        /** Shares completed immutable BC1 ownership only while Ready, without raw/native resources.
         * An issued owner survives operation cleanup. Cancellation withdraws further issuance.
         */
        std::shared_ptr<const TerrainImage> GetImage() const noexcept;

    private:
        friend class ::Services::Map::Terrain::TerrainService;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        friend class PrivateStreamingSession;
#endif
        /** Requires existing bindings, the game thread and no GC, purge or engine exit. */
        static bool CanAdvance();
        /** Requires dimensions suitable for both the final image and its 2x internal render target. */
        static bool GetSupersampledDimensions(const ITerrainDataProvider::CaptureDefinition& definition,
            std::uint32_t& widthPixels, std::uint32_t& heightPixels) noexcept;
        /** Rejects geometry unsupported by this fixed downward view and unrepresentable projection values. */
        static bool IsGeometrySupported(const ITerrainDataProvider::CaptureDefinition& definition);
        /** Box-filters each 2x2 sample group, converting sRGB samples to linear before averaging. */
        static bool DownsampleRgb(std::span<const std::uint8_t> supersampledPixels,
            std::uint32_t supersampledWidth, std::uint32_t supersampledHeight,
            std::uint32_t finalWidth, std::uint32_t finalHeight, bool sRgb,
            std::vector<std::uint8_t>& finalPixels);
        /** Decodes one 8-bit sRGB channel to its linear-light value. */
        static double DecodeSrgbChannel(std::uint8_t encoded) noexcept;
        /** Encodes one linear-light value to its nearest 8-bit sRGB channel. */
        static std::uint8_t EncodeSrgbChannel(double linear) noexcept;
        /** Creates and retains an actor, deferred capture component and native render target. */
        bool CreateResources();
        /** Configures independent rendering, opaque target clear and one-shot final-color output. */
        void ConfigureResources();
        /** Writes the explicit reversed-Z slab matrix and disables automatic plane/view adjustments. */
        void ConfigureProjection();
        /** Applies named view flags through the native setter without retaining SDK array storage. */
        void ConfigureBackground();
        /** Submits the scene once or begins its native BGRA readback after the submission fence. */
        void AdvanceSubmission();
        /** Waits for actual GPU/callback retirement before encoding temporary RGB and publishing BC1. */
        void AdvanceReadback();
        /** Constructs validated immutable output using only the authoritative terrain encoder. */
        bool CompleteImage();
        /** Transfers only the transient raw pixels retained for the enabled diagnostic export. */
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        TerrainCaptureDiagnosticPixels TakeDiagnosticPixels() noexcept;
#endif
        /** Detaches/destroys only capture-owned objects, then fences their deferred render cleanup. */
        void AdvanceRetirement();
        /** Waits for accepted scene rendering and readback GPU work without issuing an image. */
        bool DrainCaptureRenderWork();
        /** Completes existing capture-object cleanup and its ordered render fence. */
        bool FinishSynchronousRetirement();
        /** Balances native object references only after the final cleanup fence completes. */
        void ReleaseResources();

        ITerrainDataProvider::CaptureDefinition definition_;
        PrivateWorldNative native_;
        TerrainImageReadback readback_;
        std::shared_ptr<const TerrainImage> image_;
        SDK::UWorld* world_ = nullptr;
        SDK::AActor* actor_ = nullptr;
        SDK::USceneCaptureComponent2D* component_ = nullptr;
        SDK::UTextureRenderTarget2D* target_ = nullptr;
        Status status_ = Status::Inactive;
        std::string reason_;
        bool registered_ = false;
        bool componentDestroyed_ = false;
        bool actorDestroyAttempted_ = false;
        bool actorDestroyed_ = false;
        bool cleanupSubmitted_ = false;
        bool sceneCaptureAttempted_ = false;
        bool advancing_ = false;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        bool retainDiagnosticPixels_ = false;
        TerrainCaptureDiagnosticPixels diagnosticPixels_;
#endif
        static constexpr std::uint32_t kCaptureSupersamplingFactor = 2;
    };
}
#endif
