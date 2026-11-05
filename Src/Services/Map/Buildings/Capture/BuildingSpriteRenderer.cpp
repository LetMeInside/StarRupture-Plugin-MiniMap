// Implements the persistent capture world, two-pass sprite capture, asynchronous readback, and retirement.
#if defined(MODLOADER_CLIENT_BUILD)

#include "BuildingSpriteRenderer.h"

#include "../Cache/SpriteProcessing.h"
#include "Native/NativeApi.h"
#include "Native/ResidentMetadata.h"

#include <Engine_classes.hpp>
#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <span>
#include <type_traits>

namespace Services::Map::Buildings::Capture
{
	/** Stores the service-owned session identity without preparing native resources. */
	BuildingSpriteRenderer::BuildingSpriteRenderer(std::uint64_t sessionGeneration, double pixelsPerMapUnit)
		: activeGameWorldSessionGeneration_(sessionGeneration)
		, pixelsPerMapUnit_(pixelsPerMapUnit)
	{
	}

	/** Copies an owned capture request or reports why it cannot be admitted. */
	CaptureResult::Acceptance BuildingSpriteRenderer::Accept(const CaptureRequest& request)
	{
		CaptureResult::Acceptance acceptance;
		acceptance.Correlation = request.RequestIdentity;
		if (hasActiveRequest_ || serviceShutdownRequested_ || !sharedWorldUsable_)
		{
			acceptance.Result = CaptureResult::Acceptance::Status::RejectedUnavailable;
			return acceptance;
		}
		std::string spriteFileName;
		if (!request.Subject.SpriteRequirement.TryGetFileName(spriteFileName) ||
			request.Subject.ActorClassObjectPath.empty())
		{
			acceptance.Result = CaptureResult::Acceptance::Status::RejectedInvalidRequest;
			return acceptance;
		}
		if (request.RequestIdentity.SessionGeneration != activeGameWorldSessionGeneration_)
		{
			acceptance.Result = CaptureResult::Acceptance::Status::RejectedStaleSession;
			return acceptance;
		}
		if (!ValidateSubjectFacts(request.Subject))
		{
			acceptance.Result = CaptureResult::Acceptance::Status::RejectedIsolation;
			return acceptance;
		}
		if (request.RequestIdentity.SessionGeneration == 0 || request.RequestIdentity.RequestSequence == 0)
		{
			acceptance.Result = CaptureResult::Acceptance::Status::RejectedInvalidRequest;
			return acceptance;
		}
		if (request.RequestIdentity.RequestSequence <= lastAcceptedRequestSequence_)
		{
			acceptance.Result = CaptureResult::Acceptance::Status::RejectedStaleRequest;
			return acceptance;
		}
		activeRequest_ = request;
		completion_ = {};
		pendingFailure_ = CaptureResult::Completion::Failure::None;
		completion_.Correlation = request.RequestIdentity;
		completion_.Result = CaptureResult::Completion::Status::Pending;
		phase_ = Phase::PreparingWorld;
		hasActiveRequest_ = true;
		lastAcceptedRequestSequence_ = request.RequestIdentity.RequestSequence;
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
		acceptance.Result = CaptureResult::Acceptance::Status::Accepted;
		return acceptance;
	}

	/** Advances world preparation, subject setup, or one admitted capture submission. */
	void BuildingSpriteRenderer::Advance()
	{
		if (!hasActiveRequest_ || !IsGameThread() || phase_ == Phase::Terminal)
		{
			return;
		}
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
			if (!SubmitCaptureScene())
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

	/** Returns the owned request's current pending or terminal completion value. */
	CaptureResult::Completion BuildingSpriteRenderer::GetProgress(CaptureRequest::Correlation correlation) const
	{
		if (!hasActiveRequest_ || correlation.SessionGeneration != completion_.Correlation.SessionGeneration ||
			correlation.RequestSequence != completion_.Correlation.RequestSequence)
		{
			CaptureResult::Completion stale;
			stale.Correlation = correlation;
			stale.Result = CaptureResult::Completion::Status::Failed;
			stale.Error = CaptureResult::Completion::Failure::StaleRequest;
			return stale;
		}
		return completion_;
	}

	/** Cancels only the correlated request and keeps submitted resources until BM-07 retires them. */
	void BuildingSpriteRenderer::Cancel(CaptureRequest::Correlation correlation)
	{
		if (!hasActiveRequest_ || correlation.SessionGeneration != activeRequest_.RequestIdentity.SessionGeneration ||
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

	/** Releases a terminal result only after the service has matched its request correlation. */
	void BuildingSpriteRenderer::AcknowledgeCompletion(CaptureRequest::Correlation correlation)
	{
		if (!hasActiveRequest_ || phase_ != Phase::Terminal ||
			completion_.Result == CaptureResult::Completion::Status::Pending ||
			correlation.SessionGeneration != completion_.Correlation.SessionGeneration ||
			correlation.RequestSequence != completion_.Correlation.RequestSequence || sceneWorkSubmitted_ ||
			subjectActor_ || captureComponent_ || renderTarget_ || actorClass_ ||
			fencePurpose_ == FencePurpose::RequestRetirement)
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

	/** Rejects game-state dependencies while retaining all supplied values for the future scheduler. */
	bool BuildingSpriteRenderer::ValidateSubjectFacts(const CaptureSubjectFacts& subject) const
	{
		if (!subject.Isolation.IsCompatibleWithPrivateCaptureWorld ||
			subject.Isolation.RequiresGameplayWorldState || subject.Isolation.RequiresGameplayWorldTick ||
			subject.Isolation.ReferencesGameplayWorldObjects)
		{
			return false;
		}
		for (const std::string& subsystemPath : subject.Isolation.RequiredWorldSubsystemClassPaths)
		{
			if (subsystemPath.empty())
			{
				return false;
			}
		}
		return true;
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
		if (!nativeApi || !nativeApi->asset.retainObject || !nativeApi->world.getSubsystemInternal)
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
		nativeApi->asset.retainObject(privateWorld_);
		nativeApi->world.initWorld(world, initializationValues);
		nativeApi->world.updateWorldComponents(world, false, false, nullptr);
		privateWorldInitialized_ = true;
		return true;
	}

	/** Checks host-listed required subsystem classes against this new world's own collection. */
	bool BuildingSpriteRenderer::VerifyRequiredWorldSubsystems(const CaptureSubjectFacts& subject)
	{
		const auto* nativeApi = MiniMapNative::Get();
		if (!privateWorld_ || !nativeApi || !nativeApi->world.getSubsystemInternal || !IsGameThread())
		{
			return false;
		}
		static_assert(0x9f0 + 0xc0 == sizeof(SDK::UWorld));
		const std::uintptr_t collection = reinterpret_cast<std::uintptr_t>(privateWorld_) + 0x9f0;
		for (const std::string& subsystemPath : subject.Isolation.RequiredWorldSubsystemClassPaths)
		{
			std::wstring widePath;
			if (!ConvertObjectPath(subsystemPath, widePath))
			{
				return false;
			}
			SDK::UClass* subsystemClass = FindResidentClass(widePath);
			SDK::UClass* subsystemBaseClass = FindResidentClass(L"/Script/Engine.WorldSubsystem");
			if (!subsystemClass || !subsystemBaseClass || !SDK::UKismetMathLibrary::ClassIsChildOf(
				SDK::TSubclassOf<SDK::UObject>(subsystemClass),
				SDK::TSubclassOf<SDK::UObject>(subsystemBaseClass)) || !nativeApi->world.getSubsystemInternal(
				collection, reinterpret_cast<std::uintptr_t>(subsystemClass)))
			{
				return false;
			}
		}
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

	/** Unregisters and destroys the service-lifetime directional lighting rig. */
	bool BuildingSpriteRenderer::DestroySharedLighting()
	{
		for (std::size_t lightIndex = 0; lightIndex < directionalLights_.size(); ++lightIndex)
		{
			if (!RetireLight(lightIndex)) return false;
		}
		return true;
	}

	/** Cleans up the persistent private world only during service-directed shutdown. */
	bool BuildingSpriteRenderer::CleanupPrivateWorld()
	{
		if (!privateWorld_ || privateWorldCleaned_) return true;
		const auto* nativeApi = MiniMapNative::Get();
		if (!nativeApi || !nativeApi->world.cleanupWorld || !IsGameThread()) return false;
		nativeApi->world.cleanupWorld(reinterpret_cast<std::uintptr_t>(privateWorld_), true, true, 0);
		privateWorldCleaned_ = true;
		return true;
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

	/** Releases the shared light and world roots after service retirement completes. */
	bool BuildingSpriteRenderer::ReleaseWorldRoots()
	{
		for (SDK::UDirectionalLightComponent*& light : directionalLights_)
		{
			if (!ReleaseObject(reinterpret_cast<SDK::UObject*>(light))) return false;
			light = nullptr;
		}
		if (!ReleaseObject(reinterpret_cast<SDK::UObject*>(privateWorld_))) return false;
		privateWorld_ = nullptr;
		privateWorldInitialized_ = false;
		lightingRigReady_ = false;
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

	/** Begins service shutdown and cancels any request without bypassing submitted-work retirement. */
	void BuildingSpriteRenderer::RequestServiceShutdown()
	{
		serviceShutdownRequested_ = true;
		if (!hasActiveRequest_) return;
		resultEligible_ = false;
		pendingFailure_ = CaptureResult::Completion::Failure::None;
		pendingPayload_.reset();
		completion_.Payload.reset();
		if (phase_ == Phase::Terminal)
		{
			if (completion_.Result == CaptureResult::Completion::Status::Completed)
			{
				completion_.Result = CaptureResult::Completion::Status::Cancelled;
			}
			return;
		}
		phase_ = Phase::RetirementRequired;
	}

	/** Advances service shutdown without releasing any outstanding native owner. */
	bool BuildingSpriteRenderer::AdvanceServiceShutdown()
	{
		if (!serviceShutdownRequested_ || !IsGameThread()) return false;
		if (hasActiveRequest_ && !AdvanceRequestShutdown()) return false;
		return AdvanceSharedWorldShutdown();
	}

	/** Retires the active request, waiting for BM-07 after any submitted scene work. */
	bool BuildingSpriteRenderer::AdvanceRequestShutdown()
	{
		if (phase_ != Phase::Terminal)
		{
			phase_ = Phase::RetirementRequired;
			AdvanceRetirement();
			if (phase_ != Phase::Terminal) return false;
		}
		hasActiveRequest_ = false;
		activeRequest_ = {};
		completion_ = {};
		phase_ = Phase::Idle;
		return true;
	}

	/** Retires persistent lights and world, then releases the service-owned roots. */
	bool BuildingSpriteRenderer::AdvanceSharedWorldShutdown()
	{
		if (serviceRetired_) return true;
		if (fencePurpose_ == FencePurpose::LightingReadiness)
		{
			if (!PollFence(FencePurpose::LightingReadiness)) return false;
			lightingRigReady_ = true;
		}
		if (fencePurpose_ == FencePurpose::RequestRetirement) return false;
		if (!lightingRetirementFenceComplete_)
		{
			if (!DestroySharedLighting()) return false;
			if (privateWorld_ && fencePurpose_ == FencePurpose::None &&
				!StartFence(FencePurpose::ServiceRetirement)) return false;
			if (privateWorld_ && !PollFence(FencePurpose::ServiceRetirement)) return false;
			lightingRetirementFenceComplete_ = true;
		}
		if (!CleanupPrivateWorld()) return false;
		if (!serviceRetirementFenceComplete_)
		{
			if (privateWorld_ && fencePurpose_ == FencePurpose::None &&
				!StartFence(FencePurpose::ServiceRetirement)) return false;
			if (privateWorld_ && !PollFence(FencePurpose::ServiceRetirement)) return false;
			serviceRetirementFenceComplete_ = true;
		}
		if (!ReleaseWorldRoots()) return false;
		serviceRetired_ = true;
		sharedWorldUsable_ = false;
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
		lightComponentsRegistered_[lightIndex] = true;
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
		if (!SubmitCaptureScene())
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
		if (!SubmitCaptureScene())
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
			if (!sceneWorkSubmitted_ || !renderTarget_ || !readback.Start(renderTarget_))
			{
				FailRequest(CaptureResult::Completion::Failure::ReadbackFailed);
				return;
			}
			readbackStarted_ = true;
		}
		const BuildingSpriteReadback::Status status = readback.Advance();
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
		const auto& finalPixels = finalReadback_.Pixels();
		Cache::SpriteCapturePixels pixels{
			std::span<const std::uint16_t>(finalPixels.data(), finalPixels.size()),
			std::span<const std::uint16_t>(hdrPixels_.data(), hdrPixels_.size())};
		const double orthographicExtent = 2.0 * (std::max)(boundsExtent_[0], boundsExtent_[1]) * 1.1;
		Cache::SpriteCaptureMetadata metadata{
			GetCaptureBounds(),
			orthographicExtent,
			pixelsPerMapUnit_};
		const Cache::SpriteProcessingResult result = Cache::SpriteProcessing::Process(pixels, metadata);
		if (result.Result != Cache::SpriteProcessingResult::Status::Processed || !result.Payload)
		{
			pendingFailure_ = result.Result == Cache::SpriteProcessingResult::Status::EmptyCapture
				? CaptureResult::Completion::Failure::InvalidCapture
				: CaptureResult::Completion::Failure::ProcessingFailed;
			return;
		}
		pendingPayload_ = result.Payload;
	}

	/** Detaches submitted capture resources, orders their release fence, and waits without blocking. */
	bool BuildingSpriteRenderer::RetireSubmittedSceneBehindFence()
	{
		if (readbackStarted_)
		{
			BuildingSpriteReadback& readback = finalColorPassSubmitted_ ? finalReadback_ : hdrReadback_;
			readback.Advance();
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

	/** Lazily prepares the persistent world/rig and verifies this request's required subsystems. */
	void BuildingSpriteRenderer::PreparePrivateWorld()
	{
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
		if (!VerifyRequiredWorldSubsystems(activeRequest_.Subject))
		{
			FailRequest(CaptureResult::Completion::Failure::IsolationRejected);
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
		if (!ResolveAndRetainActorClass(activeRequest_.Subject.ActorClassObjectPath) ||
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

	/** Unregisters and destroys one directional light before private-world cleanup. */
	bool BuildingSpriteRenderer::RetireLight(std::size_t lightIndex)
	{
		if (lightIndex >= directionalLights_.size() || !directionalLights_[lightIndex])
		{
			return true;
		}
		SDK::UActorComponent* component = directionalLights_[lightIndex];
		if (lightComponentsRegistered_[lightIndex])
		{
			const auto* nativeApi = MiniMapNative::Get();
			if (!nativeApi || !nativeApi->player.unregisterComponent)
			{
				return false;
			}
			nativeApi->player.unregisterComponent(component);
			lightComponentsRegistered_[lightIndex] = false;
		}
		if (lightComponentsDestroyed_[lightIndex])
		{
			return true;
		}
		if (!DestroyComponent(component))
		{
			return false;
		}
		lightComponentsDestroyed_[lightIndex] = true;
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
