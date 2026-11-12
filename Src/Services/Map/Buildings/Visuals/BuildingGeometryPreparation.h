// Prepares reusable world-space Building Map geometry without raster destinations.
#pragma once

#include "BuildingRenderSceneInput.h"
#include "BuildingVisualsContext.h"
#include "BuildingVisualsResult.h"
#include "PreparedBuildingGeometry.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Services::Map::Buildings::Visuals
{
	/** Advances renderer-independent geometry preparation into caller-owned Section scene storage. */
	class BuildingGeometryPreparation final
	{
	public:
		/** Distinguishes deadline continuation from complete and terminal preparation outcomes. */
		enum class Status
		{
			Pending,
			Complete,
			CompleteWithOmissions,
			InvalidInputs,
			NumericalFailure
		};

		/** Reports the current operation state and bounded numerical work count. */
		struct StepResult
		{
			Status CurrentStatus = Status::Pending;
			std::size_t WorkUnitsConsumed = 0;
		};

		/** Retains phase cursors and outcomes only; scene spans and geometry remain externally owned. */
		class PreparationCursor final
		{
			friend class BuildingGeometryPreparation;

		public:
		/** Resets only a fresh or terminal cursor for a new Section-owned output scene. */
			void Reset() noexcept;
			/** Borrows positional numerical outcomes until this cursor is reset. */
			const BuildingVisualsResult& ContributorResults() const noexcept;
			/** Returns the cursor's terminal classification, or Pending before completion. */
			Status CurrentStatus() const noexcept;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			/** Borrows the failed operation's scalar evidence until the cursor is reset. */
			const SectionFailureDetails& FailureDetails() const noexcept
			{
				return Failure;
			}
#endif

		private:
			/** Orders geometry passes and identifies the non-destination terminal boundary. */
			enum class Phase
			{
				OrdinarySprites,
				OcclusionIndex,
				Splines,
				OverlayLastSprites,
				Complete
			};

			Phase CurrentPhase = Phase::OrdinarySprites;
			std::size_t OrdinarySpriteIndex = 0;
			std::size_t SplineIndex = 0;
			std::size_t SplineSurfaceIndex = 0;
			std::size_t OverlaySpriteIndex = 0;
			std::uint8_t PendingOpacity = 0;
			bool Initialized = false;
			bool SplineSamplingPrepared = false;
			bool HasAcceptedOmissions = false;
			Status TerminalStatus = Status::Pending;
			BuildingVisualsResult Results;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			SectionFailureDetails Failure;
#endif
		};

		/** Advances ordinary sprites, the occluder index, splines and overlay sprites in painter order.
		 * Scene views are borrowed only for this call; continuations must supply equivalent ordered inputs,
		 * and the caller retains all source owners between calls.
		 * Output must be an incomplete Section scene whose status slots already match the exact
		 * publication occurrence count. Previously prepared sprite contributions are validated
		 * and reused; unresolved slots keep the scene pending rather than being claimed complete.
		 */
		static StepResult Advance(
			const BuildingRenderSceneInput& scene,
			BuildingVisualsContext& scratch,
			PreparedBuildingGeometry& output,
			PreparationCursor& cursor,
			std::chrono::steady_clock::time_point deadline);

	private:
		/** Validates a fresh output and initializes reusable numerical scratch once. */
		static bool Initialize(
			const BuildingRenderSceneInput& scene,
			BuildingVisualsContext& scratch,
			PreparedBuildingGeometry& output,
			PreparationCursor& cursor);
		/** Validates exact ordinal ownership across the supplied scene and existing sprite records. */
		static bool ValidateContributionOwnership(
			const BuildingRenderSceneInput& scene,
			const PreparedBuildingGeometry& output,
			PreparationCursor& cursor);
		/** Indexes supplied scene contributors by ordinal and rejects duplicate or out-of-range inputs. */
		static bool IndexSceneInputs(
			const BuildingRenderSceneInput& scene,
			std::vector<const BuildingSpriteRenderInput*>& ordinaryInputs,
			std::vector<const BuildingSplineRenderInput*>& splineInputs,
			std::vector<const BuildingSpriteRenderInput*>& overlayInputs,
			PreparationCursor& cursor);
		/** Indexes overlay inputs without allowing an ordinal to be owned by two passes. */
		static bool IndexOverlayInputs(
			const BuildingRenderSceneInput& scene,
			std::vector<const BuildingSpriteRenderInput*>& overlayInputs,
			const std::vector<const BuildingSpriteRenderInput*>& ordinaryInputs,
			const std::vector<const BuildingSplineRenderInput*>& splineInputs,
			PreparationCursor& cursor);
		/** Verifies existing sprite geometry matches the exact input occurrence and painter pass. */
		static bool ValidatePreparedSpriteRecords(
			const PreparedBuildingGeometry& output,
			const std::vector<const BuildingSpriteRenderInput*>& ordinaryInputs,
			const std::vector<const BuildingSpriteRenderInput*>& overlayInputs,
			PreparationCursor& cursor);
		/** Matches one already owned sprite against the scene's direct ordinal input slot. */
		static bool MatchesPreparedSprite(
			std::uint64_t ordinal,
			const PreparedBuildingGeometry::SpriteOccurrence& occurrence,
			const std::vector<const BuildingSpriteRenderInput*>& inputs,
			std::vector<bool>& owned,
			const PreparedBuildingGeometry& output,
			PreparationCursor& cursor);
		/** Rejects failed states or terminal omissions that still claim a drawable scene input. */
		static bool ValidateSceneOccurrenceStates(
			const PreparedBuildingGeometry& output,
			const std::vector<const BuildingSpriteRenderInput*>& ordinaryInputs,
			const std::vector<const BuildingSplineRenderInput*>& splineInputs,
			const std::vector<const BuildingSpriteRenderInput*>& overlayInputs,
			PreparationCursor& cursor);
		/** Seeds the operation's occlusion workspace from already prepared Section sprite values. */
		static bool SeedPreparedOccluders(
			const PreparedBuildingGeometry& output,
			BuildingVisualsContext& scratch);
		/** Advances one phase boundary or one bounded numerical contribution. */
		static Status AdvanceOne(
			const BuildingRenderSceneInput& scene,
			BuildingVisualsContext& scratch,
			PreparedBuildingGeometry& output,
			PreparationCursor& cursor,
			std::size_t& workUnitsConsumed);
		/** Advances spline, overlay and terminal phases after ordinary occluders are indexed. */
		static Status AdvanceRemainingPhase(
			const BuildingRenderSceneInput& scene,
			BuildingVisualsContext& scratch,
			PreparedBuildingGeometry& output,
			PreparationCursor& cursor,
			std::size_t& workUnitsConsumed);
		/** Prepares one ordinary or overlay sprite without issuing an image command. */
		static Status PrepareSprite(
			const BuildingSpriteRenderInput& sprite,
			std::size_t sceneIndex,
			bool overlay,
			BuildingVisualsContext& scratch,
			PreparedBuildingGeometry& output,
			PreparationCursor& cursor);
		/** Records one sprite failure without publishing its incomplete geometry. */
		static Status RecordSpriteFailure(
			const BuildingSpriteRenderInput& sprite,
			bool overlay,
			BuildingVisualsContributorOutcome outcome,
			PreparedBuildingGeometry& output,
			PreparationCursor& cursor,
			Status status);
		/** Adds a valid sprite occurrence and its ordinary-pass occluder metadata. */
		static Status RetainSprite(
			const BuildingSpriteRenderInput& sprite,
			std::size_t sceneIndex,
			bool overlay,
			const SpriteWorldQuad& worldQuad,
			BuildingVisualsContext& scratch,
			PreparedBuildingGeometry& output,
			PreparationCursor& cursor,
			BuildingVisualsContributorOutcome& outcome);
		/** Samples and prepares one spline surface or ordered alternative. */
		static Status AdvanceSpline(
			const BuildingSplineRenderInput& spline,
			std::size_t splineIndex,
			BuildingVisualsContext& scratch,
			PreparedBuildingGeometry& output,
			PreparationCursor& cursor);
		/** Records one malformed spline input and marks its publication occurrence failed. */
		static void RecordInvalidSplineInput(
			std::size_t splineIndex,
			std::uint64_t ordinal,
			PreparedBuildingGeometry& output,
			PreparationCursor& cursor);
		/** Prepares one surface candidate, retaining rejected alternatives only as outcomes. */
		static Status PrepareSplineCandidate(
			const BuildingSplineRenderInput& spline,
			std::size_t splineIndex,
			std::size_t surfaceIndex,
			bool alternative,
			BuildingVisualsContext& scratch,
			PreparedBuildingGeometry& output,
			PreparationCursor& cursor);
		/** Builds shared sampling/surface evidence and reports drawable-alternative eligibility. */
		static bool PrepareSplineCandidateGeometry(
			const BuildingSplineRenderInput& spline,
			const PreparedSpline& preparedSpline,
			const BuildingSplineSurfaceRenderInput& surface,
			std::size_t splineIndex,
			std::size_t surfaceIndex,
			BuildingVisualsContext& scratch,
			BuildingVisualsContributorOutcome& outcome);
		/** Records a prepared candidate and advances the corresponding spline cursor. */
		static Status FinishSplineCandidate(
			const BuildingSplineRenderInput& spline,
			std::size_t splineIndex,
			std::size_t surfaceIndex,
			bool alternative,
			BuildingVisualsContext& scratch,
			PreparedBuildingGeometry& output,
			PreparationCursor& cursor,
			BuildingVisualsContributorOutcome& outcome);
		/** Clips and owns one successfully prepared surface's numeric triangles and appearance. */
		static Status RetainPreparedSurface(
			const BuildingSplineRenderInput& spline,
			const BuildingSplineSurfaceRenderInput& surface,
			std::size_t splineIndex,
			std::size_t surfaceIndex,
			BuildingVisualsContext& scratch,
			PreparedBuildingGeometry& output,
			PreparationCursor& cursor,
			BuildingVisualsContributorOutcome& outcome);
		/** Performs clipping and active solid/textured vertex conversion in reusable scratch. */
		static bool PrepareVisibleTriangles(
			const BuildingSplineSurfaceRenderInput& surface,
			BuildingVisualsContext& scratch,
			PreparationCursor& cursor,
			BuildingVisualsContributorOutcome& outcome);
		/** Validates renderer-independent appearance metadata without retaining a texture pixel span. */
		static bool ValidateAppearanceMetadata(
			const BuildingSplineSurfaceRenderInput& surface) noexcept;
		/** Moves the one active triangle vector into an owned surface record. */
		static void AppendPreparedSurface(
			const BuildingSplineRenderInput& spline,
			const BuildingSplineSurfaceRenderInput& surface,
			std::size_t surfaceIndex,
			BuildingVisualsContext& scratch,
			PreparedBuildingGeometry& output,
			PreparationCursor& cursor);
		/** Records a no-surface contributor or an exhausted drawable alternative set. */
		static Status CompleteSplineWithoutSurface(
			const BuildingSplineRenderInput& spline,
			std::size_t splineIndex,
			BuildingVisualsContext& scratch,
			PreparedBuildingGeometry& output,
			PreparationCursor& cursor);
		/** Marks one spline occurrence terminal and advances to the next contributor. */
		static void FinishSpline(
			std::uint64_t ordinal,
			PreparedBuildingGeometry& output,
			PreparationCursor& cursor,
			bool omitted);
		/** Writes one ordinal outcome after validating its representable vector index. */
		static bool SetContributionStatus(
			PreparedBuildingGeometry& output,
			std::uint64_t ordinal,
			PreparedBuildingGeometry::ContributionStatus status);
		/** Stores explicit failure evidence and terminates without marking output complete. */
		static StepResult Terminate(
			PreparationCursor& cursor,
			Status status,
			std::size_t workUnitsConsumed) noexcept;
		/** Completes an empty or fully resolved output scene at the final phase. */
		static StepResult Complete(
			PreparedBuildingGeometry& output,
			PreparationCursor& cursor,
			std::size_t workUnitsConsumed) noexcept;
	};
}
