// Owns one plugin-lifetime capture renderer, its requested lifecycle, private world and lighting rig.
// Request-scoped sprite resources use the existing capture/readback retirement operations.
#pragma once

#include "CaptureRequest.h"
#include "CaptureResult.h"
#include "BuildingSpriteReadback.h"
#include "../Visuals/SpriteGeometry.h"
#if defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
#include "CaptureFeedback.h"
#endif

#include <array>
#include <atomic>
#if defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
#include <chrono>
#endif
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

namespace Services::Map::Buildings::Scheduling
{
	class BuildingScheduler;
	class SpriteFeedbackQueue;
	class SpriteRequestQueue;
}

namespace Services::Map::Buildings::Capture
{
	/** Owns one plugin-lifetime private world and shared lighting rig plus one active capture request.
	 * Queue-driven advancement admits one moved request and reports terminal feedback through
	 * the independent FIFOs. The renderer never observes scheduler status or disk/cache ownership.
	 * All methods that touch UObject or NativeApi state require the game thread. Gameplay-session stop
	 * retires only active request resources; the shared world and lighting remain renderer-owned.
	 * Submitted scene work remains owned until readback or an ordered retirement fence proves safety.
	 */
	class BuildingSpriteRenderer final
	{
	public:
#if defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		/** Fixed-size numeric evidence for one renderer-owned capture interval. */
		struct DiagnosticsSnapshot
		{
			/** Repeated elapsed wall-time aggregate; MeanMs is derived by the reporter. */
			struct Duration
			{
				std::uint64_t Samples = 0;
				double TotalMs = 0.0;
				double MaxMs = 0.0;

				/** Adds one observed elapsed duration. */
				void Add(double elapsedMs) noexcept
				{
					++Samples;
					TotalMs += elapsedMs;
					if (elapsedMs > MaxMs)
					{
						MaxMs = elapsedMs;
					}
				}
			};

			double IntervalMs = 0.0;
			/** End-to-end elapsed request time from retention through terminal FIFO enqueue. */
			Duration AcceptedRequestLifetime;
			/** Synchronous elapsed game-thread time inside accepted Advance() calls. */
			Duration ActiveAdvance;
			Duration PrivateWorldPreparation;
			Duration SubjectPreparation;
			Duration CaptureSceneSubmission;
			/** Entries follow CaptureResult::Acceptance::Status declaration order. */
			std::array<std::uint64_t, 4> AcceptanceStatusCounts{};
			/** Entries follow CaptureSubmissionPurpose: HdrWarmup, HdrCapture, FinalColor. */
			std::array<std::uint64_t, 3> CaptureSubmissionAttempts{};
			/** Counts submissions returning true; this is not GPU completion evidence. */
			std::array<std::uint64_t, 3> CaptureSubmissionSuccesses{};
			Duration HdrReadbackStart;
			Duration HdrReadbackAdvance;
			Duration FinalReadbackStart;
			Duration FinalReadbackAdvance;
			/** Entries follow BuildingSpriteReadback::Status declaration order. */
			std::array<std::uint64_t, 4> HdrReadbackStatusCounts{};
			std::array<std::uint64_t, 4> FinalReadbackStatusCounts{};
			Duration HdrReadbackCompletion;
			Duration FinalReadbackCompletion;
			Duration FinalCaptureProcessing;
			Duration SpriteProcessing;
			/** Entries follow SpriteProcessingResult::Status declaration order. */
			std::array<std::uint64_t, 4> SpriteProcessingStatusCounts{};
			Duration AdvanceRetirement;
			Duration SubmittedSceneRetirement;
			Duration RequestResourceRetirement;
			/** Entries follow CaptureFeedback::Outcome declaration order. */
			std::array<std::uint64_t, 3> TerminalFeedbackOutcomeCounts{};
			/** Entries follow CaptureResult::Completion::Failure declaration order. */
			std::array<std::uint64_t, 8> TerminalFailureCounts{};
		};

		/** Hands off one due nonempty numeric interval without changing active capture state. */
		bool TakeDiagnosticsSnapshot(DiagnosticsSnapshot& output) noexcept;
#endif

		/** Does not perform native cleanup; lifecycle work is processed by the engine callback. */
		~BuildingSpriteRenderer() = default;
		/** Requests one-time initialization without performing native work on the caller. */
		bool RequestInitialization() noexcept;
		/** Requests gameplay capture admission when no Stop command remains pending. */
		bool RequestStart() noexcept;
		/** Requests nonblocking cancellation and retirement of session-owned capture resources. */
		void RequestStop() noexcept;
		/** Permanently requests capturer cleanup and blocks any later Start request. */
		void RequestExit() noexcept;
		/** Processes the requested lifecycle command on the Unreal game thread. */
		void ProcessLifecycle();

		/** Capturer lifecycle command requested by its plugin/service owner. */
		enum class CapturerLifecycleState : std::uint8_t
		{
			Load = 0,
			Init,
			Start,
			Stop,
			Exit
		};

	private:
		friend class Scheduling::BuildingScheduler;

#if defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		/** Identifies which existing capture submission call site performed the operation. */
		enum class CaptureSubmissionPurpose : std::uint8_t
		{
			HdrWarmup,
			HdrCapture,
			FinalColor
		};

		/** Adds one active-request acceptance observation to the current interval. */
		void RecordAcceptanceStatus(CaptureResult::Acceptance::Status status) noexcept;
		/** Invokes one existing scene submission while recording its caller purpose. */
		bool SubmitCaptureSceneForPurpose(CaptureSubmissionPurpose purpose);
		/** Adds one actual readback progress result to its pass-specific category array. */
		void RecordReadbackStatus(bool hdrPass, BuildingSpriteReadback::Status status) noexcept;
		/** Records one successfully enqueued accepted-request outcome and its lifetime. */
		void RecordTerminalFeedback(
			CaptureFeedback::Outcome outcome,
			CaptureResult::Completion::Failure failure) noexcept;
		/** Times one existing readback Start call for its pass. */
		bool StartReadbackForDiagnostics(bool hdrPass, BuildingSpriteReadback& readback);
		/** Times and records one existing nonblocking readback Advance call. */
		BuildingSpriteReadback::Status AdvanceReadbackForDiagnostics(
			bool hdrPass,
			BuildingSpriteReadback& readback);
		/** Clears interval aggregates and restarts their timestamp without touching requests. */
		void ResetDiagnosticsInterval() noexcept;
		/** Tests fixed-size observations to suppress reports for an idle interval. */
		bool HasDiagnosticsActivity() const noexcept;

		/** Adds one method's elapsed time on every ordinary return path. */
		struct DurationScope final
		{
			explicit DurationScope(DiagnosticsSnapshot::Duration& duration) noexcept;
			~DurationScope() noexcept;

		private:
			DiagnosticsSnapshot::Duration& duration_;
			std::chrono::steady_clock::time_point started_;
		};

		DiagnosticsSnapshot diagnostics_{};
		std::chrono::steady_clock::time_point diagnosticsIntervalStart_{};
		std::chrono::steady_clock::time_point activeRequestStartedAt_{};
		bool activeRequestLifetimeActive_ = false;
#endif

		/** Capturer execution progress owned only by the engine game-thread callback. */
		enum class CapturerSubState : std::uint8_t
		{
			None = 0,
			InitStep1,
			InitStep2,
			InitCompleted,
			Active,
			StopReleasing,
			StopCompleted,
			ExitCleanup,
			ExitReady
		};

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
			RequestRetirement
		};

		/** Creates inert plugin-lifetime renderer state at the service-configured map scale. */
		explicit BuildingSpriteRenderer(double pixelsPerMapUnit);
		/** Prevents copies from creating ambiguous native-resource ownership. */
		BuildingSpriteRenderer(const BuildingSpriteRenderer&) = delete;
		/** Prevents copies from creating ambiguous native-resource ownership. */
		BuildingSpriteRenderer& operator=(const BuildingSpriteRenderer&) = delete;
		/** Prevents moves from invalidating service-directed native-resource ownership. */
		BuildingSpriteRenderer(BuildingSpriteRenderer&&) = delete;
		/** Prevents moves from invalidating service-directed native-resource ownership. */
		BuildingSpriteRenderer& operator=(BuildingSpriteRenderer&&) = delete;

		/** Validates and moves a request into the retained active slot.
		 * @param request Owned request, left untouched when admission is rejected.
		 * @return Accepted or the existing request validation rejection classification.
		 */
		CaptureResult::Acceptance AcceptOwned(CaptureRequest&& request);
		/** Advances one retained request or admits one FIFO request on the Unreal game thread.
		 * A newly dequeued request is initialized only; native advancement begins on a later call.
		 * @param requestQueue Scheduler-owned FIFO supplying owned capture requests.
		 * @param feedbackQueue Scheduler-owned FIFO receiving retired terminal results.
		 */
		void AdvanceQueueDrivenCapture(
			Scheduling::SpriteRequestQueue& requestQueue,
			Scheduling::SpriteFeedbackQueue& feedbackQueue);
		/** Moves an accepted request into existing retained state and resets its per-request fields.
		 * @param request Validated owned request ready to enter the capture lifecycle.
		 */
		void RetainAcceptedRequest(CaptureRequest&& request);
		/** Determines whether idle renderer state permits one FIFO dequeue.
		 * @return True only when no active/native request work or shutdown blocks admission.
		 */
		bool CanAdmitQueuedRequest() const;
		/** Reports whether the correlated terminal request is fully retired and can be released.
		 * @param correlation Request identity that must match both retained request and completion.
		 * @return True only when readbacks, scene work, native owners and request fence are retired.
		 */
		bool CanAcknowledgeCompletion(CaptureRequest::Correlation correlation) const;
		/** Returns terminal rejection feedback for one dequeued request that could not be admitted.
		 * @param request Dequeued owned value retained when AcceptOwned rejected it.
		 * @param acceptance Existing admission rejection and matching request correlation.
		 * @param feedbackQueue FIFO that takes ownership of the terminal feedback value.
		 */
		void EmitRejectedRequestFeedback(
			CaptureRequest&& request,
			const CaptureResult::Acceptance& acceptance,
			Scheduling::SpriteFeedbackQueue& feedbackQueue);
		/** Enqueues one safe terminal result and releases its request-local state.
		 * @param feedbackQueue FIFO that takes move-only payload ownership before local release.
		 */
		void EmitTerminalFeedback(Scheduling::SpriteFeedbackQueue& feedbackQueue);
		/** Advances at most one admitted game-thread operation or readback poll. */
		void Advance();
		/** Retries safe cleanup after a pre-submission cancellation or failure. */
		void AdvanceRetirement();
		/** Runs the phase-specific world, subject, warm-up, or capture step. */
		void AdvanceCurrentPhase();
		/** Revokes result eligibility; submitted scene work remains owned until BM-07 retires it. */
		void Cancel(CaptureRequest::Correlation correlation);
		/** Advances Stop or Exit cleanup while keeping request feedback correlated. */
		void AdvanceLifecycleRetirement();
		/** Releases shared lights and private-world roots after request work is retired. */
		bool CleanupPersistentWorld();
		/** Releases retained light roots and cleans the private-world root in dependency order. */
		bool ReleasePersistentWorldRoots();
		/** Releases terminal request state after its feedback value enters the FIFO. */
		void AcknowledgeCompletion(CaptureRequest::Correlation correlation);
		/** Continues or cancels HDR work after BM-07 proves scene/readback retirement. */
		bool AcknowledgeHdrPassRetirement(bool readbackWasSubmitted, bool readbackAndSceneRetired);
		/** Returns capture-space XYZ bounds owned by the active subject request. */
		Visuals::SpriteCaptureBounds GetCaptureBounds() const;

		/** Resolves and retains a copied class path using existing resident-object services. */
		bool ResolveAndRetainActorClass(std::string_view objectPath);
		/** Creates and initializes the independently owned private world. */
		bool CreatePrivateWorld();
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
		/** Lazily creates the shared private world and prepares its lighting rig. */
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
		/** Unregisters and destroys one persistent directional light. */
		bool DestroyDirectionalLight(std::size_t lightIndex);
		/** Destroys a request-owned component after submitted request work is retired. */
		bool DestroyComponent(SDK::UActorComponent* component);
		/** Destroys the request-owned subject actor after submitted request work is retired. */
		bool DestroyActor(SDK::AActor*& actor);
		/** Releases request-specific resources only after scene work has been retired. */
		bool RetireWithoutSubmittedScene();
		/** Unregisters and destroys request-specific capture and actor resources. */
		bool DestroyRequestContents();
		/** Retires request owners behind their render fence while preserving the shared world and rig. */
		bool RetireRequestResources();
		/** Releases actor, capture-component, target, and class roots after request retirement. */
		bool ReleaseRequestRoots();
		/** Constructs, inserts, and polls a NativeApi render fence without blocking. */
		bool StartFence(FencePurpose purpose);
		/** Polls and destroys the expected render fence once its queued work completes. */
		bool PollFence(FencePurpose purpose);
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
		/** Executes one lifecycle step after the engine callback confirms game-thread ownership. */
		void AdvanceLifecycleState(CapturerLifecycleState requestedState);
		/** Activates the persistent renderer after its one-time initialization state. */
		void AdvanceStartLifecycle();
		/** Retires session request ownership and acknowledges completed Stop. */
		void AdvanceStopLifecycle();
		/** Retires request ownership and releases persistent native objects for Exit. */
		void AdvanceExitLifecycle();

		CaptureRequest activeRequest_;
		CaptureResult::Completion completion_;
		BuildingSpriteReadback hdrReadback_;
		BuildingSpriteReadback finalReadback_;
		std::vector<std::uint16_t> hdrPixels_;
		std::unique_ptr<const Cache::ProcessedSpritePayload> pendingPayload_;
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
		std::uint64_t preparedIteration_ = 0;
		std::uint64_t hdrWarmupIteration_ = 0;
		alignas(8) unsigned char retirementFenceStorage_[8]{};
		FencePurpose fencePurpose_ = FencePurpose::None;
		CaptureResult::Completion::Failure pendingFailure_ = CaptureResult::Completion::Failure::None;
		std::atomic<CapturerLifecycleState> capturerState_{CapturerLifecycleState::Load};
		CapturerSubState capturerSubState_{CapturerSubState::None};
		Scheduling::SpriteFeedbackQueue* lifecycleFeedbackQueue_ = nullptr;
		bool exitCleanupFenceComplete_ = false;
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
		bool privateWorldCleaned_ = false;
		bool requestRetirementFenceComplete_ = false;
		bool captureComponentRegistered_ = false;
		bool captureComponentDestroyed_ = false;
		bool subjectActorDestroyed_ = false;
		bool actorDestroyFailed_ = false;
		std::array<bool, 2> directionalLightRegistered_{};
		std::array<bool, 2> directionalLightDestroyed_{};
	};
}
