// Implements capturer lifecycle, persistent world/light ownership, sprite capture and asynchronous retirement.
#if defined(MODLOADER_CLIENT_BUILD)

#include "BuildingSpriteRenderer.h"

#include "../Cache/SpriteProcessing.h"
#include "../Scheduling/SpriteFeedbackQueue.h"
#include "../Scheduling/SpriteRequestQueue.h"
#include "CaptureFeedback.h"
#include "Native/NativeApi.h"
#include "Native/ResidentMetadata.h"

#include <Engine_classes.hpp>
#include <Engine_parameters.hpp>
#include <Windows.h>

#include <algorithm>
#include <cstddef>
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
#include <chrono>
#endif
#include <cmath>
#include <limits>
#include <string>
#include <span>
#include <type_traits>
#include <utility>

namespace Services::Map::Buildings::Capture
{
	static_assert(sizeof(void*) == 0x08);
	static_assert(offsetof(SDK::UActorComponent, PrimaryComponentTick) == 0x38);
	static_assert(sizeof(decltype(SDK::UActorComponent::PrimaryComponentTick)) == 0x30);
	static_assert(sizeof(decltype(SDK::UActorComponent::bReplicates)) == 0x01);
	static_assert(sizeof(decltype(SDK::UActorComponent::bAutoActivate)) == 0x01);
	static_assert(sizeof(decltype(SDK::UActorComponent::bCanEverAffectNavigation)) == 0x01);
	static_assert(offsetof(SDK::USceneComponent, Mobility) == 0x1A3);
	static_assert(sizeof(decltype(SDK::USceneComponent::Mobility)) == 0x01);
	static_assert(std::is_same_v<std::underlying_type_t<SDK::EComponentMobility>, uint8_t>);
	static_assert(offsetof(SDK::ULightComponentBase, Intensity) == 0x274);
	static_assert(sizeof(decltype(SDK::ULightComponentBase::Intensity)) == sizeof(float));
	static_assert(offsetof(SDK::ULightComponentBase, LightColor) == 0x278);
	static_assert(sizeof(decltype(SDK::ULightComponentBase::LightColor)) == 0x04);
	static_assert(std::is_same_v<decltype(SDK::ULightComponentBase::LightColor), SDK::FColor>);
	static_assert(sizeof(SDK::FColor) == 0x04);
	static_assert(alignof(SDK::FColor) == 0x04);
	static_assert(offsetof(SDK::FColor, B) == 0x00);
	static_assert(offsetof(SDK::FColor, G) == 0x01);
	static_assert(offsetof(SDK::FColor, R) == 0x02);
	static_assert(offsetof(SDK::FColor, A) == 0x03);
	static_assert(sizeof(decltype(SDK::FColor::B)) == sizeof(uint8_t));
	static_assert(sizeof(decltype(SDK::FColor::G)) == sizeof(uint8_t));
	static_assert(sizeof(decltype(SDK::FColor::R)) == sizeof(uint8_t));
	static_assert(sizeof(decltype(SDK::FColor::A)) == sizeof(uint8_t));
	static_assert(sizeof(decltype(SDK::ULightComponent::bUseTemperature)) == 0x01);
	static_assert(sizeof(decltype(SDK::ULightComponentBase::bAffectsWorld)) == 0x01);
	static_assert(sizeof(decltype(SDK::ULightComponentBase::CastShadows)) == 0x01);
	static_assert(sizeof(decltype(SDK::ULightComponentBase::CastStaticShadows)) == 0x01);
	static_assert(sizeof(decltype(SDK::ULightComponentBase::CastDynamicShadows)) == 0x01);
	static_assert(sizeof(decltype(SDK::ULightComponentBase::bAffectTranslucentLighting)) == 0x01);
	static_assert(offsetof(SDK::UDirectionalLightComponent, ForwardShadingPriority) == 0x3FC);
	static_assert(sizeof(decltype(SDK::UDirectionalLightComponent::ForwardShadingPriority)) == sizeof(int32_t));
	static_assert(offsetof(SDK::UDirectionalLightComponent, LightSourceAngle) == 0x400);
	static_assert(sizeof(decltype(SDK::UDirectionalLightComponent::LightSourceAngle)) == sizeof(float));
	static_assert(sizeof(decltype(SDK::UDirectionalLightComponent::bAtmosphereSunLight)) == 0x01);
	static_assert(sizeof(decltype(SDK::UDirectionalLightComponent::bPerPixelAtmosphereTransmittance)) == 0x01);
	static_assert(sizeof(decltype(SDK::UDirectionalLightComponent::bCastShadowsOnClouds)) == 0x01);
	static_assert(sizeof(decltype(SDK::UDirectionalLightComponent::bCastShadowsOnAtmosphere)) == 0x01);
	static_assert(sizeof(decltype(SDK::UDirectionalLightComponent::bCastCloudShadows)) == 0x01);
	static_assert(sizeof(SDK::FTickFunction) == 0x28);
	static_assert(alignof(SDK::FTickFunction) == 0x08);
	static_assert(sizeof(SDK::FActorComponentTickFunction) == 0x30);
	static_assert(alignof(SDK::FActorComponentTickFunction) == 0x08);
	static_assert(sizeof(decltype(SDK::FTickFunction::bCanEverTick)) == 0x01);
	static_assert(offsetof(SDK::USceneCaptureComponent, PrimitiveRenderMode) == 0x250);
	static_assert(sizeof(decltype(SDK::USceneCaptureComponent::PrimitiveRenderMode)) == 0x01);
	static_assert(offsetof(SDK::USceneCaptureComponent, CaptureSource) == 0x251);
	static_assert(sizeof(decltype(SDK::USceneCaptureComponent::CaptureSource)) == 0x01);
	static_assert(std::is_same_v<
		std::underlying_type_t<SDK::ESceneCapturePrimitiveRenderMode>, uint8_t>);
	static_assert(std::is_same_v<std::underlying_type_t<SDK::ESceneCaptureSource>, uint8_t>);
	static_assert(sizeof(decltype(SDK::USceneCaptureComponent::bCaptureEveryFrame)) == 0x01);
	static_assert(sizeof(decltype(SDK::USceneCaptureComponent::bCaptureOnMovement)) == 0x01);
	static_assert(offsetof(SDK::USceneCaptureComponent, bAlwaysPersistRenderingState) == 0x253);
	static_assert(sizeof(decltype(SDK::USceneCaptureComponent::bAlwaysPersistRenderingState)) == 0x01);
	static_assert(offsetof(SDK::USceneCaptureComponent, bUseRayTracingIfEnabled) == 0x2A4);
	static_assert(sizeof(decltype(SDK::USceneCaptureComponent::bUseRayTracingIfEnabled)) == 0x01);
	static_assert(offsetof(SDK::USceneCaptureComponent2D, ProjectionType) == 0x338);
	static_assert(sizeof(decltype(SDK::USceneCaptureComponent2D::ProjectionType)) == 0x01);
	static_assert(std::is_same_v<std::underlying_type_t<SDK::ECameraProjectionMode>, uint8_t>);
	static_assert(offsetof(SDK::USceneCaptureComponent2D, OrthoWidth) == 0x348);
	static_assert(sizeof(decltype(SDK::USceneCaptureComponent2D::OrthoWidth)) == sizeof(float));
	static_assert(offsetof(SDK::USceneCaptureComponent2D, bAutoCalculateOrthoPlanes) == 0x34C);
	static_assert(sizeof(decltype(SDK::USceneCaptureComponent2D::bAutoCalculateOrthoPlanes)) == 0x01);
	static_assert(offsetof(SDK::USceneCaptureComponent2D, bUpdateOrthoPlanes) == 0x354);
	static_assert(sizeof(decltype(SDK::USceneCaptureComponent2D::bUpdateOrthoPlanes)) == 0x01);
	static_assert(offsetof(SDK::USceneCaptureComponent2D, bUseCameraHeightAsViewTarget) == 0x355);
	static_assert(sizeof(decltype(SDK::USceneCaptureComponent2D::bUseCameraHeightAsViewTarget)) == 0x01);
	static_assert(offsetof(SDK::USceneCaptureComponent2D, TextureTarget) == 0x360);
	static_assert(sizeof(decltype(SDK::USceneCaptureComponent2D::TextureTarget)) == sizeof(void*));
	static_assert(std::is_same_v<
		decltype(SDK::USceneCaptureComponent2D::TextureTarget), SDK::UTextureRenderTarget2D*>);
	static_assert(offsetof(SDK::USceneCaptureComponent2D, CompositeMode) == 0x368);
	static_assert(sizeof(decltype(SDK::USceneCaptureComponent2D::CompositeMode)) == 0x01);
	static_assert(std::is_same_v<std::underlying_type_t<SDK::ESceneCaptureCompositeMode>, uint8_t>);
	static_assert(offsetof(SDK::USceneCaptureComponent2D, PostProcessBlendWeight) == 0xAD0);
	static_assert(sizeof(decltype(SDK::USceneCaptureComponent2D::PostProcessBlendWeight)) == sizeof(float));
	static_assert(offsetof(SDK::USceneCaptureComponent2D, bUseCustomProjectionMatrix) == 0xADC);
	static_assert(sizeof(decltype(SDK::USceneCaptureComponent2D::bUseCustomProjectionMatrix)) == 0x01);
	static_assert(offsetof(SDK::USceneCaptureComponent2D, bEnableOrthographicTiling) == 0xB61);
	static_assert(sizeof(decltype(SDK::USceneCaptureComponent2D::bEnableOrthographicTiling)) == 0x01);
	static_assert(offsetof(SDK::USceneCaptureComponent2D, bEnableClipPlane) == 0xB6C);
	static_assert(sizeof(decltype(SDK::USceneCaptureComponent2D::bEnableClipPlane)) == 0x01);
	static_assert(offsetof(SDK::USceneCaptureComponent2D, bRenderInMainRenderer) == 0xBA0);
	static_assert(sizeof(decltype(SDK::USceneCaptureComponent2D::bRenderInMainRenderer)) == 0x01);
	static_assert(sizeof(decltype(SDK::USceneCaptureComponent2D::bMainViewFamily)) == 0x01);
	static_assert(sizeof(decltype(SDK::USceneCaptureComponent2D::bMainViewResolution)) == 0x01);
	static_assert(sizeof(decltype(SDK::USceneCaptureComponent2D::bMainViewCamera)) == 0x01);
	static_assert(sizeof(decltype(
		SDK::USceneCaptureComponent2D::bInheritMainViewCameraPostProcessSettings)) == 0x01);
	static_assert(offsetof(SDK::UTextureRenderTarget, TargetGamma) == 0x140);
	static_assert(sizeof(decltype(SDK::UTextureRenderTarget::TargetGamma)) == sizeof(float));
	static_assert(offsetof(SDK::UTextureRenderTarget2D, ClearColor) == 0x150);
	static_assert(sizeof(decltype(SDK::UTextureRenderTarget2D::ClearColor)) == 0x10);
	static_assert(std::is_same_v<
		decltype(SDK::UTextureRenderTarget2D::ClearColor), SDK::FLinearColor>);
	static_assert(sizeof(decltype(SDK::UTextureRenderTarget2D::bSupportsUAV)) == 0x01);
	static_assert(sizeof(decltype(SDK::UTextureRenderTarget2D::bAutoGenerateMips)) == 0x01);
	static_assert(sizeof(SDK::FLinearColor) == 0x10);
	static_assert(alignof(SDK::FLinearColor) == 0x04);
	static_assert(offsetof(SDK::FLinearColor, R) == 0x00);
	static_assert(offsetof(SDK::FLinearColor, G) == 0x04);
	static_assert(offsetof(SDK::FLinearColor, B) == 0x08);
	static_assert(offsetof(SDK::FLinearColor, A) == 0x0C);
	static_assert(sizeof(decltype(SDK::FLinearColor::R)) == sizeof(float));
	static_assert(sizeof(decltype(SDK::FLinearColor::G)) == sizeof(float));
	static_assert(sizeof(decltype(SDK::FLinearColor::B)) == sizeof(float));
	static_assert(sizeof(decltype(SDK::FLinearColor::A)) == sizeof(float));
	static_assert(sizeof(SDK::FEngineShowFlagsSetting) == 0x18);
	static_assert(alignof(SDK::FEngineShowFlagsSetting) == 0x08);
	static_assert(offsetof(SDK::FEngineShowFlagsSetting, ShowFlagName) == 0x00);
	static_assert(sizeof(decltype(SDK::FEngineShowFlagsSetting::ShowFlagName)) == 0x10);
	static_assert(offsetof(SDK::FEngineShowFlagsSetting, Enabled) == 0x10);
	static_assert(sizeof(decltype(SDK::FEngineShowFlagsSetting::Enabled)) == 0x01);
	static_assert(sizeof(SDK::TArray<SDK::FEngineShowFlagsSetting>) == 0x10);
	static_assert(alignof(SDK::TArray<SDK::FEngineShowFlagsSetting>) == 0x08);
	static_assert(sizeof(SDK::Params::SceneCaptureComponent_SetShowFlagSettings) == 0x10);
	static_assert(offsetof(SDK::Params::SceneCaptureComponent_SetShowFlagSettings,
		InShowFlagSettings) == 0x00);
	static_assert(sizeof(decltype(
		SDK::Params::SceneCaptureComponent_SetShowFlagSettings::InShowFlagSettings)) == 0x10);

	/** Stores sprite capture density without preparing native resources. */
	BuildingSpriteRenderer::BuildingSpriteRenderer(double spriteCaptureDensityPixelsPerMapUnit)
		: spriteCaptureDensityPixelsPerMapUnit_(spriteCaptureDensityPixelsPerMapUnit)
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		ResetDiagnosticsInterval();
#endif
	}

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
	/** Starts one due-interval measurement at the existing method boundary. */
	BuildingSpriteRenderer::DurationScope::DurationScope(
		DiagnosticsSnapshot::Duration& duration) noexcept
		: duration_(duration)
		, started_(std::chrono::steady_clock::now())
	{
	}

	/** Adds elapsed steady-clock time when the measured operation exits. */
	BuildingSpriteRenderer::DurationScope::~DurationScope() noexcept
	{
		const double elapsedMs = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - started_).count();
		duration_.Add(elapsedMs);
	}

	/** Hands off a due interval while leaving active request lifetime state untouched. */
	bool BuildingSpriteRenderer::TakeDiagnosticsSnapshot(DiagnosticsSnapshot& output) noexcept
	{
		const auto now = std::chrono::steady_clock::now();
		const double intervalMs = std::chrono::duration<double, std::milli>(
			now - diagnosticsIntervalStart_).count();
		if (intervalMs < 10000.0 || !HasDiagnosticsActivity())
		{
			return false;
		}
		diagnostics_.IntervalMs = intervalMs;
		output = diagnostics_;
		ResetDiagnosticsInterval();
		return true;
	}

	/** Counts an actual admission result in the existing enum's declaration order. */
	void BuildingSpriteRenderer::RecordAcceptanceStatus(CaptureResult::Acceptance::Status status) noexcept
	{
		++diagnostics_.AcceptanceStatusCounts[static_cast<std::size_t>(status)];
	}

	/** Counts and measures one existing submission for its actual caller purpose. */
	bool BuildingSpriteRenderer::SubmitCaptureSceneForPurpose(CaptureSubmissionPurpose purpose)
	{
		const std::size_t purposeIndex = static_cast<std::size_t>(purpose);
		++diagnostics_.CaptureSubmissionAttempts[purposeIndex];
		const bool submitted = SubmitCaptureScene();
		if (submitted)
		{
			++diagnostics_.CaptureSubmissionSuccesses[purposeIndex];
		}
		return submitted;
	}

	/** Counts an actual readback status in its HDR or final-color category array. */
	void BuildingSpriteRenderer::RecordReadbackStatus(
		bool hdrPass,
		BuildingSpriteReadback::Status status) noexcept
	{
		if (hdrPass)
		{
			++diagnostics_.HdrReadbackStatusCounts[static_cast<std::size_t>(status)];
			return;
		}
		++diagnostics_.FinalReadbackStatusCounts[static_cast<std::size_t>(status)];
	}

	/** Records terminal evidence only after its accepted feedback value enters the FIFO. */
	void BuildingSpriteRenderer::RecordTerminalFeedback(
		CaptureFeedback::Outcome outcome,
		CaptureResult::Completion::Failure failure) noexcept
	{
		++diagnostics_.TerminalFeedbackOutcomeCounts[static_cast<std::size_t>(outcome)];
		if (outcome == CaptureFeedback::Outcome::Failed)
		{
			++diagnostics_.TerminalFailureCounts[static_cast<std::size_t>(failure)];
		}
		if (activeRequestLifetimeActive_)
		{
			const double lifetimeMs = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - activeRequestStartedAt_).count();
			diagnostics_.AcceptedRequestLifetime.Add(lifetimeMs);
		}
		activeRequestStartedAt_ = {};
		activeRequestLifetimeActive_ = false;
	}

	/** Times one existing Start invocation without changing when it is called. */
	bool BuildingSpriteRenderer::StartReadbackForDiagnostics(
		bool hdrPass,
		BuildingSpriteReadback& readback)
	{
		DurationScope durationScope(hdrPass
			? diagnostics_.HdrReadbackStart
			: diagnostics_.FinalReadbackStart);
		return readback.Start(renderTarget_);
	}

	/** Times and categorizes one existing nonblocking Advance invocation. */
	BuildingSpriteReadback::Status BuildingSpriteRenderer::AdvanceReadbackForDiagnostics(
		bool hdrPass,
		BuildingSpriteReadback& readback)
	{
		DurationScope durationScope(hdrPass
			? diagnostics_.HdrReadbackAdvance
			: diagnostics_.FinalReadbackAdvance);
		const BuildingSpriteReadback::Status status = readback.Advance();
		RecordReadbackStatus(hdrPass, status);
		return status;
	}

	/** Clears interval values and starts the next renderer-owned reporting window. */
	void BuildingSpriteRenderer::ResetDiagnosticsInterval() noexcept
	{
		diagnostics_ = {};
		diagnosticsIntervalStart_ = std::chrono::steady_clock::now();
	}

	/** Detects numeric work without retaining request or native-object history. */
	bool BuildingSpriteRenderer::HasDiagnosticsActivity() const noexcept
	{
		if (diagnostics_.AcceptedRequestLifetime.Samples || diagnostics_.ActiveAdvance.Samples ||
			diagnostics_.PrivateWorldPreparation.Samples || diagnostics_.SubjectPreparation.Samples ||
			diagnostics_.CaptureSceneSubmission.Samples || diagnostics_.HdrReadbackStart.Samples ||
			diagnostics_.HdrReadbackAdvance.Samples || diagnostics_.FinalReadbackStart.Samples ||
			diagnostics_.FinalReadbackAdvance.Samples || diagnostics_.HdrReadbackCompletion.Samples ||
			diagnostics_.FinalReadbackCompletion.Samples || diagnostics_.FinalCaptureProcessing.Samples ||
			diagnostics_.SpriteProcessing.Samples || diagnostics_.AdvanceRetirement.Samples ||
			diagnostics_.SubmittedSceneRetirement.Samples || diagnostics_.RequestResourceRetirement.Samples)
		{
			return true;
		}
		for (const auto count : diagnostics_.AcceptanceStatusCounts)
		{
			if (count != 0) return true;
		}
		for (const auto count : diagnostics_.CaptureSubmissionAttempts)
		{
			if (count != 0) return true;
		}
		for (const auto count : diagnostics_.CaptureSubmissionSuccesses)
		{
			if (count != 0) return true;
		}
		for (const auto count : diagnostics_.HdrReadbackStatusCounts)
		{
			if (count != 0) return true;
		}
		for (const auto count : diagnostics_.FinalReadbackStatusCounts)
		{
			if (count != 0) return true;
		}
		for (const auto count : diagnostics_.SpriteProcessingStatusCounts)
		{
			if (count != 0) return true;
		}
		for (const auto count : diagnostics_.TerminalFeedbackOutcomeCounts)
		{
			if (count != 0) return true;
		}
		for (const auto count : diagnostics_.TerminalFailureCounts)
		{
			if (count != 0) return true;
		}
		return false;
	}
#endif

	/** Requests one-time renderer initialization without creating lazy native resources. */
	bool BuildingSpriteRenderer::RequestInitialization() noexcept
	{
		CapturerLifecycleState expected = CapturerLifecycleState::Load;
		if (capturerState_.compare_exchange_strong(expected, CapturerLifecycleState::Init))
		{
			return true;
		}
		return expected == CapturerLifecycleState::Init || expected == CapturerLifecycleState::Start;
	}

	/** Requests Start only after initialization or when the same Start is already pending. */
	bool BuildingSpriteRenderer::RequestStart() noexcept
	{
		CapturerLifecycleState requestedState = capturerState_.load();
		while (requestedState == CapturerLifecycleState::Init ||
			requestedState == CapturerLifecycleState::Start)
		{
			if (requestedState == CapturerLifecycleState::Start)
			{
				return true;
			}
			if (capturerState_.compare_exchange_weak(requestedState, CapturerLifecycleState::Start))
			{
				return true;
			}
		}
		return false;
	}

	/** Requests Stop without advancing native retirement on the caller. */
	void BuildingSpriteRenderer::RequestStop() noexcept
	{
		CapturerLifecycleState requestedState = capturerState_.load();
		while (requestedState != CapturerLifecycleState::Exit &&
			requestedState != CapturerLifecycleState::Load &&
			requestedState != CapturerLifecycleState::Stop)
		{
			if (capturerState_.compare_exchange_weak(requestedState, CapturerLifecycleState::Stop))
			{
				return;
			}
		}
	}

	/** Permanently requests Exit, which takes priority over Start and Stop requests. */
	void BuildingSpriteRenderer::RequestExit() noexcept
	{
		capturerState_.store(CapturerLifecycleState::Exit);
	}

	/** Processes one requested lifecycle transition on the Unreal game thread. */
	void BuildingSpriteRenderer::ProcessLifecycle()
	{
		if (!IsGameThread())
		{
			return;
		}
		AdvanceLifecycleState(capturerState_.load());
	}

	/** Keeps request admission closed until the active request has safely retired. */
	void BuildingSpriteRenderer::AdvanceLifecycleState(CapturerLifecycleState requestedState)
	{
		if (requestedState == CapturerLifecycleState::Load ||
			capturerSubState_ == CapturerSubState::ExitReady)
		{
			return;
		}
		if (requestedState == CapturerLifecycleState::Init)
		{
			capturerSubState_ = CapturerSubState::InitCompleted;
			return;
		}
		if (requestedState == CapturerLifecycleState::Start)
		{
			AdvanceStartLifecycle();
			return;
		}
		if (requestedState == CapturerLifecycleState::Stop)
		{
			AdvanceStopLifecycle();
			return;
		}
		AdvanceExitLifecycle();
	}

	/** Activates the persistent renderer after its one-time initialization state. */
	void BuildingSpriteRenderer::AdvanceStartLifecycle()
	{
		if (capturerSubState_ == CapturerSubState::None)
		{
			capturerSubState_ = CapturerSubState::InitCompleted;
		}
		if (capturerSubState_ == CapturerSubState::InitCompleted ||
			capturerSubState_ == CapturerSubState::StopCompleted)
		{
			capturerSubState_ = CapturerSubState::Active;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			ResetDiagnosticsInterval();
#endif
		}
	}

	/** Retires session request ownership and acknowledges completed Stop. */
	void BuildingSpriteRenderer::AdvanceStopLifecycle()
	{
		if (capturerSubState_ != CapturerSubState::StopReleasing)
		{
			capturerSubState_ = CapturerSubState::StopReleasing;
			if (hasActiveRequest_)
			{
				Cancel(activeRequest_.RequestIdentity);
			}
		}
		AdvanceLifecycleRetirement();
		if (!hasActiveRequest_)
		{
			capturerSubState_ = CapturerSubState::StopCompleted;
			CapturerLifecycleState expected = CapturerLifecycleState::Stop;
			capturerState_.compare_exchange_strong(expected, CapturerLifecycleState::Init);
		}
	}

	/** Retires request ownership and releases persistent native objects for Exit. */
	void BuildingSpriteRenderer::AdvanceExitLifecycle()
	{
		if (capturerSubState_ != CapturerSubState::ExitCleanup)
		{
			capturerSubState_ = CapturerSubState::ExitCleanup;
			if (hasActiveRequest_)
			{
				Cancel(activeRequest_.RequestIdentity);
			}
		}
		AdvanceLifecycleRetirement();
		if (!hasActiveRequest_ && CleanupPersistentWorld())
		{
			capturerSubState_ = CapturerSubState::ExitReady;
		}
	}

	/** Validates one request and moves it into the renderer's existing retained request state. */
	CaptureResult::Acceptance BuildingSpriteRenderer::AcceptOwned(CaptureRequest&& request)
	{
		CaptureResult::Acceptance acceptance;
		acceptance.Correlation = request.RequestIdentity;
		if (capturerState_.load() != CapturerLifecycleState::Start ||
			capturerSubState_ != CapturerSubState::Active || hasActiveRequest_ || !sharedWorldUsable_)
		{
			acceptance.Result = CaptureResult::Acceptance::Status::RejectedUnavailable;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			RecordAcceptanceStatus(acceptance.Result);
#endif
			return acceptance;
		}
		std::string spriteFileName;
		if (!request.SpriteRequirement.TryGetFileName(spriteFileName))
		{
			acceptance.Result = CaptureResult::Acceptance::Status::RejectedInvalidRequest;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			RecordAcceptanceStatus(acceptance.Result);
#endif
			return acceptance;
		}
		if (request.RequestIdentity.RequestSequence == 0)
		{
			acceptance.Result = CaptureResult::Acceptance::Status::RejectedInvalidRequest;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			RecordAcceptanceStatus(acceptance.Result);
#endif
			return acceptance;
		}
		if (request.RequestIdentity.RequestSequence <= lastAcceptedRequestSequence_)
		{
			acceptance.Result = CaptureResult::Acceptance::Status::RejectedStaleRequest;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			RecordAcceptanceStatus(acceptance.Result);
#endif
			return acceptance;
		}
		RetainAcceptedRequest(std::move(request));
		acceptance.Result = CaptureResult::Acceptance::Status::Accepted;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		RecordAcceptanceStatus(acceptance.Result);
#endif
		return acceptance;
	}

	/** Moves one validated request into the existing private active slot and resets request state. */
	void BuildingSpriteRenderer::RetainAcceptedRequest(CaptureRequest&& request)
	{
		activeRequest_ = std::move(request);
		completion_ = {};
		pendingFailure_ = CaptureResult::Completion::Failure::None;
		completion_.Correlation = activeRequest_.RequestIdentity;
		completion_.Result = CaptureResult::Completion::Status::Pending;
		phase_ = Phase::PreparingWorld;
		hasActiveRequest_ = true;
		lastAcceptedRequestSequence_ = activeRequest_.RequestIdentity.RequestSequence;
		resultEligible_ = true;
		finalColorPassSubmitted_ = false;
		readbackStarted_ = false;
		requestSceneRetirementFenceComplete_ = false;
		pendingPayload_.reset();
		hdrPixels_.clear();
		preparedIteration_ = 0;
		hdrWarmupIteration_ = 0;
		requestRetirementFenceComplete_ = false;
		captureComponentRegistered_ = false;
		captureComponentDestroyed_ = false;
		subjectActorDestroyed_ = false;
		actorDestroyFailed_ = false;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		activeRequestStartedAt_ = std::chrono::steady_clock::now();
		activeRequestLifetimeActive_ = true;
#endif
	}

	/** Advances active work or admits exactly one request without holding a queue lock in capture code. */
	void BuildingSpriteRenderer::AdvanceQueueDrivenCapture(
		Scheduling::SpriteRequestQueue& requestQueue,
		Scheduling::SpriteFeedbackQueue& feedbackQueue)
	{
		lifecycleFeedbackQueue_ = &feedbackQueue;
		if (!IsGameThread())
		{
			return;
		}
		if (capturerState_.load() != CapturerLifecycleState::Start ||
			capturerSubState_ != CapturerSubState::Active)
		{
			return;
		}
		if (hasActiveRequest_)
		{
			Advance();
			EmitTerminalFeedback(feedbackQueue);
			return;
		}
		if (!CanAdmitQueuedRequest())
		{
			return;
		}

		CaptureRequest request;
		if (!requestQueue.TryDequeue(request))
		{
			return;
		}

		const CaptureResult::Acceptance acceptance = AcceptOwned(std::move(request));
		if (acceptance.Result != CaptureResult::Acceptance::Status::Accepted)
		{
			EmitRejectedRequestFeedback(std::move(request), acceptance, feedbackQueue);
		}
	}

	/** Requires idle, game-thread admission with no outstanding native request retirement. */
	bool BuildingSpriteRenderer::CanAdmitQueuedRequest() const
	{
		return !hasActiveRequest_ && phase_ == Phase::Idle &&
			sharedWorldUsable_ && !sceneWorkSubmitted_ && !readbackStarted_ &&
			fencePurpose_ == FencePurpose::None && hdrReadback_.IsRetired() && finalReadback_.IsRetired() &&
			!actorClass_ && !subjectActor_ && !captureComponent_ && !renderTarget_;
	}

	/** Converts a rejected dequeued request into one terminal owned feedback value.
	 * @param request Rejected request whose owned key and correlation enter the result.
	 * @param acceptance Existing reason the renderer did not retain the request.
	 * @param feedbackQueue Queue that takes ownership of the rejection result.
	 */
	void BuildingSpriteRenderer::EmitRejectedRequestFeedback(
		CaptureRequest&& request,
		const CaptureResult::Acceptance& acceptance,
		Scheduling::SpriteFeedbackQueue& feedbackQueue)
	{
		CaptureFeedback feedback;
		feedback.SpriteRequirement = std::move(request.SpriteRequirement);
		feedback.RequestIdentity = request.RequestIdentity;
		switch (acceptance.Result)
		{
		case CaptureResult::Acceptance::Status::RejectedStaleRequest:
			feedback.Result = CaptureFeedback::Outcome::Failed;
			feedback.Error = CaptureResult::Completion::Failure::StaleRequest;
			break;
		case CaptureResult::Acceptance::Status::RejectedUnavailable:
			feedback.Result = CaptureFeedback::Outcome::Failed;
			feedback.Error = CaptureResult::Completion::Failure::EngineOperationFailed;
			break;
		case CaptureResult::Acceptance::Status::RejectedInvalidRequest:
			feedback.Result = CaptureFeedback::Outcome::Failed;
			feedback.Error = CaptureResult::Completion::Failure::InvalidCapture;
			break;
		case CaptureResult::Acceptance::Status::Accepted:
			return;
		}
		feedbackQueue.Enqueue(std::move(feedback));
	}

	/** Advances world preparation, subject setup, or one admitted capture submission. */
	void BuildingSpriteRenderer::Advance()
	{
		if (!hasActiveRequest_ || !IsGameThread() || phase_ == Phase::Terminal)
		{
			return;
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		DurationScope durationScope(diagnostics_.ActiveAdvance);
#endif
		if (phase_ == Phase::RetirementRequired)
		{
			AdvanceRetirement();
			return;
		}
		if (admittedServiceIterations_ == (std::numeric_limits<std::uint64_t>::max)())
		{
			FailRequest(CaptureResult::Completion::Failure::EngineOperationFailed);
			return;
		}
		++admittedServiceIterations_;
		AdvanceCurrentPhase();
	}

	/** Retries pre-submission cleanup and records cancellation or failure after release. */
	void BuildingSpriteRenderer::AdvanceRetirement()
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		DurationScope durationScope(diagnostics_.AdvanceRetirement);
#endif
		if (sceneWorkSubmitted_ && !RetireSubmittedSceneBehindFence())
		{
			return;
		}
		if (!RetireRequestResources())
		{
			return;
		}
		completion_.Error = pendingFailure_;
		if (pendingFailure_ != CaptureResult::Completion::Failure::None)
		{
			completion_.Result = CaptureResult::Completion::Status::Failed;
		}
		else if (resultEligible_ && pendingPayload_)
		{
			completion_.Payload = std::move(pendingPayload_);
			completion_.Result = CaptureResult::Completion::Status::Completed;
		}
		else
		{
			completion_.Result = CaptureResult::Completion::Status::Cancelled;
		}
		phase_ = Phase::Terminal;
	}

	/** Advances the current engine setup or asynchronous capture phase. */
	void BuildingSpriteRenderer::AdvanceCurrentPhase()
	{
		if (phase_ == Phase::PreparingWorld)
		{
			PreparePrivateWorld();
			return;
		}
		if (phase_ == Phase::PreparingSubject)
		{
			PrepareSubject();
			return;
		}
		if (phase_ == Phase::WaitingForHdrWarmup)
		{
			SubmitHdrWarmupWhenAdmitted();
			return;
		}
		if (phase_ == Phase::HdrWarmupSubmitted)
		{
			SubmitHdrCaptureAfterWarmup();
			return;
		}
		if (phase_ == Phase::WaitingForFinalColor)
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			if (!SubmitCaptureSceneForPurpose(CaptureSubmissionPurpose::FinalColor))
#else
			if (!SubmitCaptureScene())
#endif
			{
				FailRequest(CaptureResult::Completion::Failure::EngineOperationFailed);
				return;
			}
			finalColorPassSubmitted_ = true;
			phase_ = Phase::AwaitingFinalReadback;
			return;
		}
		if (phase_ == Phase::AwaitingHdrReadback || phase_ == Phase::AwaitingFinalReadback)
		{
			AdvanceReadbackPhase();
		}
	}

	/** Cancels only the correlated request and keeps submitted resources until BM-07 retires them. */
	void BuildingSpriteRenderer::Cancel(CaptureRequest::Correlation correlation)
	{
		if (!hasActiveRequest_ ||
			correlation.RequestSequence != activeRequest_.RequestIdentity.RequestSequence)
		{
			return;
		}
		if (phase_ == Phase::Terminal)
		{
			resultEligible_ = false;
			if (completion_.Result == CaptureResult::Completion::Status::Completed)
			{
				completion_.Payload.reset();
				completion_.Result = CaptureResult::Completion::Status::Cancelled;
			}
			return;
		}
		resultEligible_ = false;
		pendingFailure_ = CaptureResult::Completion::Failure::None;
		pendingPayload_.reset();
		if (sceneWorkSubmitted_)
		{
			phase_ = Phase::RetirementRequired;
			return;
		}
		if (RetireRequestResources())
		{
			completion_.Result = CaptureResult::Completion::Status::Cancelled;
			phase_ = Phase::Terminal;
		}
		else
		{
			phase_ = Phase::RetirementRequired;
		}
	}

	/** Advances active-request cancellation and reports terminal feedback through the existing FIFO. */
	void BuildingSpriteRenderer::AdvanceLifecycleRetirement()
	{
		if (!hasActiveRequest_)
		{
			return;
		}
		Advance();
		if (lifecycleFeedbackQueue_ != nullptr)
		{
			EmitTerminalFeedback(*lifecycleFeedbackQueue_);
		}
	}

	/** Reports whether the matching terminal request and every native owner are safely retired. */
	bool BuildingSpriteRenderer::CanAcknowledgeCompletion(CaptureRequest::Correlation correlation) const
	{
		return hasActiveRequest_ && phase_ == Phase::Terminal &&
			completion_.Result != CaptureResult::Completion::Status::Pending &&
			correlation.RequestSequence == activeRequest_.RequestIdentity.RequestSequence &&
			correlation.RequestSequence == completion_.Correlation.RequestSequence &&
			!sceneWorkSubmitted_ && !readbackStarted_ && hdrReadback_.IsRetired() && finalReadback_.IsRetired() &&
			!subjectActor_ && !captureComponent_ && !renderTarget_ && !actorClass_ &&
			fencePurpose_ == FencePurpose::None;
	}

	/** Releases a fully retired terminal result after correlation and resource checks pass. */
	void BuildingSpriteRenderer::AcknowledgeCompletion(CaptureRequest::Correlation correlation)
	{
		if (!CanAcknowledgeCompletion(correlation))
		{
			return;
		}
		hdrReadback_.Reset();
		finalReadback_.Reset();
		hdrPixels_.clear();
		pendingPayload_.reset();
		hasActiveRequest_ = false;
		activeRequest_ = {};
		completion_ = {};
		phase_ = Phase::Idle;
	}

	/** Enqueues one terminal feedback value only after native request retirement is complete. */
	void BuildingSpriteRenderer::EmitTerminalFeedback(Scheduling::SpriteFeedbackQueue& feedbackQueue)
	{
		const CaptureRequest::Correlation correlation = activeRequest_.RequestIdentity;
		if (!CanAcknowledgeCompletion(correlation))
		{
			return;
		}

		CaptureFeedback feedback;
		feedback.SpriteRequirement = std::move(activeRequest_.SpriteRequirement);
		feedback.RequestIdentity = correlation;
		if (completion_.Result == CaptureResult::Completion::Status::Cancelled)
		{
			feedback.Result = CaptureFeedback::Outcome::Cancelled;
		}
		else if (completion_.Result == CaptureResult::Completion::Status::Failed)
		{
			feedback.Result = CaptureFeedback::Outcome::Failed;
			feedback.Error = completion_.Error;
		}
		else if (completion_.Result == CaptureResult::Completion::Status::Completed && !resultEligible_)
		{
			feedback.Result = CaptureFeedback::Outcome::Cancelled;
		}
		else if (completion_.Result == CaptureResult::Completion::Status::Completed && completion_.Payload)
		{
			feedback.Result = CaptureFeedback::Outcome::Succeeded;
			feedback.Payload = std::move(completion_.Payload);
		}
		else if (completion_.Result == CaptureResult::Completion::Status::Completed)
		{
			feedback.Result = CaptureFeedback::Outcome::Failed;
			feedback.Error = CaptureResult::Completion::Failure::InvalidCapture;
		}
		else
		{
			return;
		}

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const CaptureFeedback::Outcome terminalOutcome = feedback.Result;
		const CaptureResult::Completion::Failure terminalFailure = feedback.Error;
#endif
		feedbackQueue.Enqueue(std::move(feedback));
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		RecordTerminalFeedback(terminalOutcome, terminalFailure);
#endif
		AcknowledgeCompletion(correlation);
	}

	/** Advances HDR cancellation or final-color setup only after BM-07 confirms retirement. */
	bool BuildingSpriteRenderer::AcknowledgeHdrPassRetirement(
		bool readbackWasSubmitted,
		bool readbackAndSceneRetired)
	{
		const bool hdrReadbackExpected = phase_ == Phase::AwaitingHdrReadback;
		if (!readbackAndSceneRetired || !sceneWorkSubmitted_ || !IsGameThread() ||
			(hdrReadbackExpected && !readbackWasSubmitted) ||
			(!hdrReadbackExpected && (phase_ != Phase::RetirementRequired || resultEligible_)))
		{
			return false;
		}
		sceneWorkSubmitted_ = false;
		if (!resultEligible_)
		{
			if (!RetireRequestResources())
			{
				return false;
			}
			completion_.Result = CaptureResult::Completion::Status::Cancelled;
			phase_ = Phase::Terminal;
			return true;
		}
		const auto* nativeApi = MiniMapNative::Get();
		if (!nativeApi || !nativeApi->texture.initRenderTarget || !ConfigureCapture(true))
		{
			FailRequest(CaptureResult::Completion::Failure::EngineOperationFailed);
			return false;
		}
		nativeApi->texture.initRenderTarget(renderTarget_, 256, 256, 10, true);
		phase_ = Phase::WaitingForFinalColor;
		return true;
	}

	/** Converts the retained center and extent into owned capture-space XYZ bounds. */
	Visuals::SpriteCaptureBounds BuildingSpriteRenderer::GetCaptureBounds() const
	{
		return {
			{boundsCenter_[0] - boundsExtent_[0], boundsCenter_[1] - boundsExtent_[1], boundsCenter_[2] - boundsExtent_[2]},
			{boundsCenter_[0] + boundsExtent_[0], boundsCenter_[1] + boundsExtent_[1], boundsCenter_[2] + boundsExtent_[2]}};
	}

	/** Resolves and roots the generated class represented by the copied object path. */
	bool BuildingSpriteRenderer::ResolveAndRetainActorClass(std::string_view objectPath)
	{
		std::wstring widePath;
		if (!ConvertObjectPath(objectPath, widePath))
		{
			return false;
		}
		actorClass_ = FindResidentClass(widePath);
		if (!actorClass_)
		{
			return false;
		}
		const auto* nativeApi = MiniMapNative::Get();
		if (!nativeApi || !nativeApi->asset.retainObject)
		{
			actorClass_ = nullptr;
			return false;
		}
		nativeApi->asset.retainObject(actorClass_);
		SDK::UClass* actorBaseClass = FindResidentClass(L"/Script/Engine.Actor");
		return actorBaseClass && SDK::UKismetMathLibrary::ClassIsChildOf(
			SDK::TSubclassOf<SDK::UObject>(actorClass_), SDK::TSubclassOf<SDK::UObject>(actorBaseClass));
	}

	/** Lazily creates, initializes, and retains the shared private world through NativeApi. */
	bool BuildingSpriteRenderer::CreatePrivateWorld()
	{
		const auto* nativeApi = MiniMapNative::Get();
		if (!nativeApi || !MiniMapNative::IsInitialized() || !nativeApi->world.createWorld ||
			!nativeApi->world.initWorld || !nativeApi->world.updateWorldComponents ||
			!nativeApi->world.cleanupWorld || !nativeApi->asset.retainObject ||
			!nativeApi->asset.releaseObject || !nativeApi->texture.renderFenceConstruct ||
			!nativeApi->texture.renderFenceBegin || !nativeApi->texture.renderFencePoll ||
			!nativeApi->texture.renderFenceDestroy || !IsGameThread())
		{
			return false;
		}
		MiniMapNative::WorldInitializationValues initializationValues;
		const std::uintptr_t world = nativeApi->world.createWorld(
			5, false, SDK::FName{}, 0, false, 5, &initializationValues, true);
		if (!world)
		{
			return false;
		}
		privateWorld_ = reinterpret_cast<SDK::UWorld*>(world);
		privateWorldCleaned_ = false;
		nativeApi->asset.retainObject(privateWorld_);
		nativeApi->world.initWorld(world, initializationValues);
		nativeApi->world.updateWorldComponents(world, false, false, nullptr);
		privateWorldInitialized_ = true;
		return true;
	}

	/** Creates the two shared lights once and waits asynchronously for their render state. */
	bool BuildingSpriteRenderer::EnsureLightingRigReady()
	{
		if (lightingRigReady_)
		{
			return true;
		}
		if (!lightingRigSetupStarted_)
		{
			lightingRigSetupStarted_ = true;
			if (!CreateDirectionalLight(0) || !CreateDirectionalLight(1) ||
				!StartFence(FencePurpose::LightingReadiness))
			{
				sharedWorldUsable_ = false;
				return false;
			}
			return true;
		}
		if (fencePurpose_ != FencePurpose::LightingReadiness)
		{
			return false;
		}
		if (!PollFence(FencePurpose::LightingReadiness))
		{
			return true;
		}
		lightingRigReady_ = true;
		return true;
	}

	/** Creates the requested actor through deferred spawn, finishes construction, and records bounds. */
	bool BuildingSpriteRenderer::SpawnSubjectAndAcquireBounds()
	{
		if (!privateWorld_ || !actorClass_ || !IsGameThread())
		{
			return false;
		}
		SDK::FTransform transform{};
		transform.Rotation.W = 1.0;
		transform.Scale3D = SDK::FVector(1.0, 1.0, 1.0);
		SDK::AActor* deferredActor = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
			reinterpret_cast<SDK::UObject*>(privateWorld_), SDK::TSubclassOf<SDK::AActor>(actorClass_),
			transform, SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn, nullptr,
			SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
		if (!deferredActor)
		{
			return false;
		}
		subjectActor_ = deferredActor;
		const auto* nativeApi = MiniMapNative::Get();
		if (!nativeApi || !nativeApi->asset.retainObject)
		{
			return false;
		}
		nativeApi->asset.retainObject(subjectActor_);
		if (SDK::UGameplayStatics::FinishSpawningActor(
			subjectActor_, transform, SDK::ESpawnActorScaleMethod::MultiplyWithRoot) != subjectActor_)
		{
			return false;
		}
		return AcquireSubjectBounds();
	}

	/** Stores finite subject bounds or the established fallback values for invalid bounds. */
	bool BuildingSpriteRenderer::AcquireSubjectBounds()
	{
		SDK::FVector center;
		SDK::FVector extent;
		SDK::UKismetSystemLibrary::GetActorBounds(subjectActor_, &center, &extent);
		const float orthographicWidth = float(2.0 * (std::max)(extent.X, extent.Y) * 1.1);
		const bool validBounds = std::isfinite(center.X) && std::isfinite(center.Y) && std::isfinite(center.Z) &&
			std::isfinite(extent.X) && std::isfinite(extent.Y) && std::isfinite(extent.Z) &&
			extent.X > 0.0 && extent.Y > 0.0 && extent.Z >= 0.0 &&
			std::isfinite(center.Z + extent.Z + 1000.0) && std::isfinite(orthographicWidth) && orthographicWidth > 0.0f;
		if (!validBounds)
		{
			center = SDK::FVector(0, 0, 0);
			extent = SDK::FVector(500, 500, 500);
		}
		boundsCenter_[0] = center.X;
		boundsCenter_[1] = center.Y;
		boundsCenter_[2] = center.Z;
		boundsExtent_[0] = extent.X;
		boundsExtent_[1] = extent.Y;
		boundsExtent_[2] = extent.Z;
		return true;
	}

	/** Creates request-owned capture component and 256-square float target. */
	bool BuildingSpriteRenderer::CreateCaptureResources()
	{
		SDK::UClass* captureClass = FindResidentClass(L"/Script/Engine.SceneCaptureComponent2D");
		SDK::UClass* targetClass = FindResidentClass(L"/Script/Engine.TextureRenderTarget2D");
		if (!captureClass || !targetClass)
		{
			return false;
		}
		captureComponent_ = reinterpret_cast<SDK::USceneCaptureComponent2D*>(
			CreateRetainedObject(captureClass, reinterpret_cast<SDK::UObject*>(privateWorld_)));
		renderTarget_ = reinterpret_cast<SDK::UTextureRenderTarget2D*>(
			CreateRetainedObject(targetClass, reinterpret_cast<SDK::UObject*>(privateWorld_)));
		if (!captureComponent_ || !renderTarget_ || !ConfigureCapture(false))
		{
			return false;
		}
		const auto* nativeApi = MiniMapNative::Get();
		if (!nativeApi || !nativeApi->texture.initRenderTarget || !nativeApi->player.registerComponent)
		{
			return false;
		}
		nativeApi->texture.initRenderTarget(renderTarget_, 256, 256, 10, true);
		captureComponent_->TextureTarget = renderTarget_;
		captureComponent_->K2_SetWorldLocationAndRotation(
			SDK::FVector(boundsCenter_[0], boundsCenter_[1], boundsCenter_[2] + boundsExtent_[2] + 1000.0),
			SDK::FRotator(-90.0, -90.0, 0.0), false, nullptr, true);
		nativeApi->player.registerComponent(captureComponent_, privateWorld_, nullptr);
		captureComponentRegistered_ = true;
		return true;
	}

	/** Applies HDR inverse-opacity or final display-color capture settings and show flags. */
	bool BuildingSpriteRenderer::ConfigureCapture(bool finalColor)
	{
		if (!captureComponent_ || !renderTarget_)
		{
			return false;
		}
		captureComponent_->PrimaryComponentTick.bCanEverTick = false;
		captureComponent_->bReplicates = false;
		captureComponent_->bAutoActivate = false;
		captureComponent_->bCanEverAffectNavigation = false;
		captureComponent_->bCaptureEveryFrame = false;
		captureComponent_->bCaptureOnMovement = false;
		captureComponent_->bAlwaysPersistRenderingState = false;
		captureComponent_->bUseRayTracingIfEnabled = false;
		captureComponent_->ProjectionType = SDK::ECameraProjectionMode::Orthographic;
		captureComponent_->PrimitiveRenderMode = SDK::ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;
		captureComponent_->CaptureSource = finalColor ? SDK::ESceneCaptureSource::SCS_FinalColorLDR : SDK::ESceneCaptureSource::SCS_SceneColorHDR;
		captureComponent_->CompositeMode = SDK::ESceneCaptureCompositeMode::SCCM_Overwrite;
		captureComponent_->OrthoWidth = float(2.0 * (std::max)(boundsExtent_[0], boundsExtent_[1]) * 1.1);
		captureComponent_->bAutoCalculateOrthoPlanes = true;
		captureComponent_->bUpdateOrthoPlanes = true;
		captureComponent_->bUseCameraHeightAsViewTarget = false;
		captureComponent_->bEnableOrthographicTiling = false;
		captureComponent_->bRenderInMainRenderer = false;
		captureComponent_->bMainViewFamily = false;
		captureComponent_->bMainViewResolution = false;
		captureComponent_->bMainViewCamera = false;
		captureComponent_->bInheritMainViewCameraPostProcessSettings = false;
		captureComponent_->PostProcessBlendWeight = 0.0;
		captureComponent_->bEnableClipPlane = false;
		captureComponent_->bUseCustomProjectionMatrix = false;
		return ConfigureShowFlagsAndTarget(finalColor);
	}

	/** Keeps the existing wrapper name while retiring only request-owned resources. */
	bool BuildingSpriteRenderer::RetireWithoutSubmittedScene()
	{
		return RetireRequestResources();
	}

	/** Unregisters and destroys the actor/capture resources for the active request. */
	bool BuildingSpriteRenderer::DestroyRequestContents()
	{
		const auto* nativeApi = MiniMapNative::Get();
		if (captureComponent_ && captureComponentRegistered_)
		{
			if (!nativeApi || !nativeApi->player.unregisterComponent)
			{
				return false;
			}
			nativeApi->player.unregisterComponent(captureComponent_);
			captureComponentRegistered_ = false;
		}
		if (captureComponent_ && !captureComponentDestroyed_)
		{
			if (!DestroyComponent(captureComponent_)) return false;
			captureComponentDestroyed_ = true;
		}
		if (subjectActor_ && !subjectActorDestroyed_)
		{
			if (!DestroyActor(subjectActor_)) return false;
			subjectActorDestroyed_ = true;
		}
		return true;
	}

	/** Retires request owners behind a fence without destroying the shared world or lights. */
	bool BuildingSpriteRenderer::RetireRequestResources()
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		DurationScope durationScope(diagnostics_.RequestResourceRetirement);
#endif
		if (sceneWorkSubmitted_ || !IsGameThread()) return false;
		if (fencePurpose_ == FencePurpose::LightingReadiness)
		{
			if (!PollFence(FencePurpose::LightingReadiness)) return false;
			lightingRigReady_ = true;
		}
		if (fencePurpose_ != FencePurpose::None && fencePurpose_ != FencePurpose::RequestRetirement)
		{
			return false;
		}
		if (!DestroyRequestContents()) return false;
		const bool requestRenderResources = subjectActor_ || captureComponent_ || renderTarget_;
		if (requestRenderResources && !requestRetirementFenceComplete_)
		{
			if (fencePurpose_ == FencePurpose::None && !StartFence(FencePurpose::RequestRetirement)) return false;
			if (!PollFence(FencePurpose::RequestRetirement)) return false;
			requestRetirementFenceComplete_ = true;
		}
		return ReleaseRequestRoots();
	}

	/** Releases request-specific retained objects after their fence and owner teardown. */
	bool BuildingSpriteRenderer::ReleaseRequestRoots()
	{
		if (!ReleaseObject(reinterpret_cast<SDK::UObject*>(renderTarget_))) return false;
		renderTarget_ = nullptr;
		if (!ReleaseObject(reinterpret_cast<SDK::UObject*>(captureComponent_))) return false;
		captureComponent_ = nullptr;
		if (!ReleaseObject(reinterpret_cast<SDK::UObject*>(subjectActor_))) return false;
		subjectActor_ = nullptr;
		if (!ReleaseObject(reinterpret_cast<SDK::UObject*>(actorClass_))) return false;
		actorClass_ = nullptr;
		return true;
	}

	/** Constructs and inserts one purpose-tagged render fence using existing NativeApi calls. */
	bool BuildingSpriteRenderer::StartFence(FencePurpose purpose)
	{
		const auto* nativeApi = MiniMapNative::Get();
		if (purpose == FencePurpose::None || fencePurpose_ != FencePurpose::None || !nativeApi ||
			!nativeApi->texture.renderFenceConstruct || !nativeApi->texture.renderFenceBegin || !IsGameThread())
		{
			return false;
		}
		fencePurpose_ = purpose;
		nativeApi->texture.renderFenceConstruct(retirementFenceStorage_);
		nativeApi->texture.renderFenceBegin(retirementFenceStorage_, 1);
		return true;
	}

	/** Polls without blocking and destroys the fence only after its ordered work completes. */
	bool BuildingSpriteRenderer::PollFence(FencePurpose purpose)
	{
		const auto* nativeApi = MiniMapNative::Get();
		if (purpose == FencePurpose::None || fencePurpose_ != purpose || !nativeApi ||
			!nativeApi->texture.renderFencePoll || !nativeApi->texture.renderFenceDestroy || !IsGameThread())
		{
			return false;
		}
		if (!nativeApi->texture.renderFencePoll(retirementFenceStorage_)) return false;
		nativeApi->texture.renderFenceDestroy(retirementFenceStorage_);
		fencePurpose_ = FencePurpose::None;
		return true;
	}

	/** Releases one retained UObject after its dependent Unreal operation has completed. */
	bool BuildingSpriteRenderer::ReleaseObject(SDK::UObject* object)
	{
		if (!object)
		{
			return true;
		}
		const auto* nativeApi = MiniMapNative::Get();
		if (!nativeApi || !nativeApi->asset.releaseObject || !IsGameThread())
		{
			return false;
		}
		nativeApi->asset.releaseObject(object);
		return true;
	}

	/** Checks that existing NativeApi facilities are initialized on the Unreal game thread. */
	bool BuildingSpriteRenderer::IsGameThread() const
	{
		const auto* nativeApi = MiniMapNative::Get();
		return MiniMapNative::IsInitialized() && nativeApi && nativeApi->player.isGameThread &&
			nativeApi->player.isGameThread();
	}

	/** Converts a copied UTF-8 path without retaining caller storage or accepting embedded NULs. */
	bool BuildingSpriteRenderer::ConvertObjectPath(std::string_view objectPath, std::wstring& widePath) const
	{
		if (objectPath.empty() || objectPath.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
			objectPath.find('\0') != std::string_view::npos)
		{
			return false;
		}
		const int inputLength = static_cast<int>(objectPath.size());
		const int wideLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, objectPath.data(), inputLength, nullptr, 0);
		if (wideLength <= 0)
		{
			return false;
		}
		widePath.resize(static_cast<std::size_t>(wideLength));
		return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, objectPath.data(), inputLength,
			widePath.data(), wideLength) == wideLength;
	}

	/** Finds a resident class through the existing typed object lookup; it never loads or scans. */
	SDK::UClass* BuildingSpriteRenderer::FindResidentClass(std::wstring_view objectPath) const
	{
		if (objectPath.empty() || !IsGameThread())
		{
			return nullptr;
		}
		std::uintptr_t classMetaObject = 0;
		constexpr wchar_t classMetaPath[] = L"/Script/CoreUObject.Class";
		if (!MiniMapResidentMetadata::FindResident(nullptr, classMetaPath, false, classMetaObject))
		{
			return nullptr;
		}
		const std::wstring ownedPath(objectPath);
		std::uintptr_t foundClass = 0;
		if (!MiniMapResidentMetadata::FindResident(reinterpret_cast<SDK::UClass*>(classMetaObject),
			ownedPath.c_str(), true, foundClass))
		{
			return nullptr;
		}
		return reinterpret_cast<SDK::UClass*>(foundClass);
	}

	/** Creates and retains a UObject using the established NativeApi spawn and root operations. */
	SDK::UObject* BuildingSpriteRenderer::CreateRetainedObject(SDK::UClass* objectClass, SDK::UObject* outer)
	{
		const auto* nativeApi = MiniMapNative::Get();
		if (!objectClass || !outer || !nativeApi || !nativeApi->asset.spawnObject ||
			!nativeApi->asset.setObjectFlags || !nativeApi->asset.retainObject)
		{
			return nullptr;
		}
		SDK::TSubclassOf<SDK::UObject> objectType(objectClass);
		SDK::UObject* object = nativeApi->asset.spawnObject(&objectType, outer);
		if (!object)
		{
			return nullptr;
		}
		nativeApi->asset.setObjectFlags(object, 0x40);
		nativeApi->asset.retainObject(object);
		return object;
	}

	/** Creates one configured directional key/fill light and registers it to the private world. */
	bool BuildingSpriteRenderer::CreateDirectionalLight(std::size_t lightIndex)
	{
		if (lightIndex >= directionalLights_.size() || !privateWorld_ || !IsGameThread())
		{
			return false;
		}
		SDK::UClass* lightClass = FindResidentClass(L"/Script/Engine.DirectionalLightComponent");
		SDK::UObject* object = CreateRetainedObject(lightClass, reinterpret_cast<SDK::UObject*>(privateWorld_));
		directionalLights_[lightIndex] = reinterpret_cast<SDK::UDirectionalLightComponent*>(object);
		if (!object || !ConfigureDirectionalLight(lightIndex))
		{
			return false;
		}
		const auto* nativeApi = MiniMapNative::Get();
		if (!nativeApi || !nativeApi->player.registerComponent)
		{
			return false;
		}
		nativeApi->player.registerComponent(directionalLights_[lightIndex], privateWorld_, nullptr);
		directionalLightRegistered_[lightIndex] = true;
		return true;
	}

	/** Unregisters and destroys one persistent light while retaining its root for fence retirement. */
	bool BuildingSpriteRenderer::DestroyDirectionalLight(std::size_t lightIndex)
	{
		if (lightIndex >= directionalLights_.size() || !directionalLights_[lightIndex] ||
			directionalLightDestroyed_[lightIndex])
		{
			return true;
		}
		const auto* nativeApi = MiniMapNative::Get();
		if (!nativeApi || !nativeApi->player.unregisterComponent || !IsGameThread())
		{
			return false;
		}
		if (directionalLightRegistered_[lightIndex])
		{
			nativeApi->player.unregisterComponent(directionalLights_[lightIndex]);
			directionalLightRegistered_[lightIndex] = false;
		}
		if (!DestroyComponent(directionalLights_[lightIndex]))
		{
			return false;
		}
		directionalLightDestroyed_[lightIndex] = true;
		return true;
	}

	/** Releases the shared lighting rig and private world only after request retirement. */
	bool BuildingSpriteRenderer::CleanupPersistentWorld()
	{
		if (!IsGameThread())
		{
			return false;
		}
		for (std::size_t lightIndex = 0; lightIndex < directionalLights_.size(); ++lightIndex)
		{
			if (!DestroyDirectionalLight(lightIndex))
			{
				return false;
			}
		}
		if (privateWorld_ && !exitCleanupFenceComplete_)
		{
			if (fencePurpose_ == FencePurpose::None && !StartFence(FencePurpose::RequestRetirement))
			{
				return false;
			}
			if (fencePurpose_ != FencePurpose::RequestRetirement ||
				!PollFence(FencePurpose::RequestRetirement))
			{
				return false;
			}
			exitCleanupFenceComplete_ = true;
		}
		return ReleasePersistentWorldRoots();
	}

	/** Releases retained light roots and cleans the private-world root in dependency order. */
	bool BuildingSpriteRenderer::ReleasePersistentWorldRoots()
	{
		for (std::size_t lightIndex = 0; lightIndex < directionalLights_.size(); ++lightIndex)
		{
			if (!ReleaseObject(reinterpret_cast<SDK::UObject*>(directionalLights_[lightIndex])))
			{
				return false;
			}
			directionalLights_[lightIndex] = nullptr;
		}
		if (privateWorld_ && !privateWorldCleaned_)
		{
			const auto* nativeApi = MiniMapNative::Get();
			if (!nativeApi || !nativeApi->world.cleanupWorld)
			{
				return false;
			}
			nativeApi->world.cleanupWorld(reinterpret_cast<std::uintptr_t>(privateWorld_), true, true, 0);
			privateWorldCleaned_ = true;
		}
		if (!ReleaseObject(reinterpret_cast<SDK::UObject*>(privateWorld_)))
		{
			return false;
		}
		privateWorld_ = nullptr;
		privateWorldInitialized_ = false;
		lightingRigSetupStarted_ = false;
		lightingRigReady_ = false;
		sharedWorldUsable_ = false;
		return true;
	}

	/** Applies the demonstrated key/fill direction, intensity, color, and shadow settings. */
	bool BuildingSpriteRenderer::ConfigureDirectionalLight(std::size_t lightIndex)
	{
		if (lightIndex >= directionalLights_.size() || !directionalLights_[lightIndex])
		{
			return false;
		}
		const bool key = lightIndex == 0;
		auto* light = directionalLights_[lightIndex];
		light->PrimaryComponentTick.bCanEverTick = false;
		light->bReplicates = false;
		light->bAutoActivate = false;
		light->bCanEverAffectNavigation = false;
		light->Mobility = SDK::EComponentMobility::Movable;
		light->Intensity = key ? 6.0f : 2.0f;
		light->LightColor.R = key ? 255 : 238;
		light->LightColor.G = key ? 248 : 245;
		light->LightColor.B = key ? 240 : 255;
		light->LightColor.A = 255;
		light->bUseTemperature = false;
		light->bAffectsWorld = true;
		light->CastShadows = key;
		light->CastStaticShadows = false;
		light->CastDynamicShadows = key;
		light->bAffectTranslucentLighting = true;
		light->bAtmosphereSunLight = false;
		light->bPerPixelAtmosphereTransmittance = false;
		light->bCastCloudShadows = false;
		light->bCastShadowsOnClouds = false;
		light->bCastShadowsOnAtmosphere = false;
		light->LightSourceAngle = key ? 3.0f : 6.0f;
		light->ForwardShadingPriority = key ? 1 : 0;
		light->K2_SetWorldLocationAndRotation(SDK::FVector(0, 0, 0),
			SDK::FRotator(key ? -55.0 : -35.0, key ? -45.0 : 135.0, 0), false, nullptr, true);
		return true;
	}

	/** Submits HDR C0, which warms the capture scene before the later C10 submission. */
	void BuildingSpriteRenderer::SubmitHdrWarmupWhenAdmitted()
	{
		if (admittedServiceIterations_ <= preparedIteration_)
		{
			return;
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		if (!SubmitCaptureSceneForPurpose(CaptureSubmissionPurpose::HdrWarmup))
#else
		if (!SubmitCaptureScene())
#endif
		{
			FailRequest(CaptureResult::Completion::Failure::EngineOperationFailed);
			return;
		}
		hdrWarmupIteration_ = admittedServiceIterations_;
		phase_ = Phase::HdrWarmupSubmitted;
	}

	/** Submits HDR C10 after ten later admitted service iterations and waits for asynchronous readback. */
	void BuildingSpriteRenderer::SubmitHdrCaptureAfterWarmup()
	{
		if (admittedServiceIterations_ - hdrWarmupIteration_ < 10)
		{
			return;
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		if (!SubmitCaptureSceneForPurpose(CaptureSubmissionPurpose::HdrCapture))
#else
		if (!SubmitCaptureScene())
#endif
		{
			FailRequest(CaptureResult::Completion::Failure::EngineOperationFailed);
			return;
		}
		phase_ = Phase::AwaitingHdrReadback;
	}

	/** Starts or advances one readback and forwards pixels only after native retirement. */
	void BuildingSpriteRenderer::AdvanceReadbackPhase()
	{
		const bool hdrPass = phase_ == Phase::AwaitingHdrReadback;
		BuildingSpriteReadback& readback = hdrPass ? hdrReadback_ : finalReadback_;
		if (!readbackStarted_)
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			if (!sceneWorkSubmitted_ || !renderTarget_ ||
				!StartReadbackForDiagnostics(hdrPass, readback))
#else
			if (!sceneWorkSubmitted_ || !renderTarget_ || !readback.Start(renderTarget_))
#endif
			{
				FailRequest(CaptureResult::Completion::Failure::ReadbackFailed);
				return;
			}
			readbackStarted_ = true;
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const BuildingSpriteReadback::Status status =
			AdvanceReadbackForDiagnostics(hdrPass, readback);
#else
		const BuildingSpriteReadback::Status status = readback.Advance();
#endif
		if (status == BuildingSpriteReadback::Status::Pending || !readback.IsRetired())
		{
			return;
		}
		if (status != BuildingSpriteReadback::Status::Complete)
		{
			RetireFailedReadback(readback);
			return;
		}
		if (hdrPass)
		{
			CompleteHdrReadback();
			return;
		}
		CompleteFinalReadback();
	}

	/** Retires an unsuccessful pass only when the backend proves its resources are safe. */
	void BuildingSpriteRenderer::RetireFailedReadback(BuildingSpriteReadback& readback)
	{
		if (!readback.Reset())
		{
			return;
		}
		readbackStarted_ = false;
		FailRequest(CaptureResult::Completion::Failure::ReadbackFailed);
	}

	/** Saves HDR source pixels and reconfigures only after GPU copy and scene retirement. */
	void BuildingSpriteRenderer::CompleteHdrReadback()
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		DurationScope durationScope(diagnostics_.HdrReadbackCompletion);
#endif
		try
		{
			hdrPixels_ = hdrReadback_.Pixels();
		}
		catch (...)
		{
			RetireFailedReadback(hdrReadback_);
			return;
		}
		if (!hdrReadback_.Reset())
		{
			return;
		}
		readbackStarted_ = false;
		if (!AcknowledgeHdrPassRetirement(true, true))
		{
			FailRequest(CaptureResult::Completion::Failure::RetirementFailed);
		}
	}

	/** Converts final samples to an immutable payload before request-specific retirement. */
	void BuildingSpriteRenderer::CompleteFinalReadback()
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		DurationScope durationScope(diagnostics_.FinalReadbackCompletion);
#endif
		sceneWorkSubmitted_ = false;
		ProcessFinalCapture();
		if (!finalReadback_.Reset())
		{
			return;
		}
		readbackStarted_ = false;
		phase_ = Phase::RetirementRequired;
	}

	/** Uses the independent BM-02 processor to create the only successful capture payload. */
	void BuildingSpriteRenderer::ProcessFinalCapture()
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		DurationScope finalCaptureScope(diagnostics_.FinalCaptureProcessing);
#endif
		const auto& finalPixels = finalReadback_.Pixels();
		Cache::SpriteCapturePixels pixels{
			std::span<const std::uint16_t>(finalPixels.data(), finalPixels.size()),
			std::span<const std::uint16_t>(hdrPixels_.data(), hdrPixels_.size())};
		const double orthographicExtent = 2.0 * (std::max)(boundsExtent_[0], boundsExtent_[1]) * 1.1;
		Cache::SpriteCaptureMetadata metadata{
			GetCaptureBounds(),
			orthographicExtent,
			spriteCaptureDensityPixelsPerMapUnit_};
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		Cache::SpriteProcessingResult result;
		{
			DurationScope spriteProcessingScope(diagnostics_.SpriteProcessing);
			result = Cache::SpriteProcessing::Process(pixels, metadata);
		}
		++diagnostics_.SpriteProcessingStatusCounts[static_cast<std::size_t>(result.Result)];
#else
		Cache::SpriteProcessingResult result = Cache::SpriteProcessing::Process(pixels, metadata);
#endif
		if (result.Result != Cache::SpriteProcessingResult::Status::Processed || !result.Payload)
		{
			pendingFailure_ = result.Result == Cache::SpriteProcessingResult::Status::EmptyCapture
				? CaptureResult::Completion::Failure::InvalidCapture
				: CaptureResult::Completion::Failure::ProcessingFailed;
			return;
		}
		pendingPayload_ = std::move(result.Payload);
	}

	/** Detaches submitted capture resources, orders their release fence, and waits without blocking. */
	bool BuildingSpriteRenderer::RetireSubmittedSceneBehindFence()
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		DurationScope durationScope(diagnostics_.SubmittedSceneRetirement);
#endif
		if (readbackStarted_)
		{
			BuildingSpriteReadback& readback = finalColorPassSubmitted_ ? finalReadback_ : hdrReadback_;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			const bool hdrPass = !finalColorPassSubmitted_;
			AdvanceReadbackForDiagnostics(hdrPass, readback);
#else
			readback.Advance();
#endif
			if (!readback.IsRetired())
			{
				return false;
			}
			readback.Reset();
			readbackStarted_ = false;
		}
		if (!DestroyRequestContents())
		{
			return false;
		}
		if (!requestSceneRetirementFenceComplete_)
		{
			if (fencePurpose_ == FencePurpose::None && !StartFence(FencePurpose::RequestRetirement))
			{
				return false;
			}
			if (!PollFence(FencePurpose::RequestRetirement))
			{
				return false;
			}
			requestSceneRetirementFenceComplete_ = true;
			requestRetirementFenceComplete_ = true;
		}
		sceneWorkSubmitted_ = false;
		return true;
	}

	/** Lazily prepares the persistent world and lighting rig for capture. */
	void BuildingSpriteRenderer::PreparePrivateWorld()
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		DurationScope durationScope(diagnostics_.PrivateWorldPreparation);
#endif
		if (!privateWorld_ && !CreatePrivateWorld())
		{
			sharedWorldUsable_ = false;
			FailRequest(CaptureResult::Completion::Failure::EngineOperationFailed);
			return;
		}
		if (!privateWorldInitialized_)
		{
			sharedWorldUsable_ = false;
			FailRequest(CaptureResult::Completion::Failure::EngineOperationFailed);
			return;
		}
		if (!EnsureLightingRigReady())
		{
			sharedWorldUsable_ = false;
			FailRequest(CaptureResult::Completion::Failure::EngineOperationFailed);
			return;
		}
		if (!lightingRigReady_)
		{
			return;
		}
		phase_ = Phase::PreparingSubject;
	}

	/** Resolves the subject, obtains bounds, and creates its capture rig without submission. */
	void BuildingSpriteRenderer::PrepareSubject()
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		DurationScope durationScope(diagnostics_.SubjectPreparation);
#endif
		if (!ResolveAndRetainActorClass(activeRequest_.SpriteRequirement.Bytes()) ||
			!SpawnSubjectAndAcquireBounds())
		{
			FailRequest(CaptureResult::Completion::Failure::SubjectUnavailable);
			return;
		}
		if (!CreateCaptureResources())
		{
			FailRequest(CaptureResult::Completion::Failure::EngineOperationFailed);
			return;
		}
		preparedIteration_ = admittedServiceIterations_;
		phase_ = Phase::WaitingForHdrWarmup;
	}

	/** Marks a failed request terminal only when no submitted scene resource remains outstanding. */
	void BuildingSpriteRenderer::FailRequest(CaptureResult::Completion::Failure failure)
	{
		resultEligible_ = false;
		pendingFailure_ = failure;
		if (sceneWorkSubmitted_)
		{
			phase_ = Phase::RetirementRequired;
			return;
		}
		if (!RetireWithoutSubmittedScene())
		{
			phase_ = Phase::RetirementRequired;
			return;
	}
		completion_.Error = failure;
		completion_.Result = CaptureResult::Completion::Status::Failed;
		phase_ = Phase::Terminal;
	}

	/** Captures one scene and retains all connected resources until BM-07 retires its work. */
	bool BuildingSpriteRenderer::SubmitCaptureScene()
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		DurationScope durationScope(diagnostics_.CaptureSceneSubmission);
#endif
		if (!captureComponent_ || !captureComponentRegistered_ || !IsGameThread())
		{
			return false;
		}
		if (!BuildingSpriteReadback::InitializeNativeCapture())
		{
			return false;
		}
		sceneWorkSubmitted_ = true;
		captureComponent_->CaptureScene();
		return true;
	}

	/** Unregisters and destroys one capture or light component before world cleanup. */
	bool BuildingSpriteRenderer::DestroyComponent(SDK::UActorComponent* component)
	{
		if (!component)
		{
			return true;
		}
		const auto* nativeApi = MiniMapNative::Get();
		if (!nativeApi || !nativeApi->player.destroyComponent)
		{
			return false;
		}
		nativeApi->player.destroyComponent(component, false);
		return true;
	}

	/** Destroys one actor before private-world cleanup. */
	bool BuildingSpriteRenderer::DestroyActor(SDK::AActor*& actor)
	{
		if (!actor || actorDestroyFailed_)
		{
			return !actor;
		}
		const auto* nativeApi = MiniMapNative::Get();
		if (!nativeApi || !nativeApi->world.destroyActor)
		{
			return false;
		}
		if (!nativeApi->world.destroyActor(reinterpret_cast<std::uintptr_t>(actor), false, false))
		{
			actorDestroyFailed_ = true;
			return false;
		}
		return true;
	}

	/** Applies the established show flags, gamma, target format, and non-mipped target settings. */
	bool BuildingSpriteRenderer::ConfigureShowFlagsAndTarget(bool finalColor)
	{
		static_assert(std::is_trivially_destructible_v<SDK::FEngineShowFlagsSetting>);
		const wchar_t* flagNames[] = {L"Fog", L"Atmosphere", L"VolumetricFog", L"Cloud", L"MotionBlur", L"TemporalAA",
			L"AntiAliasing", L"PostProcessing", L"Bloom", L"EyeAdaptation", L"Lighting", L"DirectLighting",
			L"Diffuse", L"Specular", L"DynamicShadows", L"Tonemapper"};
		SDK::FEngineShowFlagsSetting flags[16]{};
		for (int flagIndex = 0; flagIndex < 16; ++flagIndex)
		{
			flags[flagIndex].ShowFlagName = SDK::FString(flagNames[flagIndex]);
			flags[flagIndex].Enabled = (flagIndex >= 10 && flagIndex < 15) ||
				(finalColor && (flagIndex == 7 || flagIndex == 15));
		}
		SDK::TArray<SDK::FEngineShowFlagsSetting> settings(flags, 16, 16);
		captureComponent_->SetShowFlagSettings(settings);
		renderTarget_->TargetGamma = finalColor ? 2.2f : 1.0f;
		renderTarget_->bAutoGenerateMips = false;
		renderTarget_->bSupportsUAV = false;
		renderTarget_->ClearColor = SDK::FLinearColor(0, 0, 0, 1);
		return true;
	}
}

#endif // MODLOADER_CLIENT_BUILD
