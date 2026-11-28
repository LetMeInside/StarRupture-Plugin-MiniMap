// Owns one native terrain capture, its component selection, readback, temporary RGB and canonical BC1 output.
// The initialized resident private world is borrowed; this operation never streams or retires it.
#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "../ITerrainDataProvider.h"
#include "PrivateWorldNative.h"
#include "TerrainImageReadback.h"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
namespace SDK { class UWorld; class AActor; class UStaticMeshComponent; class USceneCaptureComponent2D; class UTextureRenderTarget2D; }
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

    /** One explicit game-thread image operation with a warm-up capture and a final capture.
     * The session owner must keep the world initialized and resident until Retired, including cancellation.
     * Calls must be serialized and nonreentrant. Destruction requires Inactive or acknowledged Retired.
     * Projection depth clipping is requested; renderer culling/Nanite slab isolation is not proven.
     */
    class TerrainImageCapture final
    {
    public:
        /** Separates ordered submission completion from resource cleanup and logical cancellation. */
        enum class Status { Inactive, Preparing, WarmingUp, Submitted, ReadingBack, Ready, Retiring, Retired };
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
         * The session's streaming source actor is borrowed only to omit it from the component inventory.
         */
        bool Submit(SDK::UWorld* residentWorld, const ITerrainDataProvider::CaptureDefinition& definition,
            bool retainDiagnosticPixels, const SDK::AActor* streamingSourceActor);
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
        /** Creates and retains an actor, capture component and native render target. */
        bool CreateResources();
        /** Uses one resident-world inventory for configured exclusions and development diagnostics. */
        bool ApplyComponentConfiguration(const SDK::AActor* streamingSourceActor);
        /** Returns borrowed components from valid resident-level actors, omitting the two infrastructure actors. */
        std::vector<SDK::UActorComponent*> CollectResidentComponents(const SDK::AActor* streamingSourceActor) const;
        /** Adds registered matching mesh components to the capture's weak hidden list. */
        bool ApplyMeshExclusions(std::span<SDK::UActorComponent* const> components);
        /** Compares the component's top-level mesh object path with the configured exact paths. */
        bool MatchesExcludedMesh(const SDK::UStaticMeshComponent* component) const;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        /** Logs inventory size and authored local-light totals, registration and render-state readiness. */
        void ObserveAuthoredLocalLightReadiness(std::span<SDK::UActorComponent* const> components) const;
#endif
        /** Configures independent rendering, opaque target clear and manual final-color output. */
        void ConfigureResources();
        /** Writes the explicit reversed-Z slab matrix and disables automatic plane/view adjustments. */
        void ConfigureProjection();
        /** Applies named view flags through the native setter without retaining SDK array storage. */
        void ConfigureBackground();
        /** Submits the warm-up or final scene, or begins BGRA readback after the final submission fence. */
        void AdvanceSubmission();
        /** Records outstanding scene work and orders the submitted capture behind the native fence. */
        void SubmitSceneCapture();
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
        std::int64_t lastWarmupFrame_ = 0;
        std::uint32_t warmupTicks_ = 0;
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
        static constexpr std::uint32_t kCaptureWarmupTicks = 10;
    };
}
#endif
