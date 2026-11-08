// Owns Building Map acquisition and synchronously renders and commits complete Section images.
#pragma once

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
#include "BuildingScheduler.h"
#endif

#include "../BuildingCandidatePublication.h"
#include "../Rendering/SectionRendering.h"
#include "../Visuals/BuildingRenderSceneInput.h"
#include "../Cache/OwnedSpriteKey.h"
#include "../SplinePolicy.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Services::Map::Buildings
{
	class BuildingMapSection;
}

namespace Services::Map::Buildings::Cache
{
	class CpuSpriteSourceManager;
	class ProcessedSpritePayload;
	class SpriteDiskCache;
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

	/** Owns persistent acquisition resources and synchronously prepares, renders and commits Sections.
	 * SpriteDiskCache outlives the CPU manager, which outlives BuildingScheduler and its renderer.
	 * The renderer object survives gameplay stops under this plugin-lifetime owner. Construction creates
	 * CPU-side owners only; the renderer lifecycle callback advances cancellation independently.
	 * Completed image ownership remains exclusively with the supplied BuildingMapSection; the commit
	 * outcome reports whether adoption succeeded.
	 */
	class SectionScheduler final
	{
	public:
		/** Reports how one fixed publication's known sprite requirements were examined.
		 * KnownRequirementsProcessed means only that its published candidates were examined;
		 * it does not certify complete source enumeration, geometry support, or Section readiness.
		 * Prepared sprite inputs borrow CPU pixels owned by CpuSpriteSourceManager and are valid
		 * only for immediate synchronous consumption before those payloads can be replaced, cleared,
		 * or withdrawn. They must never be retained as Section state.
		 */
		struct KnownSpritePreparationResult
		{
			/** Distinguishes a processed descriptor batch from publication/key validity failures. */
			enum class Status
			{
				KnownRequirementsProcessed,
				InvalidPublication,
				StalePublication,
				InvalidSourceRequirement
			};

			/** Outcome of this single preparation operation. */
			Status PreparationStatus = Status::InvalidPublication;
			/** Non-owning publication identity valid only while the caller retains that owner. */
			const BuildingCandidatePublication* PublicationIdentity = nullptr;
			/** Candidate publication evidence; sprite readiness cannot upgrade these facts. */
			BuildingCandidateEvidence CandidateEvidence{};
			/** True when evidence or unprepared source kinds leave possible requirements unresolved. */
			bool MayHaveUnknownRequirements = true;
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
			/** Describes one short-lived half-open world-space XY envelope derived from a sprite quad.
			 * X increases rightward and Y downward. This bound does not certify Section membership.
			 */
			struct PreparedSpriteSupport
			{
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
				/** Render input borrowing pixels from CpuSpriteSourceManager. */
				Visuals::BuildingSpriteRenderInput RenderInput{};
				/** Independent projected XY support paired with RenderInput. */
				PreparedSpriteSupport Support{};
			};
			/** Ready known sprite occurrences, preserving duplicates and publication correspondence. */
			std::vector<PreparedSpriteOccurrence> PreparedSprites;
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
		 * PublicationOrdinal distinguishes duplicate occurrences. Surface indexes map accepted inputs
		 * back to profile alternatives. BorrowRenderInput creates spans only on request; this object
		 * must remain alive and unchanged for their full synchronous use. Composed means inputs are
		 * available for a numerical rendering attempt; it does not certify candidate completeness.
		 */
		struct SplineRenderInputPreparationResult
		{
			/** Describes why a complete neutral render input was or was not composed. */
			enum class Status
			{
				NotAttempted,
				Composed,
				StalePublication,
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
			/** Eligible surfaces in source-relative order with copied solid policy appearance. */
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
			 * @param output Receives spans borrowing this result's segment and surface vectors.
			 * @return True when a complete neutral input was composed and its owned arrays are available.
			 */
			bool BorrowRenderInput(Visuals::BuildingSplineRenderInput& output) const noexcept;
		};

		/** Owns one Section's numerical scene inputs and readiness evidence.
		 * Sprite pixels remain borrowed from the CPU manager, and publication/observation storage is
		 * never retained. Spline vectors and pass arrays are caller-owned and must remain unchanged
		 * through synchronous rendering. No final Section membership or image completion is implied.
		 */
		struct SectionScenePreparationResult
		{
			/** Complete means all observed occurrences are accounted for and inputs are available or terminally omitted. */
			enum class Readiness
			{
				InvalidPublication,
				StalePublication,
				InvalidObservation,
				IncompleteCandidateCoverage,
				PendingNativeSourceAcquisition,
				PendingSpriteAcquisition,
				TemporarilyUnavailableResources,
				UnresolvedInputs,
				CompleteSceneInputs
			};

			/** Per-observation-occurrence preparation evidence, preserving duplicates and source order. */
			std::vector<KnownSpritePreparationResult::OccurrenceOutcome> OccurrenceOutcomes;
			/** Ready FirstPass sprites, in observation order, borrowing CPU pixel payloads. */
			std::vector<Visuals::BuildingSpriteRenderInput> OrdinarySprites;
			/** Owned spline composition results in observation order. */
			std::vector<SplineRenderInputPreparationResult> SplinePreparations;
			/** Ready SecondPass sprites, in observation order, borrowing CPU pixel payloads. */
			std::vector<Visuals::BuildingSpriteRenderInput> OverlayLastSprites;
			/** Publication and observation evidence retained without publication-owned spans. */
			BuildingCandidateEvidence CandidateEvidence{};
			/** Non-owning identity token checked during borrow while the caller retains the owner. */
			const BuildingCandidatePublication* PublicationIdentity = nullptr;
			Readiness PreparationReadiness = Readiness::InvalidPublication;

			/** Clears owned values; rebuild borrowed spline views after reset or modification. */
			void Reset() noexcept;

			/** Builds temporary views into caller-owned scratch and rejects incomplete or stale inputs.
			 * @param publicationOwner Retained owner whose pixels and observation were prepared.
			 * @param scratch Receives borrowed views into SplinePreparations and must remain unchanged.
			 * @param output Receives ordered sprite/spline spans for one synchronous render call.
			 * @return True only while the publication is current and readiness is CompleteSceneInputs.
			 */
			bool BorrowRenderSceneInput(
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
				StalePublication,
				NumericalRenderingFailed,
				ImageAdoptionRejected,
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

		/** Stores immutable service configuration and creates acquisition resources without I/O.
		 * @param sectionSizeMapUnits Width and height of the service's square cell in map units.
		 * @param sectionResolutionPixels Width and height of the future square image in pixels.
		 * @param buildingsCacheDirectory Service-validated absolute Buildings cache directory.
		 */
		SectionScheduler(
			double sectionSizeMapUnits,
			std::uint32_t sectionResolutionPixels,
			std::filesystem::path buildingsCacheDirectory);

		/** Starts gameplay scheduling against the persistent BuildingScheduler renderer.
		 * @return True when BuildingScheduler enters gameplay-active scheduling state.
		 */
		bool BeginCaptureGameWorldSession();

		/** Ends gameplay-session acquisition and begins cancellation of active request work. */
		void StopCaptureGameWorldSession();
		/** Returns the renderer used by the plugin's direct engine callback. */
		Capture::BuildingSpriteRenderer* GetCapturerLifecycleTarget() noexcept;
		/** Clears the configured sprite cache without changing acquisition-session state.
		 * @return True when SpriteDiskCache clears its configured directory without error.
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

		/** Processes known sprites from exactly one retained, current source publication.
		 * The caller retains publicationOwner for this operation; its borrowed spans and keys are
		 * never retained. Repeated descriptors are processed independently, while BuildingScheduler
		 * suppresses duplicate acquisition by canonical key. Ready pixels are borrowed from the CPU manager;
		 * callers must consume input/support arrays synchronously before manager mutation or shutdown.
		 * Unknown source requirements remain unknown, and no result certifies a Section or scene.
		 * @param publicationOwner Shared owner of the one fixed source publication.
		 * @param output Receives prepared sprite occurrences and individual outcome counts.
		 */
		void PrepareKnownSpriteRequirements(
			const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
			KnownSpritePreparationResult& output);

		/** Prepares neutral inputs for exactly one observation from the explicitly retained publication.
		 * Reuses a publication-level sprite result, composes each spline occurrence with the borrowed
		 * service policy span, and reports complete scene inputs only after final currentness validation.
		 * @param publicationOwner Retained owner for source ordinals and the identity checked on sprite results.
		 * @param observation One borrowed candidate list; its ordinals must match this publication in order.
		 * @param knownSpritePreparation Reusable publication-level outcomes with short-lived pixel views.
		 * @param splinePolicies Borrowed immutable service-owned table indexed directly by source policy index.
		 * @param output Receives caller-owned per-occurrence evidence and neutral scene arrays.
		 * The caller retains the publication and sprite result through immediate synchronous consumption.
		 */
		void PrepareSectionSceneInputs(
			const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
			const BuildingCandidateObservation& observation,
			const KnownSpritePreparationResult& knownSpritePreparation,
			std::span<const SplinePolicy> splinePolicies,
			SectionScenePreparationResult& output) const;

		/** Validates and prepares one exact Section observation before rendering or empty completion.
		 * @param section Service-owned Section that the observation must describe.
		 * @param publicationOwner Retained candidate publication used for preparation.
		 * @param observation Candidate observation to validate and prepare.
		 * @param knownSpritePreparation Existing same-publication sprite outcomes.
		 * @param splinePolicies Borrowed immutable service policy span.
		 * @param output Receives the complete prepared scene when readiness permits.
		 * @param commitResult Receives readiness and stale-publication evidence.
		 * @return True only for a valid exact-cell observation with a current complete scene.
		 */
		bool TryPrepareSectionSceneForCommit(
			const ::Services::Map::Buildings::BuildingMapSection& section,
			const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
			const BuildingCandidateObservation& observation,
			const KnownSpritePreparationResult& knownSpritePreparation,
			std::span<const SplinePolicy> splinePolicies,
			SectionScenePreparationResult& output,
			SectionImageCommitResult& commitResult) const;

		/** Validates currentness and completion after the existing numerical render call.
		 * @param publicationOwner Retained publication whose currentness gates completed output.
		 * @param commitResult Result containing the numerical status and receiving validation failure.
		 * @return True only for a Complete numerical result from a still-current publication.
		 */
		bool IsCompletedRenderCurrentAndComplete(
			const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
			SectionImageCommitResult& commitResult) const;

		/** Prepares complete inputs, commits a zero-candidate Section without rendering, or renders
		 * once and classifies the finalized image before either completing empty or move-adopting it.
		 * CompleteSceneInputs and publication currentness gate either successful outcome. Failed
		 * attempts preserve any previously stored Section image. The caller retains the publication
		 * and borrowed inputs through this synchronous operation.
		 * @param section Service-owned Section rendered and updated on success.
		 * @param publicationOwner Retained publication used for rendering and final currentness check.
		 * @param observation Candidate observation matching the Section cell.
		 * @param knownSpritePreparation Existing same-publication sprite outcomes.
		 * @param splinePolicies Borrowed immutable service policy span.
		 * @param output Receives completion status, image-removal fact, readiness and numerical evidence.
		 */
		void RenderAndCommitSectionImage(
			::Services::Map::Buildings::BuildingMapSection& section,
			const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
			const BuildingCandidateObservation& observation,
			const KnownSpritePreparationResult& knownSpritePreparation,
			std::span<const SplinePolicy> splinePolicies,
			SectionImageCommitResult& output) const;

		/** Completes an empty Section after validated pre-render or post-render evidence.
		 * The caller has validated the exact Section observation and CompleteSceneInputs readiness;
		 * this operation rechecks publication currentness before releasing any stored image.
		 * @param section Service-owned Section whose optional image may be removed.
		 * @param publicationOwner Retained publication whose currentness gates image removal.
		 * @param output Receives CommittedEmpty and whether image storage changed.
		 */
		void CommitEmptySection(
			::Services::Map::Buildings::BuildingMapSection& section,
			const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
			SectionImageCommitResult& output) const;

		/** Validates the expected image byte count and checks exact transparent RGBA16F clear bytes.
		 * @param imageBytes Borrowed completed row-major RGBA16F pixel bytes.
		 * @return True only for a correctly sized image whose every pixel is exactly transparent.
		 */
		bool IsFullyTransparentRgba16fImage(std::span<const std::uint8_t> imageBytes) const noexcept;

		/** Validates that the requested half-open cell corresponds to the supplied Section coordinates.
		 * @param section Service-owned Section whose signed grid location is authoritative.
		 * @param observation Borrowed candidate observation to match against the Section cell.
		 * @return True for the same square cell under the service's map-unit scale.
		 */
		bool IsObservationForSection(
			const ::Services::Map::Buildings::BuildingMapSection& section,
			const BuildingCandidateObservation& observation) const;
		/** Compares coordinates within floating-point rounding tolerance, capped below cell separation.
		 * @param actual Observation coordinate.
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
		static Rendering::Rgba16fImageGrid CreateSectionRenderingGrid(
			const ::Services::Map::Buildings::BuildingMapSection& section,
			double sectionSizeMapUnits,
			std::uint32_t sectionResolutionPixels);
		/** Invokes the existing renderer with the Section's privately owned numerical context.
		 * @param section Section that owns the reusable context.
		 * @param sceneInput Borrowed complete neutral scene valid for the synchronous call.
		 * @param grid Existing zero-origin destination grid.
		 * @param imageBytes Receives finalized bytes only on numerical success.
		 * @return The exact existing SectionRendering outcome.
		 */
		static Rendering::SectionRendering::Status RenderIntoSectionContext(
			::Services::Map::Buildings::BuildingMapSection& section,
			const Visuals::BuildingRenderSceneInput& sceneInput,
			const Rendering::Rgba16fImageGrid& grid,
			std::vector<std::uint8_t>& imageBytes);
		/** Validates one well-formed observation and its exact source-relative ordinal sequence.
		 * @param publication Retained publication whose candidate occurrence order is authoritative.
		 * @param observation Borrowed section observation to validate.
		 * @return True when geometry and all candidate ordinals match the publication.
		 */
		static bool IsValidSectionObservation(
			const BuildingCandidatePublication& publication,
			const BuildingCandidateObservation& observation);
		/** Validates publication identity, currentness and bounded preparation inputs.
		 * @param publicationOwner Explicitly retained source publication.
		 * @param observation One borrowed publication observation.
		 * @param spritePreparation Publication-level sprite outcomes to reuse.
		 * @param readiness Receives the invalid or stale category on rejection.
		 * @return True when preparation may safely begin.
		 */
		static bool ValidateSectionPreparationInputs(
			const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
			const BuildingCandidateObservation& observation,
			const KnownSpritePreparationResult& spritePreparation,
			SectionScenePreparationResult::Readiness& readiness);
		/** Rejects missing, reordered, or mismatched publication-level sprite outcomes.
		 * @param publication Retained publication used to verify ordinal and pass correspondence.
		 * @param spritePreparation Caller-owned result from the same retained publication.
		 * @return True when each occurrence outcome corresponds to this publication.
		 */
		static bool IsCompatibleSpritePreparation(
			const BuildingCandidatePublication& publication,
			const KnownSpritePreparationResult& spritePreparation);
		/** Combines observation evidence without promoting weaker publication facts.
		 * @param publication Publication owning the certified source coverage.
		 * @param observation Evidence and requested region for one Section.
		 * @return Evidence limited by the publication's own coverage and source facts.
		 */
		static BuildingCandidateEvidence GetSectionCandidateEvidence(
			const BuildingCandidatePublication& publication,
			const BuildingCandidateObservation& observation);
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
		/** Preserves acquisition certainty before any resource or numerical preparation.
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
		/** Reduces evidence and individual outcomes without upgrading unknown or pending facts.
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

		/** Preserves source/profile/curve/root distinctions and rejects incomplete prerequisites.
		 * @param source Borrowed occurrence source record.
		 * @param output Receives the precise first source or geometry composition status.
		 * @return True when source, profiles, converted world curve and root are all eligible.
		 */
		static bool CheckSplineSourcePrerequisites(
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

		/** Resolves every occurrence in one borrowed pass without collapsing equal keys.
		 * @param candidates Candidate occurrences whose ordinals resolve records in this publication.
		 * @param publication Retained owner used to resolve publication-local source records.
		 * @param output Accumulates per-requirement readiness and validation outcomes.
		 */
		void PrepareSpriteRequirementSpan(
			std::span<const BuildingCandidateOccurrence> candidates,
			const BuildingCandidatePublication& publication,
			KnownSpritePreparationResult& output);
		/** Clears per-publication counters, identity and short-lived pixel views.
		 * @param output Caller-owned result whose prior pixel borrows are invalidated.
		 */
		static void ResetKnownSpritePreparationResult(KnownSpritePreparationResult& output);
		/** Copies aligned candidate/source facts into one owned outcome per ordinal.
		 * @param candidateData Borrowed occurrences from the retained publication.
		 * @param sourceData Aligned borrowed source records from the same publication.
		 * @param output Receives source-ordered per-occurrence evidence.
		 */
		static void InitializeKnownSpriteOccurrences(
			const BuildingCandidateData& candidateData,
			const BuildingCandidateSourceData& sourceData,
			KnownSpritePreparationResult& output);
		/** Resolves and prepares one candidate occurrence while preserving its outcome.
		 * @param candidate Publication-owned occurrence with its local ordinal.
		 * @param publication Retained owner used to resolve the matching source record.
		 * @param output Receives per-occurrence readiness and any borrowed pixel input.
		 */
		void PrepareSpriteOccurrence(
			const BuildingCandidateOccurrence& candidate,
			const BuildingCandidatePublication& publication,
			KnownSpritePreparationResult& output);
		/** Preserves non-ready source and pass states before cache acquisition.
		 * @param requirement Borrowed source record for one sprite occurrence.
		 * @param output Receives aggregate unknown-requirement evidence.
		 * @param outcome Receives the distinct per-occurrence source/pass state.
		 * @return True when the existing sprite acquisition path may proceed.
		 */
		static void RecordSpriteSourceUncertainty(
			const BuildingCandidateSourceRecord& requirement,
			KnownSpritePreparationResult& output);

		/** Acquires one valid sprite subject and appends a drawable input when placement is known.
		 * @param publicationOrdinal Corresponding ordinal in the retained candidate publication.
		 * @param requirement Borrowed neutral source key and optional validated placement.
		 * @param ownedKey Validated owned copy of the canonical sprite subject.
		 * @param output Accumulates the outcome for this known requirement.
		 */
		void PrepareSpriteRequirement(
			std::uint64_t publicationOrdinal,
			const BuildingCandidateSourceRecord& requirement,
			const Cache::OwnedSpriteKey& ownedKey,
			KnownSpritePreparationResult& output,
			KnownSpritePreparationResult::OccurrenceOutcome& occurrenceOutcome);

		/** Borrows the ready payload, validates placement geometry and appends one paired occurrence.
		 * @param publicationOrdinal Corresponding ordinal in the retained candidate publication.
		 * @param requirement Borrowed known descriptor with its optional validated placement.
		 * @param ownedKey Validated key whose payload is ready through BuildingScheduler.
		 * @param output Accumulates temporary payload and geometry outcomes.
		 * @param occurrenceOutcome Receives this ordinal's independently preserved preparation state.
		 */
		void PrepareReadySpriteRequirement(
			std::uint64_t publicationOrdinal,
			const BuildingCandidateSourceRecord& requirement,
			const Cache::OwnedSpriteKey& ownedKey,
			KnownSpritePreparationResult& output,
			KnownSpritePreparationResult::OccurrenceOutcome& occurrenceOutcome);
		/** Builds one ready sprite input from an already acquired CPU payload.
		 * @param publicationOrdinal Exact source occurrence ordinal.
		 * @param requirement Borrowed source values and placement for that occurrence.
		 * @param payload Ready payload whose pixels remain borrowed.
		 * @param output Receives the input view and any geometry outcome.
		 * @param occurrenceOutcome Receives the matching per-ordinal state.
		 */
		void AppendReadySpriteInput(
			std::uint64_t publicationOrdinal,
			const BuildingCandidateSourceRecord& requirement,
			const Cache::ProcessedSpritePayload& payload,
			KnownSpritePreparationResult& output,
			KnownSpritePreparationResult::OccurrenceOutcome& occurrenceOutcome);

		/** Derives the independent world-space XY envelope from the prepared projected quad.
		 * @param renderInput Prepared placement, capture metadata and projected image edges.
		 * @param output Receives the minimum and maximum world-space XY corner coordinates.
		 * @return True when SpriteGeometry builds all finite projected world-space corners.
		 */
		bool TryBuildPreparedSpriteSupport(
			const Visuals::BuildingSpriteRenderInput& renderInput,
			KnownSpritePreparationResult::PreparedSpriteSupport& output) const noexcept;

		/** Rechecks currentness and withdraws all borrowed render inputs if the source went stale.
		 * @param publicationOwner Retained owner of the source used for this preparation.
		 * @param output Preparation result whose borrowed arrays are cleared when stale.
		 * @return True when the fixed publication remains current after processing.
		 */
		bool RevalidatePreparedPublication(
			const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
			KnownSpritePreparationResult& output) const noexcept;

		const double sectionSizeMapUnits_;
		const std::uint32_t sectionResolutionPixels_;
		std::unique_ptr<Cache::SpriteDiskCache> spriteDiskCache_;
		std::unique_ptr<Cache::CpuSpriteSourceManager> cpuSpriteSourceManager_;
		std::unique_ptr<BuildingScheduler> buildingScheduler_;
	};
}
