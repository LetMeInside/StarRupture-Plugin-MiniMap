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
		/** Identifies occurrence preparation, retained-scene numerical work, or scene-free continuation. */
	enum class SectionJobContinuationMode : std::uint8_t
	{
		FullPreparation,
		PreparedSceneInput,
		NoSceneInput
	};

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
		/** Retains Section-local known-sprite preparation evidence and its ready payload owners.
		 * KnownRequirementsProcessed describes a validated occurrence set; readiness remains
		 * determined from each occurrence's source and resource outcome.
		 * Aggregate counters serve the synchronous API; resumable Section preparation uses outcomes.
		 * Each prepared view is paired with a shared payload owner. Synchronous callers retain this
		 * result through consumption; Section jobs keep its owner when a view outlives preparation.
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
			/** Describes one half-open world-space XY envelope derived once from a sprite quad.
			 * X increases rightward and Y downward. This bound does not certify Section membership.
			 */
			struct PreparedSpriteSupport
			{
				Visuals::SpriteWorldQuad WorldQuad{};
				double OccluderTopHeight = 0.0;
				bool IsOccluder = false;
				/** Inclusive left edge, the minimum world-space X of the quad corners. */
				double MinimumX = 0.0;
				/** Inclusive top edge, the minimum world-space Y of the quad corners. */
				double MinimumY = 0.0;
				/** Exclusive right edge, the maximum world-space X of the quad corners. */
				double MaximumX = 0.0;
				/** Exclusive bottom edge, the maximum world-space Y of the quad corners. */
				double MaximumY = 0.0;
			};
			/** One ready sprite occurrence without an assumed ordinary/overlay rendering pass. */
			struct PreparedSpriteOccurrence
			{
				/** Ordinal of the corresponding occurrence in the retained candidate publication. */
				std::uint64_t PublicationOrdinal = 0;
				/** Neutral pass designation; Unknown remains unresolved for later scene eligibility. */
				BuildingCandidateSpritePass SpritePass = BuildingCandidateSpritePass::Unknown;
				/** Render input borrowing pixels from the paired shared payload owner. */
				Visuals::BuildingSpriteRenderInput RenderInput{};
				/** Keeps the immutable payload alive for transfer into Section rendering state. */
				std::shared_ptr<const Cache::ProcessedSpritePayload> Payload;
				/** Independent projected XY support paired with RenderInput. */
				PreparedSpriteSupport Support{};
			};
			/** Ready known sprite occurrences, preserving duplicates and publication correspondence. */
			std::vector<PreparedSpriteOccurrence> PreparedSprites;
			/** Section-local ordinal to sparse PreparedSprites index correspondence.
			 * Empty entries have no prepared sprite. Owns indices only, never vector pointers or pixels.
			 * Reset clears this alongside PreparedSprites.
			 */
			std::vector<std::optional<std::size_t>> PreparedSpriteIndicesByOrdinal;
		};

		/** Owns ordered numerical profile values prepared from one retained source record.
		 * SourceStatus preserves acquisition evidence. PreparedNumericalInput means only that one
		 * profile maps to the generic surface input; it does not establish drawable geometry,
		 * appearance policy, render readiness, or Section membership.
		 */
		struct SplineProfilePreparationResult
		{
			/** Retains the producer's independent profile acquisition result. */
			SplineProfileAcquisitionStatus SourceStatus = SplineProfileAcquisitionStatus::Unknown;
			/** One result per source alternative, with source order and cardinality preserved. */
			struct Alternative
			{
				/** Numerical compatibility of this individual acquired alternative. */
				enum class Status
				{
					PreparedNumericalInput,
					MissingMesh,
					InvalidBounds,
					InvalidValues,
					Unsupported
				};

				/** Per-alternative result, separate from the overall acquisition status. */
				Status PreparationStatus = Status::Unsupported;
				/** Geometry values only; callers apply appearance policy in a later stage. */
				Visuals::BuildingSplineSurfaceRenderInput SurfaceInput{};
			};
			/** Caller-owned values; contains no view into the publication or profile set. */
			std::vector<Alternative> Alternatives;
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
			/** Independent authored-profile acquisition state retained for the future assembler. */
			SplineProfileAcquisitionStatus ProfileStatus = SplineProfileAcquisitionStatus::Unknown;
			/** Original source selection evidence for the curve values. */
			BuildingCandidateSplineProvenance Provenance = BuildingCandidateSplineProvenance::Unknown;
			/** Original coordinate basis; only WorldSpace is currently eligible for scene inputs. */
			BuildingCandidateSplineCoordinateSpace CoordinateSpace =
				BuildingCandidateSplineCoordinateSpace::Unknown;
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

		/** Owns one occurrence's composed spline inputs and all source/profile eligibility evidence.
		 * PublicationOrdinal distinguishes duplicate occurrences. Authored surfaces retain their
		 * profile-alternative correspondence; a synthetic ribbon owns one independent surface.
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
				MalformedPreparedSurface,
				NoPreparedSurface
			};

			/** Exact publication-local identity for this one occurrence, including duplicates. */
			std::uint64_t PublicationOrdinal = 0;
			/** Source-supplied direct policy index, preserved even when resolution fails. */
			std::uint16_t SplinePolicyIndex = 0;
			/** Explicit source geometry intent, never inferred from profile acquisition status. */
			BuildingCandidateSplineGeometryMode GeometryMode =
				BuildingCandidateSplineGeometryMode::AuthoredProfiles;
			/** Source-supplied synthetic ribbon half-width in map units. */
			double SyntheticThinRibbonHalfWidthMapUnits = 0.0;
			/** R2Q curve, root, acquisition, provenance and coordinate evidence owned by this result. */
			SplineCurvePreparationResult CurvePreparation;
			/** Per-alternative outcomes in original source order, including each accepted surface index. */
			struct AlternativeOutcome
			{
				/** Zero-based index in the original immutable profile alternative list. */
				std::size_t SourceAlternativeIndex = 0;
				/** Original R2P result for this profile value. */
				SplineProfilePreparationResult::Alternative::Status PreparationStatus =
					SplineProfilePreparationResult::Alternative::Status::Unsupported;
				/** Index into SurfaceInputs, or empty when the alternative was not included. */
				std::optional<std::size_t> SurfaceIndex;
			};
			std::vector<AlternativeOutcome> Alternatives;
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

		/** Owns neutral scene values and per-occurrence evidence without owning their publication.
		 * Sprite pixel spans borrow from immutable payload owners held by Section-owned occurrence
		 * progress or retained requirements in the resumable path, and by KnownSpritePreparationResult
		 * during synchronous preparation. The corresponding owner must remain alive and unchanged.
		 * No final Section membership or image completion is implied.
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
			/** Ready FirstPass sprites, stably ordered by placement Z, Y, then X, borrowing CPU pixel payloads. */
			std::vector<Visuals::BuildingSpriteRenderInput> OrdinarySprites;
			/** Owned spline composition results in Section occurrence order; initial borrowing sorts descriptors once. */
			std::vector<SplineRenderInputPreparationResult> SplinePreparations;
			/** Ready SecondPass sprites, stably ordered by placement Z, Y, then X, borrowing CPU pixel payloads. */
			std::vector<Visuals::BuildingSpriteRenderInput> OverlayLastSprites;
			/** Raw identity confirms sprite preparation came from this retained immutable publication. */
			const BuildingCandidatePublication* PublicationIdentity = nullptr;
			Readiness PreparationReadiness = Readiness::InvalidPublication;

			/** Clears owned values and invalidates any borrowed views into those values. */
			void Reset() noexcept;

			/** Builds ordered borrowed views once for a complete scene and rejects incomplete inputs.
			 * @param section Section receiving failure diagnostics for this retained scene.
			 * @param publicationOwner Retained owner whose Section occurrences were prepared.
			 * @param scratch Receives sorted spline descriptors whose spans refer into this result.
			 * @param output Receives spans in ordinary, sorted-spline, overlay-last painter order.
			 * @return True when this publication's prepared scene is CompleteSceneInputs.
			 */
			bool BorrowRenderSceneInput(
				const ::Services::Map::Buildings::BuildingMapSection& section,
				const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
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
		struct SectionJob;
		struct RetainedSectionScene;
		struct RetainedSpriteRequirement;
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
			std::optional<SectionScenePreparationResult::Readiness> SceneReadiness;
			std::optional<SectionImageCommitResult> FinishedAttempt;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
#endif
		};

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
		 * Equal ContentHash retains all prepared views on their original shared owner.
		 * @param section Service-owned Section whose unfinished state is accessed directly.
		 * @param publicationOwner Non-null shared owner of the selected immutable publication.
		 * @param generationReplaced Receives whether changed content reset this Section's generation.
		 */
		std::shared_ptr<const BuildingCandidatePublication> EnsureSectionJob(
			::Services::Map::Buildings::BuildingMapSection& section,
			const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
			bool& generationReplaced);
		/** Selects initial preparation, retained-scene reuse or a scene-free continuation.
		 * @param section Section carrying the rendering state to inspect.
		 * @param publicationOwner Currently selected shared publication owner.
		 * @return The conservative continuation mode supported by exact retained state.
		 */
		SectionJobContinuationMode SelectSectionJobContinuationMode(
			const ::Services::Map::Buildings::BuildingMapSection& section,
			const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner) const noexcept;
		/** Checks retained publication identity and complete scene evidence before mode selection.
		 * @param job Retained job selected by the exact Section coordinate.
		 * @param publicationOwner Currently selected shared publication owner.
		 * @return True when exact ownership and retained-scene readiness agree.
		 */
		bool HasValidRetainedSceneForContinuation(
			const SectionJob& job,
			const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner) const noexcept;
		/** Advances the supplied Section's publication state using its selected continuation contract.
		 * @param section Exact Section whose stable numerical context is borrowed for this call.
		 * @param publicationOwner Currently selected immutable publication for this coordinate.
		 * @param continuationMode Scheduler-owned initial-preparation, retained-scene or scene-free mode.
		 * @param splinePolicies Existing immutable spline policy table.
		 * @param deadline Absolute end of the shared scheduling allowance.
		 * @return Pending, waiting, restart-required, or a fully populated terminal attempt result.
		 */
		SectionJobAdvanceResult AdvanceSectionJob(
			::Services::Map::Buildings::BuildingMapSection& section,
			const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
			SectionJobContinuationMode continuationMode,
			std::span<const SplinePolicy> splinePolicies,
			std::chrono::steady_clock::time_point deadline,
			bool renderSectionImages);

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
		 * @param deadline Absolute end of the shared scheduling allowance.
		 * @param deadlineDeferred Receives whether unfinished preparation was deferred by elapsed time.
		 * @return Existing scene readiness; only CompleteSceneInputs installs retained state.
		 */
		SectionScenePreparationResult::Readiness PrepareRetainedSectionScene(
			::Services::Map::Buildings::BuildingMapSection& section,
			SectionJob& job,
			std::span<const SplinePolicy> splinePolicies,
			std::chrono::steady_clock::time_point deadline,
			bool& deadlineDeferred);
		/** Initializes exact-publication occurrence state once and prepares only untouched ordinals.
		 * @param job Section-owned generation state retaining source facts and completed actions.
		 * @param publicationOwner Exact immutable publication selected for this Section.
		 * @param splinePolicies Borrowed immutable policies used by each spline occurrence once.
		 * @return True when all ordinal/source correspondence is valid for this generation.
		 */
		bool InitializeSectionPreparationProgress(
			const ::Services::Map::Buildings::BuildingMapSection& section,
			SectionJob& job,
			const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner);
		/** Advances untouched ordinals and retries only pending sprite-resource operations.
		 * @param job Section-owned progress with direct ordinal-indexed state.
		 * @param publicationOwner Exact publication supplying immutable source values.
		 * @param splinePolicies Existing service-owned immutable policy table.
		 * @param deadline Absolute end of this shared scheduling allowance.
		 * @return True when unfinished preparation was skipped because the deadline elapsed.
		 */
		bool AdvanceSectionOccurrencePreparation(
			::Services::Map::Buildings::BuildingMapSection& section,
			SectionJob& job,
			const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
			std::span<const SplinePolicy> splinePolicies,
			std::chrono::steady_clock::time_point deadline);
		/** Stores one ready occurrence's owned quad in first-appearance Section geometry.
		 * @param section Owner whose PreparingScene receives this contribution.
		 * @param job Source-owned preparation with direct ordinal-to-payload correspondence.
		 * @param ordinal Exact publication-local occurrence to retain.
		 * @return True when the source key and projected numerical geometry are valid.
		 */
		bool RetainProgressiveSpriteContribution(
			::Services::Map::Buildings::BuildingMapSection& section,
			SectionJob& job,
			std::size_t ordinal);
		/** Builds one Section-clipped prepared occurrence without changing its source correspondence.
		 * @param preparedSprite Ready source occurrence with its already projected world quad.
		 * @param sourceKey Canonical key retained by the current publication ordinal.
		 * @param sectionRegion Exact rectangle owned by the Section.
		 * @param output Receives the original quad and clipped drawable triangles.
		 * @return True when the Section clipping operation succeeds, including valid exclusion.
		 */
		static bool TryPrepareClippedSpriteOccurrence(
			const KnownSpritePreparationResult::PreparedSpriteOccurrence& preparedSprite,
			const Cache::OwnedSpriteKey& sourceKey,
			const BuildingCollectionRegion& sectionRegion,
			Visuals::PreparedBuildingGeometry::SpriteOccurrence& output);
		/** Inserts one sprite occurrence at its stable Z/Y/X drawing position.
		 * @param destination Ordinary or overlay pass owned by the preparing Section scene.
		 * @param occurrence Complete owned sprite geometry and source identity.
		 */
		static void InsertPreparedSpriteOccurrence(
			std::vector<Visuals::PreparedBuildingGeometry::SpriteOccurrence>& destination,
			Visuals::PreparedBuildingGeometry::SpriteOccurrence&& occurrence);
		/** Prepares one previously untouched occurrence and records its next outstanding action.
		 * @param job Section-owned occurrence state and sprite outcomes.
		 * @param publication Exact immutable publication used for source-record correspondence.
		 * @param candidate Immutable candidate occurrence aligned with ordinal.
		 * @param source Immutable source facts aligned with ordinal.
		 * @param ordinal Section-local publication ordinal to prepare.
		 * @param splinePolicies Immutable service-owned policy table.
		 */
		void PrepareSectionOccurrence(
			SectionJob& job,
			const BuildingCandidatePublication& publication,
			const BuildingCandidateOccurrence& candidate,
			const BuildingCandidateSourceRecord& source,
			std::size_t ordinal,
			std::span<const SplinePolicy> splinePolicies);
		/** Validates one sprite key and performs its first resource acquisition attempt.
		 * @param job Section-owned progress retaining the occurrence's key/result.
		 * @param publication Exact immutable publication used for ordinal correspondence.
		 * @param source Immutable source facts for the exact occurrence.
		 * @param ordinal Exact publication-local sprite ordinal.
		 * @param sourcePreparationBlocked Whether immutable source facts already block the scene.
		 */
		void PrepareSectionSpriteOccurrence(
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
		/** Collects ready sprite indexes from their direct ordinal outcome slots once.
		 * @param job Complete Section-owned occurrence progress.
		 * @param output Receives indexes into the retained prepared-sprite values.
		 * @return True when every ready sprite has exact sparse-index correspondence.
		 */
		bool CollectReadySpriteOrder(
			const ::Services::Map::Buildings::BuildingMapSection& section,
			const SectionJob& job,
			std::vector<std::size_t>& output) const;
		/** Applies the established stable Z/Y/X painter ordering to selected prepared values.
		 * @param preparation Complete prepared-sprite values whose indexes are retained.
		 * @param order Index array to sort in place.
		 */
		void SortPreparedSpriteOrder(
			const KnownSpritePreparationResult& preparation,
			std::vector<std::size_t>& order) const;
		/** Moves sorted render inputs, validated keys and payload owners into matching passes.
		 * @param job Section-owned progress containing per-occurrence keys and payloads.
		 * @param preparedOrder Stable painter-ordered indexes into prepared sprite values.
		 * @param retained Receives parallel shared payload owners for retained scene inputs.
		 * @param preparation Receives ordinary and overlay render inputs in painter order.
		 * @return True when every input has exact pass, key, payload and pixel-span ownership.
		 */
		bool MoveOrderedSpriteInputs(
			const ::Services::Map::Buildings::BuildingMapSection& section,
			SectionJob& job,
			const std::vector<std::size_t>& preparedOrder,
			RetainedSectionScene& retained,
			SectionScenePreparationResult& preparation);
		/** Stores one status transition and advances past resolved ordinals once.
		 * @param job Section-owned progress with a monotonic readiness cursor.
		 * @param ordinal Exact publication-local occurrence whose result changed.
		 * @param status New preparation status for the occurrence.
		 */
		void SetSectionOccurrenceStatus(
			SectionJob& job,
			std::size_t ordinal,
			KnownSpritePreparationResult::OccurrenceOutcome::Status status);
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
		 * @param continuationMode Scheduler-selected occurrence, retained-scene or scene-free phase.
		 * @param splinePolicies Borrowed immutable service policy values.
		 * @param deadline Absolute end of the shared scheduling allowance.
		 * @param result Receives readiness, state and consumed units.
		 * @return True when numerical continuation may proceed.
		 */
		bool PrepareSectionJobAttempt(
			::Services::Map::Buildings::BuildingMapSection& section,
			SectionJob& job,
			SectionJobContinuationMode continuationMode,
			std::span<const SplinePolicy> splinePolicies,
			std::chrono::steady_clock::time_point deadline,
			bool renderSectionImages,
			SectionJobAdvanceResult& result);
		/** Advances numerical geometry directly into the Section's PreparingScene. */
		bool AdvanceSectionJobGeometry(
			::Services::Map::Buildings::BuildingMapSection& section,
			SectionJob& job,
			std::chrono::steady_clock::time_point deadline,
			SectionJobAdvanceResult& result);
		/** Resumes occurrence preparation or preserves an established retained scene.
		 * @param section Exact service-owned Section described by the publication.
		 * @param job Selected retained job whose scene state may be rebuilt or reused.
		 * @param continuationMode Selected initial-preparation, retained-scene or scene-free mode.
		 * @param splinePolicies Borrowed immutable service policy values.
		 * @param deadline Absolute end of the shared scheduling allowance.
		 * @param result Receives preparation readiness and any restart/wait state.
		 * @return True when a valid retained scene is available for this advancement.
		 */
		bool PrepareSectionJobSceneForAttempt(
			::Services::Map::Buildings::BuildingMapSection& section,
			SectionJob& job,
			SectionJobContinuationMode continuationMode,
			std::span<const SplinePolicy> splinePolicies,
			std::chrono::steady_clock::time_point deadline,
			SectionJobAdvanceResult& result);
		/** Runs one source-composition attempt and records its readiness and Debug duration. */
		bool PrepareRetainedSceneForAttempt(
			::Services::Map::Buildings::BuildingMapSection& section,
			SectionJob& job,
			std::span<const SplinePolicy> splinePolicies,
			std::chrono::steady_clock::time_point deadline,
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
		/** Builds the stable, sorted spline array and all three retained scene spans after ownership settles.
		 * @param retained Section-owned preparation arrays and spline storage receiving its final descriptors.
		 * @param publicationOwner Exact owner used to validate the completed scene before descriptor creation.
		 * @return True when the retained scene inputs are internally complete and stable.
		 */
		static bool InitializeRetainedSceneInputs(
			::Services::Map::Buildings::BuildingMapSection& section,
			RetainedSectionScene& retained,
			const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner);
		/** Assigns exact painter-pass indexes after validating progressive sprite correspondence.
		 * @param occurrences Section-owned progressive sprite geometry in painter order.
		 * @param descriptors Final retained resource descriptors for the same pass.
		 * @return True when each occurrence uniquely matches the descriptor at its exact array index.
		 */
		static bool AssignSpriteSceneIndexes(
			const ::Services::Map::Buildings::BuildingMapSection& section,
			std::uint64_t sourceContentHash,
			bool overlay,
			std::vector<Visuals::PreparedBuildingGeometry::SpriteOccurrence>& occurrences,
			std::span<const Visuals::BuildingSpriteRenderInput> descriptors);
		/** Confirms an already completed scene retains indexes for these exact resource descriptors.
		 * @param occurrences Immutable completed sprite geometry in painter order.
		 * @param descriptors Retained resource descriptors for the corresponding pass.
		 * @return True only when every stored index, ordinal and canonical key matches its descriptor.
		 */
		static bool ValidateSpriteSceneIndexes(
			const ::Services::Map::Buildings::BuildingMapSection& section,
			std::uint64_t sourceContentHash,
			bool overlay,
			const std::vector<Visuals::PreparedBuildingGeometry::SpriteOccurrence>& occurrences,
			std::span<const Visuals::BuildingSpriteRenderInput> descriptors);

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		/** Reports failed pass correspondence using the expected slot and stored occurrence identity. */
		static void ReportSpriteSceneIndexMismatch(
			const ::Services::Map::Buildings::BuildingMapSection& section,
			std::uint64_t sourceContentHash,
			bool overlay,
			bool completed,
			const Visuals::PreparedBuildingGeometry::SpriteOccurrence& occurrence,
			const Visuals::BuildingSpriteRenderInput& descriptor,
			std::size_t descriptorIndex,
			std::size_t descriptorCount) noexcept;
#endif

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
		/** Validates publication identity and sprite-preparation correspondence.
		 * @param publicationOwner Explicitly retained source publication.
		 * @param spritePreparation Publication-level sprite outcomes to reuse.
		 * @param readiness Receives the invalid category on rejection.
		 * @return True when publication identity and occurrence correspondence are valid.
		 */
		bool ValidateSectionPreparationInputs(
			const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
			const KnownSpritePreparationResult& spritePreparation,
			SectionScenePreparationResult::Readiness& readiness) const;
		/** Rejects missing, reordered, or mismatched publication-level sprite outcomes.
		 * @param publication Retained publication used to verify ordinal and pass correspondence.
		 * @param spritePreparation Caller-owned result from the same retained publication.
		 * @return True when each occurrence outcome corresponds to this publication.
		 */
		static bool IsCompatibleSpritePreparation(
			const BuildingCandidatePublication& publication,
			const KnownSpritePreparationResult& spritePreparation);
		/** Accounts for one exact occurrence and appends only ready pass-eligible numerical inputs.
		 * @param publicationOwner Retained source owner used by existing spline composition.
		 * @param ordinal Publication-local occurrence ordinal.
		 * @param spritePreparation Reusable outcomes from the same publication.
		 * @param splinePolicies Borrowed immutable service policy table.
		 * @param output Receives this occurrence's evidence and any owned numerical input.
		 */
		void ProcessSectionOccurrence(
			const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
			std::uint64_t ordinal,
			const KnownSpritePreparationResult& spritePreparation,
			std::span<const SplinePolicy> splinePolicies,
			SectionScenePreparationResult& output) const;
		static void InitializeSectionOccurrenceOutcome(
			std::uint64_t ordinal,
			const BuildingCandidateOccurrence& candidate,
			const BuildingCandidateSourceRecord& source,
			const KnownSpritePreparationResult& spritePreparation,
			KnownSpritePreparationResult::OccurrenceOutcome& outcome);
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
		/** Appends an already prepared ready sprite to its neutral ordered pass.
		 * @param ordinal Publication-local occurrence ordinal to resolve.
		 * @param spritePreparation Existing publication-level sprite result; no acquisition is repeated.
		 * @param outcome Receives the resolved per-occurrence state.
		 * @param output Receives an ordinary or overlay input only when its pass is known.
		 */
		static void ProcessPreparedSprite(
			std::uint64_t ordinal,
			const KnownSpritePreparationResult& spritePreparation,
			KnownSpritePreparationResult::OccurrenceOutcome& outcome,
			SectionScenePreparationResult& output);
		/** Reuses R2R composition for one spline occurrence and its direct service policy value.
		 * @param publicationOwner Retained source owner required by the composition operation.
		 * @param ordinal Publication-local spline occurrence ordinal.
		 * @param source Borrowed source record carrying its policy index.
		 * @param splinePolicies Borrowed immutable service-owned policy table.
		 * @param outcome Receives readiness classification while retaining acquisition evidence.
		 * @param output Owns the spline preparation result and occurrence report.
		 */
		void ProcessPreparedSpline(
			const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
			std::uint64_t ordinal,
			const BuildingCandidateSourceRecord& source,
			std::span<const SplinePolicy> splinePolicies,
			KnownSpritePreparationResult::OccurrenceOutcome& outcome,
			SectionScenePreparationResult& output) const;
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
		/** Reduces Section-local outcomes without upgrading unknown or pending source facts.
		 * @param output Completed caller-owned result to classify.
		 * @return Complete-only input readiness or its first blocking category.
		 */
		static SectionScenePreparationResult::Readiness DetermineSceneReadiness(
			const SectionScenePreparationResult& output);

		/** Copies compatible authored profiles into caller-owned generic numerical inputs.
		 * The caller retains the publication owner for the borrowed record; output owns every value.
		 * Source alternatives remain in exact order and are never selected here.
		 * @param source Borrowed immutable neutral candidate source record.
		 * @param output Receives source acquisition state and ordered per-alternative results.
		 */
		void PrepareSplineProfileAlternatives(
			const BuildingCandidateSourceRecord& source,
			SplineProfilePreparationResult& output) const;

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

		/** Checks independent source, curve, profile, transform, policy and scale prerequisites.
		 * @param source Borrowed occurrence source record.
		 * @param resolvedPolicyIndex Index used by the service policy lookup.
		 * @param resolvedPolicy Borrowed service-owned policy value.
		 * @param output Receives the validated existing sampling defaults and pixel density.
		 * @return The first composition failure, or NotAttempted when surface preparation may proceed.
		 */
		bool CheckSplineRenderInputPrerequisites(
			const BuildingCandidateSourceRecord& source,
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

		/** Derives PixelsPerUnit and preserves every other existing sampling-policy default.
		 * @param output Receives the configured policy only after finite positive density validation.
		 * @return True when the immutable Section configuration yields finite positive pixel density.
		 */
		bool TryPrepareSplineSamplingPolicy(SplineRenderInputPreparationResult& output) const;

		/** Moves compatible profile values into ordered surface inputs and records source indexes.
		 * @param policy Validated service-owned solid appearance policy copied into each surface.
		 * @param output Owns surfaces and alternative-to-surface correspondence.
		 * @return True when at least one compatible surface was available.
		 */
		static bool AppendSplineRenderSurfaces(
			const SplinePolicy& policy,
			SplineProfilePreparationResult& profiles,
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

		/** Maps one authored alternative's evidence and fixed parameters to generic surface values.
		 * @param source One neutral profile alternative borrowed for this call.
		 * @param output Receives an owned numerical surface input when representable.
		 */
		static void PrepareSplineProfileAlternative(
			const SplineProfileAlternative& source,
			SplineProfilePreparationResult::Alternative& output);

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
			std::uint64_t publicationOrdinal,
			const BuildingCandidateSourceRecord& requirement,
			const Cache::OwnedSpriteKey& ownedKey,
			KnownSpritePreparationResult& output,
			KnownSpritePreparationResult::OccurrenceOutcome& occurrenceOutcome,
			bool updatePreparationCounts = true,
			bool prepareGeometrySupport = true);

		/** Acquires a ready payload, validates placement geometry and appends one paired occurrence.
		 * @param publicationOrdinal Corresponding ordinal in the retained candidate publication.
		 * @param requirement Borrowed known descriptor with its optional validated placement.
		 * @param ownedKey Validated key whose payload is ready through BuildingScheduler.
		 * @param output Accumulates payload ownership and geometry outcomes.
		 * @param occurrenceOutcome Receives this ordinal's independently preserved preparation state.
		 * @param updatePreparationCounts Whether to maintain synchronous aggregate counters.
		 */
		void PrepareReadySpriteRequirement(
			std::uint64_t publicationOrdinal,
			const BuildingCandidateSourceRecord& requirement,
			const Cache::OwnedSpriteKey& ownedKey,
			KnownSpritePreparationResult& output,
			KnownSpritePreparationResult::OccurrenceOutcome& occurrenceOutcome,
			bool updatePreparationCounts = true,
			bool prepareGeometrySupport = true);
		/** Builds one ready sprite input from an already acquired CPU payload.
		 * @param publicationOrdinal Exact source occurrence ordinal.
		 * @param requirement Borrowed source values and placement for that occurrence.
		 * @param payload Ready payload retained by the paired prepared occurrence.
		 * @param output Receives the input view and any geometry outcome.
		 * @param occurrenceOutcome Receives the matching per-ordinal state.
		 * @param updatePreparationCounts Whether to maintain synchronous aggregate counters.
		 */
		void AppendReadySpriteInput(
			std::uint64_t publicationOrdinal,
			const BuildingCandidateSourceRecord& requirement,
			std::shared_ptr<const Cache::ProcessedSpritePayload> payload,
			KnownSpritePreparationResult& output,
			KnownSpritePreparationResult::OccurrenceOutcome& occurrenceOutcome,
			bool updatePreparationCounts = true,
			bool prepareGeometrySupport = true);

		/** Derives the independent world-space XY envelope from the prepared projected quad.
		 * @param renderInput Prepared placement, capture metadata and projected image edges.
		 * @param output Receives the minimum and maximum world-space XY corner coordinates.
		 * @return True when SpriteGeometry builds all finite projected world-space corners.
		 */
		bool TryBuildPreparedSpriteSupport(
			const Visuals::BuildingSpriteRenderInput& renderInput,
			KnownSpritePreparationResult::PreparedSpriteSupport& output) const noexcept;

		/** Owns one drawable occurrence's validated key, pass identity and pixel payload owner. */
		struct RetainedSpriteRequirement
		{
			std::uint64_t PublicationOrdinal = 0;
			BuildingCandidateSpritePass SpritePass = BuildingCandidateSpritePass::Unknown;
			Cache::OwnedSpriteKey SpriteKey;
			std::shared_ptr<const Cache::ProcessedSpritePayload> Payload;
		};
		/** Owns prepared scene arrays, shared sprite payloads and stable render spans for one Section job. */
		struct RetainedSectionScene
		{
			SectionScenePreparationResult Preparation;
			std::vector<RetainedSpriteRequirement> OrdinarySprites;
			std::vector<RetainedSpriteRequirement> OverlayLastSprites;
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
			std::vector<std::uint8_t> IsOrdinarySpriteByOrdinal;
			std::vector<std::uint8_t> IsSplineByOrdinal;
			std::size_t NextOccurrenceOrdinal = 0;
			std::size_t FirstUnresolvedOrdinal = 0;
			std::size_t UnresolvedOrdinarySpriteCount = 0;
			std::size_t UnresolvedSplineCount = 0;
			std::optional<std::size_t> SpriteRetryHead;
			std::optional<std::size_t> SpriteRetryTail;
			std::size_t SpriteRetryCount = 0;
			std::optional<SectionScenePreparationResult::Readiness> TerminalReadiness;
			bool Initialized = false;
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
			std::optional<Rendering::SectionRendering::Status> NumericalStatus;
			bool CompletedImageReady = false;
			bool CompletedImageTransparent = false;
			bool GeometryComplete = false;
			bool GeometryFailed = false;
			std::unique_ptr<Rendering::SectionRendering::IncrementalOperation> NumericalOperation;
		};

		const double sectionSizeMapUnits_;
		const std::uint32_t sectionResolutionPixels_;
		const std::filesystem::path diagnosticsDirectory_;
		Cache::SpriteSourceManager* spriteSourceManager_ = nullptr;
		std::unique_ptr<BuildingScheduler> buildingScheduler_;
	};
}
