// Owns Building Map acquisition resources, session lifecycle and neutral source preparation.
#pragma once

#include "../BuildingCandidatePublication.h"
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

namespace Services::Map::Buildings::Cache
{
	class CpuSpriteSourceManager;
	class SpriteDiskCache;
}

namespace Services::Map::Buildings::Scheduling
{
	class BuildingScheduler;
	class SectionSchedulingCoordinator;

	/** Owns sibling acquisition resources; its coordinator delegates session lifetime and shutdown progression.
	 * SpriteDiskCache outlives the CPU manager, which outlives BuildingScheduler and its renderer.
	 * Construction is inert; session shutdown must finish before this owner is destroyed.
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

		/** Begins the existing BuildingScheduler session using the stored service image scale.
		 * @param sessionGeneration Game-world session identity admitted by the service.
		 * @return True when BuildingScheduler establishes this renderer session.
		 */
		bool BeginCaptureGameWorldSession(std::uint64_t sessionGeneration);

		/** Advances existing ordered renderer retirement without performing destructor cleanup.
		 * @return True when native retirement is complete and session state is released.
		 */
		bool AdvanceCaptureGameWorldSessionShutdown();

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
			KnownSpritePreparationResult& output);

		/** Borrows the ready payload, validates placement geometry and appends one paired occurrence.
		 * @param publicationOrdinal Corresponding ordinal in the retained candidate publication.
		 * @param requirement Borrowed known descriptor with its optional validated placement.
		 * @param ownedKey Validated key whose payload is ready through BuildingScheduler.
		 * @param output Accumulates temporary payload and geometry outcomes.
		 */
		void PrepareReadySpriteRequirement(
			std::uint64_t publicationOrdinal,
			const BuildingCandidateSourceRecord& requirement,
			const Cache::OwnedSpriteKey& ownedKey,
			KnownSpritePreparationResult& output);

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
