// Owns one persistent private world and lighting rig plus request-scoped sprite capture resources.
// It is an internal BuildingMapService component with explicit service-directed retirement.
#pragma once

#include "CaptureRequest.h"
#include "CaptureResult.h"
#include "BuildingSpriteReadback.h"
#include "../Visuals/SpriteGeometry.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace SDK
{
	class AActor;
	class UActorComponent;
	class UClass;
	class UDirectionalLightComponent;
	class UObject;
	class USceneCaptureComponent2D;
	class UTextureRenderTarget2D;
	class UWorld;
}

namespace Services::Map::Buildings
{
	class BuildingMapService;
}

namespace Services::Map::Buildings::Capture
{
	/** Owns one persistent private world and shared lighting rig plus one active capture request.
	 * Only BuildingMapService may submit, advance, cancel, or coordinate its retirement.
	 * All methods that touch UObject or NativeApi state require the game thread. Native
	 * resources must be explicitly retired before the service releases this object. Submitted
	 * scene work remains owned until asynchronous readback or an ordered retirement fence proves safety.
	 */
	class BuildingSpriteRenderer final
	{
	public:
		/** Does not perform native cleanup; the service must retire resources explicitly. */
		~BuildingSpriteRenderer() = default;

	private:
		friend class Services::Map::Buildings::BuildingMapService;

	private:
		/** Describes the point reached in the nonblocking HDR/final-color capture sequence. */
		enum class Phase : std::uint8_t
		{
			Idle,
			PreparingWorld,
			PreparingSubject,
			WaitingForHdrWarmup,
			HdrWarmupSubmitted,
			AwaitingHdrReadback,
			WaitingForFinalColor,
			AwaitingFinalReadback,
			RetirementRequired,
			Terminal
		};
		/** Identifies the owner whose queued render operations a fence protects. */
		enum class FencePurpose : std::uint8_t
		{
			None,
			LightingReadiness,
			RequestRetirement,
			ServiceRetirement
		};

		/** Creates an inert renderer tied to one active game-world session generation. */
		explicit BuildingSpriteRenderer(std::uint64_t sessionGeneration, double pixelsPerMapUnit);
		/** Prevents copies from creating ambiguous native-resource ownership. */
		BuildingSpriteRenderer(const BuildingSpriteRenderer&) = delete;
		/** Prevents copies from creating ambiguous native-resource ownership. */
		BuildingSpriteRenderer& operator=(const BuildingSpriteRenderer&) = delete;
		/** Prevents moves from invalidating service-directed native-resource ownership. */
		BuildingSpriteRenderer(BuildingSpriteRenderer&&) = delete;
		/** Prevents moves from invalidating service-directed native-resource ownership. */
		BuildingSpriteRenderer& operator=(BuildingSpriteRenderer&&) = delete;

		/** Copies and accepts one owned request or returns a synchronous rejection.
		 * @param request Owned sprite identity, subject facts, and session correlation.
		 */
		CaptureResult::Acceptance Accept(const CaptureRequest& request);
		/** Advances at most one admitted game-thread operation or readback poll. */
		void Advance();
		/** Retries safe cleanup after a pre-submission cancellation or failure. */
		void AdvanceRetirement();
		/** Runs the phase-specific world, subject, warm-up, or capture step. */
		void AdvanceCurrentPhase();
		/** Returns matching progress or a stale-request rejection for the supplied correlation. */
		CaptureResult::Completion GetProgress(CaptureRequest::Correlation correlation) const;
		/** Revokes result eligibility; submitted scene work remains owned until BM-07 retires it. */
		void Cancel(CaptureRequest::Correlation correlation);
		/** Releases a terminal value after the service has consumed its completion. */
		void AcknowledgeCompletion(CaptureRequest::Correlation correlation);
		/** Continues or cancels HDR work after BM-07 proves scene/readback retirement. */
		bool AcknowledgeHdrPassRetirement(bool readbackWasSubmitted, bool readbackAndSceneRetired);
		/** Returns capture-space XYZ bounds owned by the active subject request. */
		Visuals::SpriteCaptureBounds GetCaptureBounds() const;

		/** Validates subject facts without interpreting game-specific names or policy. */
		bool ValidateSubjectFacts(const CaptureSubjectFacts& subject) const;
		/** Resolves and retains a copied class path using existing resident-object services. */
		bool ResolveAndRetainActorClass(std::string_view objectPath);
		/** Creates and initializes the independently owned private world. */
		bool CreatePrivateWorld();
		/** Confirms request-required subsystems and prepares/reuses the shared lighting rig. */
		bool VerifyRequiredWorldSubsystems(const CaptureSubjectFacts& subject);
		/** Creates the persistent lighting rig once and polls its nonblocking readiness fence. */
		bool EnsureLightingRigReady();
		/** Creates the deferred actor, finishes construction, and records native bounds. */
		bool SpawnSubjectAndAcquireBounds();
		/** Records finite actor bounds or the demonstrated legacy fallback bounds. */
		bool AcquireSubjectBounds();
		/** Creates the key/fill lighting and capture component/render target. */
		bool CreateCaptureResources();
		/** Applies the demonstrated scene flags and selected HDR or LDR source. */
		bool ConfigureCapture(bool finalColor);
		/** Applies the selected capture source gamma and engine show flags. */
		bool ConfigureShowFlagsAndTarget(bool finalColor);
		/** Lazily creates the shared private world and verifies facts for the active subject. */
		void PreparePrivateWorld();
		/** Prepares the class, actor bounds, request capture component, and target. */
		void PrepareSubject();
		/** Attempts the C0 submission only after one iteration beyond preparation. */
		void SubmitHdrWarmupWhenAdmitted();
		/** Attempts the C10 submission after ten admitted iterations from C0. */
		void SubmitHdrCaptureAfterWarmup();
		/** Advances the current HDR or final-color asynchronous readback. */
		void AdvanceReadbackPhase();
		/** Saves completed HDR samples before allowing final-color target reconfiguration. */
		void CompleteHdrReadback();
		/** Processes final-color samples and begins request-resource retirement. */
		void CompleteFinalReadback();
		/** Fails a pass only after its readback backend reports safe native retirement. */
		void RetireFailedReadback(BuildingSpriteReadback& readback);
		/** Processes both packed captures into the immutable BM-02 payload. */
		void ProcessFinalCapture();
		/** Retires submitted scene work behind the renderer's nonblocking request fence. */
		bool RetireSubmittedSceneBehindFence();
		/** Marks a failed request terminal only after safe pre-submission cleanup. */
		void FailRequest(CaptureResult::Completion::Failure failure);
		/** Creates and registers one configured directional light. */
		bool CreateDirectionalLight(std::size_t lightIndex);
		/** Configures the source-backed key/fill light values. */
		bool ConfigureDirectionalLight(std::size_t lightIndex);
		/** Destroys an already unregistered component before world cleanup. */
		bool DestroyComponent(SDK::UActorComponent* component);
		/** Destroys an actor before private-world cleanup. */
		bool DestroyActor(SDK::AActor*& actor);
		/** Unregisters and destroys one shared light component during service teardown. */
		bool RetireLight(std::size_t lightIndex);
		/** Releases request-specific resources only after scene work has been retired. */
		bool RetireWithoutSubmittedScene();
		/** Unregisters and destroys request-specific capture and actor resources. */
		bool DestroyRequestContents();
		/** Retires request owners behind their render fence while preserving the shared world and rig. */
		bool RetireRequestResources();
		/** Unregisters and destroys the shared lighting rig during service teardown. */
		bool DestroySharedLighting();
		/** Cleans up the private world only during explicit service teardown. */
		bool CleanupPrivateWorld();
		/** Releases actor, capture-component, target, and class roots after request retirement. */
		bool ReleaseRequestRoots();
		/** Releases shared light and world roots after service retirement. */
		bool ReleaseWorldRoots();
		/** Constructs, inserts, and polls a NativeApi render fence without blocking. */
		bool StartFence(FencePurpose purpose);
		/** Polls and destroys the expected render fence once its queued work completes. */
		bool PollFence(FencePurpose purpose);
		/** Begins service-directed shutdown without freeing resources still in use by BM-07. */
		void RequestServiceShutdown();
		/** Advances request and shared-world retirement; true means the renderer can be released. */
		bool AdvanceServiceShutdown();
		/** Cancels and retires the active request before shared-world shutdown. */
		bool AdvanceRequestShutdown();
		/** Retires the shared lighting rig and world behind ordered render work. */
		bool AdvanceSharedWorldShutdown();
		/** Releases one retained UObject after its component/actor/world role is retired. */
		bool ReleaseObject(SDK::UObject* object);
		/** Checks NativeApi availability and confirms this is the Unreal game thread. */
		bool IsGameThread() const;
		/** Converts an owned UTF-8 object path into an owned wide path. */
		bool ConvertObjectPath(std::string_view objectPath, std::wstring& widePath) const;
		/** Resolves a resident UObject class by copied path without loading or scanning. */
		SDK::UClass* FindResidentClass(std::wstring_view objectPath) const;
		/** Creates and retains a UObject from a resolved class using the existing native call. */
		SDK::UObject* CreateRetainedObject(SDK::UClass* objectClass, SDK::UObject* outer);
		/** Submits one capture scene and records the outstanding GPU-work boundary. */
		bool SubmitCaptureScene();

		CaptureRequest activeRequest_;
		CaptureResult::Completion completion_;
		BuildingSpriteReadback hdrReadback_;
		BuildingSpriteReadback finalReadback_;
		std::vector<std::uint16_t> hdrPixels_;
		std::shared_ptr<const Cache::ProcessedSpritePayload> pendingPayload_;
		Phase phase_ = Phase::Idle;
		SDK::UClass* actorClass_ = nullptr;
		SDK::UWorld* privateWorld_ = nullptr;
		SDK::AActor* subjectActor_ = nullptr;
		std::array<SDK::UDirectionalLightComponent*, 2> directionalLights_{};
		SDK::USceneCaptureComponent2D* captureComponent_ = nullptr;
		SDK::UTextureRenderTarget2D* renderTarget_ = nullptr;
		double boundsCenter_[3]{};
		double boundsExtent_[3]{};
		double pixelsPerMapUnit_ = 0.0;
		std::uint64_t admittedServiceIterations_ = 0;
		std::uint64_t lastAcceptedRequestSequence_ = 0;
		std::uint64_t activeGameWorldSessionGeneration_ = 0;
		std::uint64_t preparedIteration_ = 0;
		std::uint64_t hdrWarmupIteration_ = 0;
		alignas(8) unsigned char retirementFenceStorage_[8]{};
		FencePurpose fencePurpose_ = FencePurpose::None;
		CaptureResult::Completion::Failure pendingFailure_ = CaptureResult::Completion::Failure::None;
		bool hasActiveRequest_ = false;
		bool resultEligible_ = true;
		bool sceneWorkSubmitted_ = false;
		bool finalColorPassSubmitted_ = false;
		bool readbackStarted_ = false;
		bool requestSceneRetirementFenceComplete_ = false;
		bool privateWorldInitialized_ = false;
		bool lightingRigSetupStarted_ = false;
		bool lightingRigReady_ = false;
		bool sharedWorldUsable_ = true;
		bool serviceShutdownRequested_ = false;
		bool serviceRetired_ = false;
		bool requestRetirementFenceComplete_ = false;
		bool lightingRetirementFenceComplete_ = false;
		bool serviceRetirementFenceComplete_ = false;
		bool captureComponentRegistered_ = false;
		bool captureComponentDestroyed_ = false;
		bool subjectActorDestroyed_ = false;
		bool actorDestroyFailed_ = false;
		bool privateWorldCleaned_ = false;
		std::array<bool, 2> lightComponentsRegistered_{};
		std::array<bool, 2> lightComponentsDestroyed_{};
	};
}
