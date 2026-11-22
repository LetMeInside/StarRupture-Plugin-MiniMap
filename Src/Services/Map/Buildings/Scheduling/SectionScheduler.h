// Owns Building Map acquisition and advances resumable rendering state held by each Section.
#pragma once

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
#include "BuildingScheduler.h"
#endif

#include "../BuildingCandidatePublication.h"
#include "../Rendering/SectionRendering.h"
#include "../Visuals/BuildingGeometryPreparation.h"
#include "../Visuals/BuildingRenderSceneInput.h"
#include "../Visuals/PreparedBuildingGeometry.h"
#include "../Cache/OwnedSpriteKey.h"
#include "../SplinePolicy.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Services::Map::Buildings
{
	class BuildingMapSection;
}

namespace Services::Map::Buildings::Cache
{
	class SpriteSourceManager;
	class ProcessedSpritePayload;
}

namespace Services::Map::Buildings::Visuals
{
	class PreparedBuildingGeometry;
}

namespace Services::Map::Buildings::Scheduling
{
	class BuildingScheduler;
	class SectionSchedulingCoordinator;
}

namespace Services::Map::Buildings::Capture
{
	class BuildingSpriteRenderer;
}

namespace Services::Map::Buildings::Scheduling
{
	/** Coordinates service-owned sprite acquisition and executes rendering state held by service-owned Sections.
	 * The coordinator supplies Sections directly; unfinished jobs are not indexed or owned here.
	 * SpriteSourceManager and its disk cache outlive this scheduler and BuildingScheduler.
	 * The renderer object survives gameplay stops under this plugin-lifetime owner. Construction creates
	 * CPU-side owners only; the renderer lifecycle callback advances cancellation independently.
	 * Completed image ownership remains exclusively with the supplied BuildingMapSection; the commit
	 * outcome reports whether adoption succeeded.
	 */
	class SectionScheduler final
	{
	public:
		/** Retains Section-local known-sprite preparation evidence and synchronous ready payload owners.
		 * KnownRequirementsProcessed describes a validated occurrence set; readiness remains
		 * determined from each occurrence's source and resource outcome.
		 * Aggregate counters serve the synchronous API; resumable Section preparation uses outcomes.
		 * Section jobs transfer the exact acquired payload into the Section-owned sprite occurrence.
		 */
		struct KnownSpritePreparationResult
		{
			/** Distinguishes a processed Section batch from publication/key validity failures. */
			enum class Status
			{
				KnownRequirementsProcessed,
				InvalidPublication,
				InvalidSourceRequirement
			};

			/** Validation state for synchronous evidence or a resumable Section generation. */
			Status PreparationStatus = Status::InvalidPublication;
			/** Non-owning publication identity valid only while the caller retains that owner. */
			const BuildingCandidatePublication* PublicationIdentity = nullptr;
			/** Number of known requirements with an outstanding or newly queued acquisition. */
			std::size_t PendingCount = 0;
			/** Number of known requirements with an established session-local terminal failure. */
			std::size_t FailedCount = 0;
			/** Number of known requirements whose cache/payload access is temporarily unavailable. */
			std::size_t UnavailableCount = 0;
			/** Number of ready CPU sprites that cannot draw because their placement is unknown. */
			std::size_t ReadyWithoutPlacementCount = 0;
			/** Number of ready placed sprites whose independent numerical quad could not be built. */
			std::size_t UnresolvedSpriteGeometryCount = 0;
			/** Number of malformed or unrepresentable source keys. */
			std::size_t InvalidRequirementCount = 0;
			/** One source-ordered acquisition outcome for every publication occurrence. */
			struct OccurrenceOutcome
			{
				/** Distinguishes preserved acquisition and preparation outcomes. */
				enum class Status
				{
					NotProcessed,
					UnknownSource,
					PendingNativeSourceAcquisition,
					TemporarilyUnavailableSource,
					TerminalSourceFailure,
					PendingSpriteAcquisition,
					TerminalSpriteCaptureFailure,
					TerminalSpriteGeometryFailure,
					TemporarilyUnavailableSprite,
					InvalidSpriteRequirement,
					MissingSpritePlacement,
					UnresolvedSpriteGeometry,
					UnknownSpritePass,
					UnresolvedInputs,
					ReadySprite,
					ReadySpline
			};

				/** Exact publication-local occurrence ordinal. */
				std::uint64_t PublicationOrdinal = 0;
				/** Source classification and status copied for explicit evidence. */
				BuildingCandidateSourceKind SourceKind = BuildingCandidateSourceKind::Unknown;
				BuildingCandidateSourceStatus SourceStatus = BuildingCandidateSourceStatus::Unknown;
				/** Source pass evidence remains visible even when acquisition is pending or failed. */
				BuildingCandidateSpritePass SpritePass = BuildingCandidateSpritePass::Unknown;
				/** Native acquisition evidence from the paired candidate occurrence. */
				bool NativeFactsComplete = false;
				bool IsPendingOrUnresolved = true;
				/** Outcome of the existing sprite preparation operation, when applicable. */
				Status PreparationStatus = Status::NotProcessed;
				/** Sprite-specific status retained independently from scene readiness classification. */
				Status SpritePreparationStatus = Status::NotProcessed;
			};
			std::vector<OccurrenceOutcome> OccurrenceOutcomes;
		};

		/** Owns numerical curve inputs while preserving independent source and profile evidence.
		 * Prepared segments contain only converted source coordinates. The root transform is a
		 * separate optional value for surface projection and is never applied to those coordinates.
		 */
		struct SplineCurvePreparationResult
		{
			/** Describes keyed conversion and coordinate eligibility of segments only.
			 * RootTransformStatus remains independent; neither status certifies a complete contributor.
			 */
			enum class CurveStatus
			{
				NotPrepared,
				ConvertedWorldSpace,
				NotSpline,
				MissingKeyedCurve,
				UnsupportedCoordinateSpace
			};
			/** Describes whether the copied placement can serve as the surface root transform. */
			enum class RootTransformStatus
			{
				NotApplicable,
				Prepared,
				MissingPlacement,
				InvalidPlacement
			};

			/** Original acquisition state; numerical conversion never promotes this value. */
			BuildingCandidateSourceStatus SourceStatus = BuildingCandidateSourceStatus::Unknown;
			/** Independent profile acquisition state used by input composition and source-status mapping. */
			SplineProfileAcquisitionStatus ProfileStatus = SplineProfileAcquisitionStatus::Unknown;
			/** Keyed conversion outcome returned by the existing SplineGeometry operation. */
			Visuals::SplineNumericalIssue ConversionIssue = Visuals::SplineNumericalIssue::None;
			/** Curve-input eligibility, separate from acquisition and profile status. */
			CurveStatus PreparationStatus = CurveStatus::NotPrepared;
			/** Independent placement-to-root conversion result. */
			RootTransformStatus RootStatus = RootTransformStatus::NotApplicable;
			/** Present only for a finite, invertible placement with a unit XYZW quaternion. */
			std::optional<Visuals::SplineSurfaceTransform> RootTransform;
			/** Caller-owned converted curve segments; no view into publication-owned points remains. */
			std::vector<Visuals::SplineCurveSegment> Segments;
		};

		/** Owns one occurrence's composed spline inputs and source/profile eligibility evidence.
		 * PublicationOrdinal distinguishes duplicate occurrences. Authored surface inputs retain
		 * the relative order of their eligible source alternatives; a synthetic ribbon owns one surface.
		 * BorrowRenderInput creates spans only on request; this object must remain alive and unchanged
		 * for their full synchronous use. Composed does not certify candidate completeness.
		 */
		struct SplineRenderInputPreparationResult
		{
			/** Describes why a complete neutral render input was or was not composed. */
			enum class Status
			{
				NotAttempted,
				Composed,
				InvalidOccurrence,
				NotSpline,
				SourceNotComplete,
				ProfilesNotComplete,
				CurveNotEligible,
				RootTransformUnavailable,
				PolicyUnresolved,
				PolicyIndexMismatch,
				InvalidPolicy,
				InvalidSamplingDensity,
				NoPreparedSurface
			};

			/** Exact publication-local identity for this one occurrence, including duplicates. */
			std::uint64_t PublicationOrdinal = 0;
			/** Explicit source geometry intent, never inferred from profile acquisition status. */
			BuildingCandidateSplineGeometryMode GeometryMode =
				BuildingCandidateSplineGeometryMode::AuthoredProfiles;
			/** Converted curve, root and acquisition evidence owned by this result. */
			SplineCurvePreparationResult CurvePreparation;
			/** Eligible authored or synthetic surfaces with copied solid policy appearance. */
			std::vector<Visuals::BuildingSplineSurfaceRenderInput> SurfaceInputs;
			/** Existing renderer sampling defaults with service-derived pixel density. */
			Visuals::SplineSamplingPolicy SamplingPolicy{};
			/** Limited input-composition outcome; Composed does not certify geometry or rendering. */
			Status PreparationStatus = Status::NotAttempted;

			/** Clears values while retaining caller-owned vector capacities for a later preparation.
			 * Any previously borrowed render-input view becomes invalid after this call.
			 */
			void Reset() noexcept;

			/** Creates borrowed render spans only for a composed result.
			 * The spans remain valid while this result stays alive and unchanged; obtain a fresh view
			 * after moving or modifying the result. On failure, output is reset to default values.
			 * @param section Section receiving failure diagnostics for the borrowed descriptor.
			 * @param publication Selected immutable publication supplying the diagnostic content hash.
			 * @param output Receives spans borrowing this result's segment and surface vectors.
			 * @return True when a complete neutral input was composed and its owned arrays are available.
			 */
			bool BorrowRenderInput(
				const ::Services::Map::Buildings::BuildingMapSection& section,
				const BuildingCandidatePublication& publication,
				Visuals::BuildingSplineRenderInput& output) const noexcept;
		};

		/** Owns per-occurrence evidence without owning its publication.
		 * Resumable sprite geometry and payload ownership live directly in the Section scene; only
		 * spline inputs are retained here. No final Section membership or image completion is implied.
		 */
		struct SectionScenePreparationResult
		{
			/** Complete means all Section-local occurrences are ready or have terminally omittable outcomes. */
			enum class Readiness
			{
				InvalidPublication,
				InvalidSection,
				InvalidPreparationInputs,
				PendingNativeSourceAcquisition,
				PendingSpriteAcquisition,
				TemporarilyUnavailableResources,
				UnresolvedInputs,
				CompleteSceneInputs
			};

			/** Per-publication-occurrence preparation evidence, preserving duplicates and source order. */
			std::vector<KnownSpritePreparationResult::OccurrenceOutcome> OccurrenceOutcomes;
			/** Owned spline composition results in Section occurrence order; initial borrowing sorts descriptors once. */
			std::vector<SplineRenderInputPreparationResult> SplinePreparations;
			/** Raw identity confirms sprite preparation came from this retained immutable publication. */
			const BuildingCandidatePublication* PublicationIdentity = nullptr;
			Readiness PreparationReadiness = Readiness::InvalidPublication;

			/** Clears owned values and invalidates any borrowed views into those values. */
			void Reset() noexcept;

			/** Builds ordered borrowed spline views for a complete scene and rejects incomplete inputs.
			 * @param section Section receiving failure diagnostics for this retained scene.
			 * @param publicationOwner Retained owner whose Section occurrences were prepared.
			 * @param splinePreparations Final moved owner of the curve and surface values being borrowed.
			 * @param scratch Receives sorted spline descriptors whose spans refer into those owned values.
			 * @param output Receives the established sorted-spline input span.
			 * @return True when this publication's prepared scene is CompleteSceneInputs.
			 */
			bool BorrowRenderSceneInput(
				const ::Services::Map::Buildings::BuildingMapSection& section,
				const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
				std::span<const SplineRenderInputPreparationResult> splinePreparations,
				std::vector<Visuals::BuildingSplineRenderInput>& scratch,
				Visuals::BuildingRenderSceneInput& output) const;
		};

		/** Reports successful image adoption or confirmed-empty completion without owning image bytes.
		 * Readiness and numerical status preserve the underlying render evidence. Committed means
		 * the Section adopted an image; CommittedEmpty means the complete scene was empty before
		 * rendering or its successfully finalized image contained only transparent clear pixels.
		 */
		struct SectionImageCommitResult
		{
			/** Distinguishes incomplete work, rendering failure, image adoption and empty completion. */
			enum class Status
			{
				InvalidOrIncompleteInput,
				NumericalRenderingFailed,
				Committed,
				CommittedEmpty
			};

			/** Outcome of this synchronous render/adopt or confirmed-empty attempt. */
			Status CommitStatus = Status::InvalidOrIncompleteInput;
			/** Existing R2S.B readiness evidence when preparation ran. */
			std::optional<SectionScenePreparationResult::Readiness> SceneReadiness;
			/** Existing numerical renderer outcome when rendering was invoked. */
			std::optional<Rendering::SectionRendering::Status> NumericalStatus;
			/** True only when confirmed-empty completion removed a previously stored image. */
			bool CompletedImageRemoved = false;
			/** Terminal no-drawable spline omissions from a finalized and successfully completed image. */
			Visuals::BuildingVisualsTerminalOmissionSummary TerminalSplineOmissions;
			/** Debug-only elapsed timings for the existing prepare, render and commit call boundaries. */
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			struct TimingData
			{
				double TotalMs = 0.0;
				double ScenePreparationMs = 0.0;
				double SceneBorrowMs = 0.0;
				double NumericalRenderingMs = 0.0;
				double TransparencyCheckMs = 0.0;
				double ImageAdoptionMs = 0.0;
				bool NumericalRenderingInvoked = false;
				bool TransparencyCheckInvoked = false;
				bool ImageAdoptionAttempted = false;
				Visuals::BuildingVisualsPhaseTimings NumericalPhases;
				std::size_t OrdinarySpriteInputCount = 0;
				std::size_t SplineContributorInputCount = 0;
				std::size_t OverlayLastSpriteInputCount = 0;
			};
			TimingData Timing;
#endif

			/** Clears prior caller-owned evidence before another attempt. */
			void Reset() noexcept;
		};

		/** Prevents copies from duplicating acquisition resources or borrowed dependencies. */
		SectionScheduler(const SectionScheduler&) = delete;
		/** Prevents copies from duplicating acquisition resources or borrowed dependencies. */
		SectionScheduler& operator=(const SectionScheduler&) = delete;
		/** Prevents moves from invalidating subordinate borrowed resource pointers. */
		SectionScheduler(SectionScheduler&&) = delete;
		/** Prevents moves from invalidating subordinate borrowed resource pointers. */
		SectionScheduler& operator=(SectionScheduler&&) = delete;
		/** Releases owned objects without initiating native cleanup. */
		~SectionScheduler();

	private:
		friend class SectionSchedulingCoordinator;
		/** Releases numerical image state in the supplied Section without resetting geometry or sources. */
		void ReleaseSectionImageContinuation(
			::Services::Map::Buildings::BuildingMapSection& section) noexcept;
		struct SectionJob;
		struct RetainedSectionScene;
		/** Carries the coordinator's current allowance and initial-sprite stopping boundary. */
		struct SectionPreparationVisit
		{
			std::chrono::steady_clock::time_point Deadline;
			std::uint64_t TickSequence;
			bool InitialSpritesOnly;
		};
		/** Reports whether this publication still needs initialization or initial sprite visits. */
		static bool NeedsInitialSpriteVisit(
			const ::Services::Map::Buildings::BuildingMapSection* section,
			const BuildingCandidatePublication& publication) noexcept;
		/** Describes one bounded advancement without conflating waits with render failures. */
		struct SectionJobAdvanceResult
		{
			enum class State
			{
				Pending,
				WaitingForResources,
				TerminalFailure,
				RestartRequired,
				AttemptFinished
			};

			State CurrentState = State::Pending;
			std::size_t WorkUnitsConsumed = 0;
			bool AwaitingImageDemand = false;
			bool HasSpriteRetries = false;
			Visuals::BuildingVisualsTerminalOmissionSummary TerminalSplineOmissions;
			std::optional<SectionScenePreparationResult::Readiness> SceneReadiness;
			std::optional<SectionImageCommitResult> FinishedAttempt;
		};
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		/** Retains scalar spline-stage evidence for one selected Section publication. */
		struct SplineDiagnosticMetrics
		{
			std::size_t SourceOccurrenceCount = 0;
			std::size_t SourceSplineOccurrenceCount = 0;
			std::size_t VisitedSplineOccurrenceCount = 0;
			std::size_t CompositionAttemptCount = 0;
			std::size_t CompositionSuccessCount = 0;
			std::size_t CompositionUnavailableCount = 0;
			std::size_t CompositionPendingCount = 0;
			std::size_t CompositionRejectedCount = 0;
			std::size_t ComposedSurfaceCount = 0;
			std::size_t SplineInputCount = 0;
			std::uint64_t BlockingOrdinal = 0;
			SectionScenePreparationResult::Readiness Readiness =
				SectionScenePreparationResult::Readiness::InvalidPublication;
			SplineRenderInputPreparationResult::Status LastCompositionStatus =
				SplineRenderInputPreparationResult::Status::NotAttempted;
			KnownSpritePreparationResult::OccurrenceOutcome::Status LastOccurrenceStatus =
				KnownSpritePreparationResult::OccurrenceOutcome::Status::NotProcessed;
			BuildingCandidateSourceKind BlockingKind = BuildingCandidateSourceKind::Unknown;
			KnownSpritePreparationResult::OccurrenceOutcome::Status BlockingStatus =
				KnownSpritePreparationResult::OccurrenceOutcome::Status::NotProcessed;
			bool HasBlockingOccurrence = false;
			bool AllOccurrencesVisited = false;
			bool SceneFinalizationAttempted = false;
			std::uint8_t ReportedNumericalStageMask = 0;
		};

		/** Deduplicates reports at resource, scene, numerical and GPU publication transitions. */
		struct SplineDiagnosticReportKey
		{
			SectionScenePreparationResult::Readiness Readiness =
				SectionScenePreparationResult::Readiness::InvalidPublication;
			KnownSpritePreparationResult::OccurrenceOutcome::Status BlockingStatus =
				KnownSpritePreparationResult::OccurrenceOutcome::Status::NotProcessed;
			BuildingCandidateSourceKind BlockingKind = BuildingCandidateSourceKind::Unknown;
			Visuals::BuildingGeometryPreparation::PreparationCursor::DiagnosticSnapshot::SplineStage NumericalStage =
				Visuals::BuildingGeometryPreparation::PreparationCursor::DiagnosticSnapshot::SplineStage::NotStarted;
			bool WaitingForResources = false;
			bool HasBlockingOccurrence = false;
			bool AllOccurrencesVisited = false;
			bool AnySplineVisited = false;
			bool AnyCompositionSucceeded = false;
			bool AnyCompositionUnavailable = false;
			bool AnyCompositionPending = false;
			bool AnyCompositionRejected = false;
			bool SceneFinalizationAttempted = false;
			bool RetainedSceneExists = false;
			bool HasRetainedSplineTriangles = false;
			bool HasPublishedSplineTriangles = false;
			bool GpuAssemblyActive = false;
			bool GeometryComplete = false;

			bool operator==(const SplineDiagnosticReportKey&) const = default;
		};

		/** Borrows only scalar counts and cursor state needed for one bounded diagnostic line. */
		struct SplineDiagnosticReportSnapshot
		{
			SplineDiagnosticReportKey Key;
			Visuals::BuildingGeometryPreparation::PreparationCursor::DiagnosticSnapshot Numerical;
			std::size_t RetainedSurfaceCount = 0;
			std::size_t RetainedTriangleCount = 0;
			std::size_t PublishedTriangleCount = 0;
			std::size_t AssemblyTriangleCount = 0;
			bool PublishedForSource = false;
			bool AssemblyForSource = false;
		};
#endif

		/** Stores immutable service configuration and creates acquisition resources without I/O.
		 * @param sectionSizeMapUnits Width and height of the service's square cell in map units.
		 * @param sectionResolutionPixels Width and height of the future square image in pixels.
		 * @param spriteCaptureDensityPixelsPerMapUnit Pixels per map unit used for sprite capture processing.
		 * @param buildingsCacheDirectory Service-validated absolute Buildings cache directory.
		 */
		SectionScheduler(
			double sectionSizeMapUnits,
			std::uint32_t sectionResolutionPixels,
			double spriteCaptureDensityPixelsPerMapUnit,
			std::filesystem::path buildingsCacheDirectory,
			Cache::SpriteSourceManager* spriteSourceManager);
		/** Retains the exact existing owner for equal content and replaces it for changed content.
		 * Equal ContentHash retains the Section's existing prepared occurrence storage.
		 * @param section Service-owned Section whose unfinished state is accessed directly.
		 * @param publicationOwner Non-null shared owner of the selected immutable publication.
		 */
		std::shared_ptr<const BuildingCandidatePublication> EnsureSectionJob(
			::Services::Map::Buildings::BuildingMapSection& section,
			const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner);
		/** Checks that an unfinished publication job is safe to retain while its Section is parked.
		 * @param section Section whose opaque scheduler state is inspected.
		 * @return True only for a valid, nonterminal job that owns resumable work.
		 */
		bool CanRetainIncompleteJobOnDemandWithdrawal(
			const ::Services::Map::Buildings::BuildingMapSection* section) const noexcept;
		/** Advances the supplied Section's retained publication job using authoritative job and Section state.
		 * @param section Exact Section whose stable numerical context is borrowed for this call.
		 * @param splinePolicies Existing immutable spline policy table.
		 * @param visit Shared deadline, retry tick and initial sprite boundary.
		 * @return Pending, waiting, restart-required, or a fully populated terminal attempt result.
		 */
		SectionJobAdvanceResult AdvanceSectionJob(
			::Services::Map::Buildings::BuildingMapSection& section,
			std::span<const SplinePolicy> splinePolicies,
			const SectionPreparationVisit& visit,
			bool renderSectionImages);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		/** Emits one scalar summary when this Section publication crosses a diagnostic milestone. */
		void ReportSplineProgressDiagnostics(
			const ::Services::Map::Buildings::BuildingMapSection& section,
			SectionJob& job,
			SectionJobAdvanceResult::State state);
		/** Captures existing scalar scheduler, scene, cursor and GPU publication state without scans. */
		SplineDiagnosticReportSnapshot CaptureSplineDiagnosticReportSnapshot(
			const ::Services::Map::Buildings::BuildingMapSection& section,
			const SectionJob& job,
			SectionJobAdvanceResult::State state) const noexcept;
		/** Selects only wait changes and coarse scene, numerical, and publication milestones. */
		static bool ShouldReportSplineDiagnosticSnapshot(
			const SplineDiagnosticReportSnapshot& snapshot,
			const SplineDiagnosticReportKey& previous,
			bool hasPrevious,
			bool firstReportForNumericalStage,
			SectionJobAdvanceResult::State state) noexcept;
		/** Formats and emits one bounded scalar snapshot using existing diagnostic logging. */
		void LogSplineDiagnosticSnapshot(
			const ::Services::Map::Buildings::BuildingMapSection& section,
			const SectionJob& job,
			const SplineDiagnosticReportSnapshot& snapshot,
			SectionJobAdvanceResult::State state) const;
		/** Returns a stable log label for the existing source-readiness enum. */
		static const char* ReadinessName(SectionScenePreparationResult::Readiness readiness) noexcept;
		/** Returns a stable log label for the existing candidate source-kind enum. */
		static const char* SourceKindName(BuildingCandidateSourceKind kind) noexcept;
		/** Returns a stable log label for the existing occurrence outcome enum. */
		static const char* OccurrenceStatusName(
			KnownSpritePreparationResult::OccurrenceOutcome::Status status) noexcept;
		/** Returns a stable log label for the existing spline composition enum. */
		static const char* CompositionStatusName(SplineRenderInputPreparationResult::Status status) noexcept;
		/** Returns a stable log label for the existing numerical spline cursor stage. */
		static const char* NumericalSplineStageName(
			Visuals::BuildingGeometryPreparation::PreparationCursor::DiagnosticSnapshot::SplineStage stage) noexcept;
#endif

		/** Starts gameplay scheduling against the persistent BuildingScheduler renderer.
		 * @return True when BuildingScheduler enters gameplay-active scheduling state.
		 */
		bool BeginCaptureGameWorldSession();

		/** Ends gameplay-session acquisition and begins cancellation of active request work. */
		void StopCaptureGameWorldSession();
		/** Returns the renderer used by the plugin's direct engine callback. */
		Capture::BuildingSpriteRenderer* GetCapturerLifecycleTarget() noexcept;
		/** Clears the configured sprite cache without changing acquisition-session state.
		 * @return True when the manager-owned disk cache clears its directory without error.
		 */
		bool ClearCache();
		/** Advances one existing sprite renderer operation and drains available feedback.
		 * Called only for an admitted gameplay scheduling tick on the serialized game thread.
		 */
		void AdvanceSpriteAcquisition();
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		/** Forwards the BuildingScheduler's due numeric acquisition snapshot without advancing it.
		 * @param output Receives the completed acquisition interval when one is due.
		 * @return True when the owned BuildingScheduler hands off a completed interval.
		 */
		bool TakeSpriteAcquisitionDiagnosticsSnapshot(
			BuildingScheduler::DiagnosticsSnapshot& output) noexcept;
#endif

		/** Advances monotonic occurrence preparation and distinguishes deadline yields from waits.
		 * @param section Exact service-owned Section matched against the selected publication.
		 * @param job Existing Section-owned state retaining the selected immutable publication.
		 * @param splinePolicies Existing immutable service policy table.
		 * @param visit Shared deadline and initial sprite boundary.
		 * @param deadlineDeferred Receives whether the visit yielded before finalization.
		 * @return Existing scene readiness; only CompleteSceneInputs installs retained state.
		 */
		SectionScenePreparationResult::Readiness PrepareRetainedSectionScene(
			::Services::Map::Buildings::BuildingMapSection& section,
			SectionJob& job,
			std::span<const SplinePolicy> splinePolicies,
			const SectionPreparationVisit& visit,
			bool& deadlineDeferred);
		/** Reuses matching completed geometry directly when no textured resource views are needed.
		 * @param section Section whose completed scene may match the selected immutable publication.
		 * @param job Retains image-operation state and receives the completed-scene marker.
		 * @return True when direct reuse is ready; false when source resources still need preparation.
		 */
		bool TryReuseCompletedSceneWithoutResources(
			::Services::Map::Buildings::BuildingMapSection& section,
			SectionJob& job) const;
		/** Initializes exact-publication occurrence state once and prepares only untouched ordinals.
		 * @param job Section-owned generation state retaining source facts and completed actions.
		 * @param publicationOwner Exact immutable publication selected for this Section.
		 * @return True when all ordinal/source correspondence is valid for this generation.
		 */
		bool InitializeSectionPreparationProgress(
			::Services::Map::Buildings::BuildingMapSection& section,
			SectionJob& job,
			const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner);
		/** Builds exact ordinal outcomes and a stable initial traversal for the selected publication. */
		bool InitializeSectionOccurrenceOrder(
			::Services::Map::Buildings::BuildingMapSection& section,
			SectionJob& job,
			const BuildingCandidatePublication& publication);
		/** Sorts the sprite prefix by valid source placement and leaves unresolved ordinals in source order.
		 * @param sourceData Immutable source records aligned to publication ordinals.
		 * @param occurrenceOrder Existing Section-local traversal array whose suffix remains unchanged.
		 * @param spriteCount Number of leading sprite ordinals to sort.
		 */
		static std::size_t SortSpriteOccurrenceOrder(
			const BuildingCandidateSourceData& sourceData,
			std::vector<std::size_t>& occurrenceOrder,
			std::size_t spriteCount);
		/** Advances untouched ordinals and retries only pending sprite-resource operations.
		 * @param job Section-owned progress with direct ordinal-indexed state.
		 * @param publicationOwner Exact publication supplying immutable source values.
		 * @param splinePolicies Existing service-owned immutable policy table.
		 * @param visit Shared deadline and initial sprite boundary.
		 * @return True when the visit yields at the sprite boundary or deadline.
		 */
		bool AdvanceSectionOccurrencePreparation(
			::Services::Map::Buildings::BuildingMapSection& section,
			SectionJob& job,
			const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
			std::span<const SplinePolicy> splinePolicies,
			const SectionPreparationVisit& visit);
		/** Retains an initial terminal source or capture omission in Section state. */
		void RetainTerminalSpriteOmission(
			::Services::Map::Buildings::BuildingMapSection& section,
			SectionJob& job,
			std::size_t ordinal);
		/** Retries eligible FIFO entries until the deadline or the first entry queued this tick. */
		void AdvanceSectionSpriteRetries(
			::Services::Map::Buildings::BuildingMapSection& section,
			SectionJob& job,
			const BuildingCandidateSourceData& sourceData,
			std::chrono::steady_clock::time_point deadline);
		/** Prepares one previously untouched occurrence and records its next outstanding action.
		 * @param job Section-owned occurrence state and sprite outcomes.
		 * @param publication Exact immutable publication used for source-record correspondence.
		 * @param candidate Immutable candidate occurrence aligned with ordinal.
		 * @param source Immutable source facts aligned with ordinal.
		 * @param ordinal Section-local publication ordinal to prepare.
		 * @param splinePolicies Immutable service-owned policy table.
		 */
		void PrepareSectionOccurrence(
			::Services::Map::Buildings::BuildingMapSection& section,
			SectionJob& job,
			const BuildingCandidatePublication& publication,
			const BuildingCandidateOccurrence& candidate,
			const BuildingCandidateSourceRecord& source,
			std::size_t ordinal,
			std::span<const SplinePolicy> splinePolicies);
		/** Acquires one sprite and writes valid geometry directly into its Section occurrence.
		 * @param section Section retaining the geometry and payload.
		 * @param job Source generation and per-ordinal retry state.
		 * @param requirement Immutable source placement and pass facts.
		 * @param ordinal Exact publication-local sprite ordinal.
		 * @param sourcePreparationBlocked Whether source facts prevent geometry retention.
		 */
		void PrepareSectionSpriteOccurrence(
			::Services::Map::Buildings::BuildingMapSection& section,
			SectionJob& job,
			const BuildingCandidatePublication& publication,
			const BuildingCandidateSourceRecord& source,
			std::size_t ordinal,
			bool sourcePreparationBlocked);
		/** Retries one queued sprite whose only unfinished work is resource availability.
		 * @param job Section-owned state retaining its validated key and result slot.
		 * @param source Immutable source record selected by the occurrence ordinal.
		 * @param ordinal Exact publication-local sprite occurrence.
		 */
		void RetrySectionSpriteResource(
			::Services::Map::Buildings::BuildingMapSection& section,
			SectionJob& job,
			const BuildingCandidateSourceRecord& source,
			std::size_t ordinal);
		/** Finalizes complete occurrence values into the retained three-pass scene exactly once.
		 * @param job Section-owned progress and target retained scene.
		 * @param publicationOwner Exact immutable publication used by final correspondence checks.
		 * @return True only when all prepared values assemble into a complete scene.
		 */
		bool FinalizeRetainedSectionScene(
			::Services::Map::Buildings::BuildingMapSection& section,
			SectionJob& job,
			const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner);
		/** Advances past resolved ordinals after the selected outcome changes.
		 * @param job Section-owned progress with a monotonic readiness cursor.
		 * @param ordinal Exact publication-local occurrence whose result changed.
		 */
		static void AdvanceFirstUnresolvedOrdinal(SectionJob& job, std::size_t ordinal) noexcept;
		/** Adds one pending sprite ordinal to the Section-owned retry queue.
		 * @param job Section-owned progress and queue endpoints.
		 * @param ordinal Exact occurrence ordinal to enqueue.
		 */
		void QueueSectionSpriteRetry(SectionJob& job, std::size_t ordinal);
		/** Removes and returns the head ordinal from the sprite-resource retry queue.
		 * @param job Section-owned progress and retry endpoints.
		 * @param ordinal Receives the removed ordinal.
		 */
		void PopSectionSpriteRetry(SectionJob& job, std::size_t& ordinal) noexcept;
		/** Returns whether a sprite-resource result may change under the same publication.
		 * @param status Existing occurrence status.
		 * @return True only for pending or temporarily unavailable sprite acquisition.
		 */
		static bool IsSpriteResourceRetryStatus(
			KnownSpritePreparationResult::OccurrenceOutcome::Status status) noexcept;
		/** Returns whether one preparation status prevents complete scene assembly.
		 * @param status Existing occurrence status.
		 * @return False only for ready inputs or established terminal omissions.
		 */
		static bool IsSectionOccurrenceBlockingStatus(
			KnownSpritePreparationResult::OccurrenceOutcome::Status status) noexcept;
		/** Maps the earliest outstanding ordinal to the established readiness category.
		 * @param job Section-owned progress whose readiness cursor is monotonic.
		 * @return Existing readiness classification without rescanning completed occurrences.
		 */
		SectionScenePreparationResult::Readiness GetSectionPreparationReadiness(
			const SectionJob& job) const noexcept;
		/** Resumes occurrence preparation or completes validated zero-candidate work before the deadline.
		 * @param section Exact service-owned Section for this publication coordinate.
		 * @param job Selected retained job and attempt state.
		 * @param splinePolicies Borrowed immutable service policy values.
		 * @param visit Shared deadline, retry tick and initial sprite boundary.
		 * @param result Receives readiness, state and consumed units.
		 * @return True when numerical continuation may proceed.
		 */
		bool PrepareSectionJobAttempt(
			::Services::Map::Buildings::BuildingMapSection& section,
			SectionJob& job,
			std::span<const SplinePolicy> splinePolicies,
			const SectionPreparationVisit& visit,
			bool renderSectionImages,
			SectionJobAdvanceResult& result);
		/** Advances numerical geometry directly into the Section's PreparingScene. */
		bool AdvanceSectionJobGeometry(
			::Services::Map::Buildings::BuildingMapSection& section,
			SectionJob& job,
			std::chrono::steady_clock::time_point deadline,
			SectionJobAdvanceResult& result);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		/** Reports one terminal Section attempt's accumulated BVH experiment measurements. */
		static void ReportBvhExperimentDiagnostics(
			const ::Services::Map::Buildings::BuildingMapSection& section,
			std::uint64_t publicationHash,
			Visuals::BuildingGeometryPreparation::Status status,
			const Visuals::BuildingGeometryPreparation::PreparationCursor& cursor);
#endif
		/** Resumes occurrence preparation or reuses the established retained scene.
		 * @param section Exact service-owned Section described by the publication.
		 * @param job Selected retained job whose scene state may be rebuilt or reused.
		 * @param splinePolicies Borrowed immutable service policy values.
		 * @param visit Shared deadline, retry tick and initial sprite boundary.
		 * @param result Receives preparation readiness and any restart/wait state.
		 * @return True when a valid retained scene is available for this advancement.
		 */
		bool PrepareSectionJobSceneForAttempt(
			::Services::Map::Buildings::BuildingMapSection& section,
			SectionJob& job,
			std::span<const SplinePolicy> splinePolicies,
			const SectionPreparationVisit& visit,
			SectionJobAdvanceResult& result);
		/** Runs one source-composition attempt and records its readiness and Debug duration. */
		bool PrepareRetainedSceneForAttempt(
			::Services::Map::Buildings::BuildingMapSection& section,
			SectionJob& job,
			std::span<const SplinePolicy> splinePolicies,
			const SectionPreparationVisit& visit,
			SectionJobAdvanceResult& result);
		/** Advances the existing persistent operation with its retained scene or an empty scene view.
		 * @param section Service-owned Section whose stable context is borrowed for rendering.
		 * @param job Retained operation, scene and attempt evidence.
		 * @param deadline Absolute end of the shared scheduling allowance.
		 * @param result Receives status and actual consumed units.
		 * @return True when numerical work completed or remains ready for commitment.
		 */
		bool AdvanceSectionJobNumerical(
			::Services::Map::Buildings::BuildingMapSection& section,
			SectionJob& job,
			std::chrono::steady_clock::time_point deadline,
			SectionJobAdvanceResult& result);
		/** Runs one numerical slice and translates its terminal status into the existing commit result. */
		bool RunSectionNumericalSlice(
			::Services::Map::Buildings::BuildingMapSection& section,
			SectionJob& job,
			const Visuals::BuildingRenderSceneInput& sceneInput,
			std::chrono::steady_clock::time_point deadline,
			SectionJobAdvanceResult& result);
		/** Transfers finalized bytes and resumes transparency/adoption commitment before the deadline. */
		void CommitSectionJobImage(
			::Services::Map::Buildings::BuildingMapSection& section,
			SectionJob& job,
			std::chrono::steady_clock::time_point deadline,
			SectionJobAdvanceResult& result);
		/** Takes finalized output once and routes exact finalized transparency to empty commitment. */
		bool ClassifyCompletedSectionImage(
			::Services::Map::Buildings::BuildingMapSection& section,
			SectionJob& job,
			SectionJobAdvanceResult& result);
		/** Finishes one attempt and releases numerical progress before its retained scene. */
		void FinishSectionJobAttempt(
			::Services::Map::Buildings::BuildingMapSection& section,
			SectionJob& job,
			SectionJobAdvanceResult& result);
		/** Destroys all attempt state while preserving the selected publication owner. */
		static void ResetSectionJobProgress(
			SectionJob& job,
			::Services::Map::Buildings::BuildingMapSection& section) noexcept;
		/** Rejects retained spline values that contain unsupported borrowed texture inputs.
		 * @param preparation Existing owned spline preparation to inspect.
		 * @return True only for the current solid-color policy with no borrowed texture pixels.
		 */
		static bool HasOnlyRetainableSplineAppearances(
			const ::Services::Map::Buildings::BuildingMapSection& section,
			std::uint64_t sourceContentHash,
			const SectionScenePreparationResult& preparation) noexcept;
		/** Builds the stable sorted spline array after sprite ownership has settled in the Section.
		 * @param retained Spline storage receiving its final descriptors.
		 * @param publicationOwner Exact owner used to validate the completed scene before spline view creation.
		 * @return True when the retained scene inputs are internally complete and stable.
		 */
		static bool InitializeRetainedSceneInputs(
			::Services::Map::Buildings::BuildingMapSection& section,
			SectionScenePreparationResult& preparation,
			RetainedSectionScene& retained,
			const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner);
		/** Validates completion after the existing numerical render call.
		 * @param commitResult Result containing the numerical status and receiving validation failure.
		 * @return True only for a Complete numerical result.
		 */
		bool IsCompletedRenderComplete(
			SectionImageCommitResult& commitResult) const;

		/** Encodes borrowed completed pixels and overwrites the coordinate-named PNG. */
		void ExportCompletedSectionImage(
			const ::Services::Map::Buildings::BuildingMapSection& section,
			std::span<const std::uint8_t> rgba8Bytes) const noexcept;

		/** Completes an empty Section after validated pre-render or post-render evidence.
		 * The caller has validated the exact Section publication and CompleteSceneInputs readiness.
		 * @param section Service-owned Section whose optional image may be removed.
		 * @param output Receives CommittedEmpty and whether image storage changed.
		 */
		void CommitEmptySection(
			::Services::Map::Buildings::BuildingMapSection& section,
			SectionImageCommitResult& output) const;

		/** Classifies renderer-finalized RGBA8 output as transparent using alpha bytes only.
		 * @param imageBytes Borrowed completed row-major RGBA8 bytes with renderer-guaranteed size.
		 * @return True when every destination pixel has zero alpha.
		 */
		bool IsFullyTransparentRgba8Image(std::span<const std::uint8_t> imageBytes) const noexcept;

		/** Validates that the publication's exact half-open cell corresponds to the supplied Section coordinates.
		 * @param section Service-owned Section whose signed grid location is authoritative.
		 * @param publication Immutable Section publication to match against the Section cell.
		 * @return True for the same square cell under the service's map-unit scale.
		 */
		bool IsPublicationForSection(
			const ::Services::Map::Buildings::BuildingMapSection& section,
			const BuildingCandidatePublication& publication) const;
		/** Compares coordinates within floating-point rounding tolerance, capped below cell separation.
		 * @param actual Publication rectangle coordinate.
		 * @param expected Coordinate derived from the Section grid index.
		 * @param sectionSize Positive map-space cell dimension.
		 * @return True only when the coordinates match the same cell location.
		 */
		static bool IsSectionRegionCoordinateMatch(double actual, double expected, double sectionSize) noexcept;
		/** Constructs the renderer's established zero-origin global-cell grid.
		 * @param section Section supplying signed global cell coordinates.
		 * @param sectionSizeMapUnits Square world-space cell size.
		 * @param sectionResolutionPixels Square destination dimension.
		 * @return Grid configuration with complementary world-units-per-pixel scale.
		 */
		static ImageGrid CreateSectionRenderingGrid(
			const ::Services::Map::Buildings::BuildingMapSection& section,
			double sectionSizeMapUnits,
			std::uint32_t sectionResolutionPixels);
		/** Preserves per-occurrence acquisition evidence before resource or numerical preparation.
		 * @param source Borrowed source record for one ordinal.
		 * @param candidate Matching candidate occurrence with native acquisition evidence.
		 * @param outcome Receives the preserved classification and preparation result.
		 * @return True when no later preparation should be attempted.
		 */
		static bool ProcessTerminalOrPendingSource(
			const BuildingCandidateSourceRecord& source,
			const BuildingCandidateOccurrence& candidate,
			KnownSpritePreparationResult::OccurrenceOutcome& outcome);
		/** Composes one spline occurrence once and classifies its preserved source outcome.
		 * @param publicationOwner Retained immutable publication for the occurrence.
		 * @param ordinal Exact Section-local spline occurrence.
		 * @param source Borrowed immutable source record.
		 * @param splinePolicies Immutable service-owned policy table.
		 * @param outcome Receives the established readiness/terminal classification.
		 * @param preparation Receives owned curve, profile, policy and surface values.
		 */
		void PrepareSplineOccurrence(
			const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
			std::uint64_t ordinal,
			const BuildingCandidateSourceRecord& source,
			std::span<const SplinePolicy> splinePolicies,
			KnownSpritePreparationResult::OccurrenceOutcome& outcome,
			SplineRenderInputPreparationResult& preparation) const;
		/** Converts one borrowed keyed curve and prepares its separate placement root transform.
		 * The caller retains the publication owner through this call; all returned segments and
		 * transform values are owned by output. World-space curve points are never transformed.
		 * @param source Borrowed immutable neutral source record from a retained publication.
		 * @param output Receives source evidence, numerical conversion outcome and owned values.
		 */
		void PrepareSplineCurve(
			const BuildingCandidateSourceRecord& source,
			SplineCurvePreparationResult& output) const;

		/** Composes numerical inputs for one ordinal in a retained publication.
		 * The resolved policy must be the service-owned value returned for resolvedPolicyIndex;
		 * that index must equal the source record's policy index. The publication and policy are
		 * borrowed only during this synchronous call. Output owns all resulting values.
		 * @param publicationOwner Retained owner of the immutable source publication.
		 * @param publicationOrdinal Exact occurrence ordinal in that publication.
		 * @param resolvedPolicyIndex Index used by the service's policy lookup.
		 * @param resolvedPolicy Service-owned policy pointer matching the resolved index, or null.
		 * @param output Receives composition status and caller-owned render-input values.
		 */
		void ComposeSplineRenderInput(
			const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
			std::uint64_t publicationOrdinal,
			std::uint16_t resolvedPolicyIndex,
			const SplinePolicy* resolvedPolicy,
			SplineRenderInputPreparationResult& output) const;

		/** Preserves source/geometry/curve/root distinctions and rejects incomplete prerequisites.
		 * @param source Borrowed occurrence source record.
		 * @param output Receives the precise first source or geometry composition status.
		 * @return True when the selected geometry mode and its required inputs are eligible.
		 */
		static bool CheckSplineSourcePrerequisites(
			const BuildingCandidateSourceRecord& source,
			SplineRenderInputPreparationResult& output);

		/** Validates explicit geometry intent against its required profile state and width.
		 * @param source Borrowed occurrence source record.
		 * @param output Receives the first geometry-intent failure.
		 * @return True when the intent has its required profile evidence and dimensions.
		 */
		static bool CheckSplineGeometryIntentPrerequisites(
			const BuildingCandidateSourceRecord& source,
			SplineRenderInputPreparationResult& output);

		/** Validates the caller-resolved service policy against the occurrence's source index.
		 * @param source Borrowed occurrence source record.
		 * @param resolvedPolicyIndex Index used for the service-owned lookup.
		 * @param resolvedPolicy Borrowed service-owned policy value.
		 * @param output Receives the precise policy failure status.
		 * @return True when the policy is present, matches the source index and contains valid values.
		 */
		static bool CheckSplinePolicyPrerequisites(
			const BuildingCandidateSourceRecord& source,
			std::uint16_t resolvedPolicyIndex,
			const SplinePolicy* resolvedPolicy,
			SplineRenderInputPreparationResult& output);

		/** Applies the fixed legacy minimum-map-resolution density and keeps other policy defaults.
		 * @param output Receives the policy only after finite positive density validation.
		 * @return True when the fixed spline pixel density is finite and positive.
		 */
		bool TryPrepareSplineSamplingPolicy(SplineRenderInputPreparationResult& output) const;

		/** Prepares authored alternatives directly into the ordered final surface inputs.
		 * @param source Borrowed immutable spline source record with acquired profile values.
		 * @param policy Validated service-owned solid appearance policy copied into each surface.
		 * @param output Owns eligible surfaces in their original relative order.
		 * @return True when at least one compatible surface was available.
		 */
		static bool AppendSplineRenderSurfaces(
			const BuildingCandidateSourceRecord& source,
			const SplinePolicy& policy,
			SplineRenderInputPreparationResult& output);

		/** Creates one synthetic thin ribbon using the resolved appearance policy.
		 * @param policy Validated service-owned solid appearance policy.
		 * @param halfWidthMapUnits Positive synthetic half-width in map units.
		 * @param output Owns the surface and its explicit geometry intent.
		 * @return True when one finite synthetic surface was appended.
		 */
		static bool AppendSyntheticThinRibbon(
			const SplinePolicy& policy,
			double halfWidthMapUnits,
			SplineRenderInputPreparationResult& output);

		/** Copies placement into a validated transform used only for surface projection.
		 * @param source Borrowed neutral source record.
		 * @param output Receives an optional root transform and its independent status.
		 */
		static void PrepareSplineRootTransform(
			const BuildingCandidateSourceRecord& source,
			SplineCurvePreparationResult& output);

		/** Copies one eligible authored alternative into generic surface values.
		 * @param source One neutral profile alternative borrowed for this call.
		 * @param output Receives an owned numerical surface input when eligible.
		 * @return True when the acquired values are supported for the numerical surface input.
		 */
		static bool PrepareSplineProfileAlternative(
			const SplineProfileAlternative& source,
			Visuals::BuildingSplineSurfaceRenderInput& output);

		/** Copies validated profile dimensions and fixed cross-section parameters.
		 * @param source Neutral acquired alternative values.
		 * @param output Receives generic numerical geometry fields.
		 * @return True when every copied geometry value is finite and bounds are valid.
		 */
		static bool CopySplineSurfaceValues(
			const SplineProfileAlternative& source,
			Visuals::BuildingSplineSurfaceRenderInput& output);

		/** Copies a valid neutral XYZW quaternion into a generic numerical transform.
		 * @param rotation Preconverted component quaternion from game-specific acquisition.
		 * @param output Receives the numerical component transform rotation.
		 * @return True when the quaternion is finite and unit length within renderer tolerance.
		 */
		static bool CopySplineSurfaceRotation(
			const std::array<double, 4>& rotation,
			Visuals::SplineSurfaceTransform& output);

		/** Copies aligned candidate/source facts into one owned outcome per ordinal.
		 * @param candidateData Borrowed occurrences from the retained publication.
		 * @param sourceData Aligned borrowed source records from the same publication.
		 * @param output Receives source-ordered per-occurrence evidence.
		 */
		static bool InitializeKnownSpriteOccurrences(
			const ::Services::Map::Buildings::BuildingMapSection& section,
			std::uint64_t sourceContentHash,
			const BuildingCandidateData& candidateData,
			const BuildingCandidateSourceData& sourceData,
			KnownSpritePreparationResult& output);
		/** Acquires one valid sprite subject and appends a drawable input when placement is known.
		 * @param publicationOrdinal Corresponding ordinal in the retained candidate publication.
		 * @param requirement Borrowed neutral source key and optional validated placement.
		 * @param ownedKey Validated owned copy of the canonical sprite subject.
		 * @param output Accumulates the outcome for this known requirement.
		 * @param updatePreparationCounts Whether to maintain synchronous aggregate counters.
		 */
		void PrepareSpriteRequirement(
			::Services::Map::Buildings::BuildingMapSection& section,
			SectionJob& job,
			const BuildingCandidateSourceRecord& requirement,
			std::size_t ordinal,
			bool sourcePreparationBlocked,
			bool updatePreparationCounts = true);

		/** Projects, clips and stores one ready sprite directly in its Section occurrence.
		 * @param section Section whose preparing geometry owns the occurrence.
		 * @param job Source generation and exact per-ordinal key state.
		 * @param ordinal Publication-local sprite ordinal.
		 * @param requirement Placement and pass facts from that exact source ordinal.
		 * @param payload Exact immutable payload returned by the successful readiness lookup.
		 * @param occurrenceOutcome Receives the resulting sprite preparation status.
		 * @param updatePreparationCounts Whether to maintain aggregate counters.
		 */
		void AppendReadySpriteInput(
			::Services::Map::Buildings::BuildingMapSection& section,
			SectionJob& job,
			std::size_t ordinal,
			const BuildingCandidateSourceRecord& requirement,
			std::shared_ptr<const Cache::ProcessedSpritePayload> payload,
			KnownSpritePreparationResult::OccurrenceOutcome& occurrenceOutcome,
			bool updatePreparationCounts,
			bool retainGeometry);

		/** Owns spline backing values and borrowed descriptors while sprite data and geometry remain Section-owned. */
		struct RetainedSectionScene
		{
			/** Owns segment and surface arrays referenced by descriptor spans in Splines. */
			std::vector<SplineRenderInputPreparationResult> SplinePreparations;
			std::vector<Visuals::BuildingSplineRenderInput> Splines;
			Visuals::BuildingRenderSceneInput RenderSceneInput;
		};
		/** Per-occurrence sprite retry state indexed by the immutable publication ordinal.
		 * Spline preparation values are stored separately in compact source order.
		 */
		struct SectionOccurrenceProgress
		{
			std::optional<Cache::OwnedSpriteKey> SpriteKey;
			std::optional<std::size_t> NextSpriteRetryOrdinal;
		};
		/** Retains ordinal outcomes, compact spline values and outstanding sprite-resource work.
		 * The readiness cursor advances only forward; retry links cover pending sprite resources.
		 */
		struct SectionPreparationProgress
		{
			KnownSpritePreparationResult SpritePreparation;
			std::vector<SectionOccurrenceProgress> Occurrences;
			std::vector<SplineRenderInputPreparationResult> SplinePreparations;
			std::vector<std::size_t> OccurrenceOrder;
			/** Valid-placement prefix length returned by the existing one-time sprite sort. */
			std::size_t ValidSpriteCount = 0;
			std::size_t InitialSpriteCount = 0;
			std::size_t NextOccurrenceIndex = 0;
			std::size_t FirstUnresolvedOrdinal = 0;
			std::optional<std::uint64_t> RetryTickSequence;
			std::optional<std::size_t> FirstRetryQueuedThisTick;
			std::optional<std::size_t> SpriteRetryHead;
			std::optional<std::size_t> SpriteRetryTail;
			std::optional<SectionScenePreparationResult::Readiness> TerminalReadiness;
			bool Initialized = false;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			std::size_t SourceOccurrenceCount = 0;
			std::size_t SourceSplineOccurrenceCount = 0;
#endif
		};

		/** Holds every resumable rendering field for one Section-owned unfinished generation. */
		struct SectionJob
		{
			std::shared_ptr<const BuildingCandidatePublication> publicationOwner;
			std::optional<SectionImageCommitResult> AttemptEvidence;
			SectionPreparationProgress Preparation;
			std::unique_ptr<RetainedSectionScene> retainedScene;
			Visuals::BuildingGeometryPreparation::PreparationCursor GeometryCursor;
			std::vector<std::uint8_t> CompletedImage;
			Visuals::BuildingVisualsTerminalOmissionSummary TerminalSplineOmissions{};
			bool CompletedImageReady = false;
			bool CompletedImageTransparent = false;
			bool GeometryComplete = false;
			bool GeometryFailed = false;
			std::unique_ptr<Rendering::SectionRendering::IncrementalOperation> NumericalOperation;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			SplineDiagnosticMetrics SplineDiagnostics;
			SplineDiagnosticReportKey LastSplineDiagnosticReport;
			bool HasSplineDiagnosticReport = false;
#endif
		};

		const double sectionSizeMapUnits_;
		const std::uint32_t sectionResolutionPixels_;
		const std::filesystem::path diagnosticsDirectory_;
		Cache::SpriteSourceManager* spriteSourceManager_ = nullptr;
		std::unique_ptr<BuildingScheduler> buildingScheduler_;
	};
}
