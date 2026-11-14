// Prepares reusable world-space Building Map geometry without raster destinations.
#pragma once

#include "BuildingRenderSceneInput.h"
#include "BuildingVisualsContext.h"
#include "BuildingVisualsResult.h"
#include "PreparedBuildingGeometry.h"
#include "../BuildingMapRegion.h"

#include <array>
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
			bool SplineHasDrawableSurface = false;
			bool SplineHasNumericalCandidateRejection = false;
			bool HasSectionRegion = false;
			BuildingCollectionRegion SectionRegion{};
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
			const BuildingCollectionRegion& sectionRegion,
			BuildingVisualsContext& scratch,
			PreparedBuildingGeometry& output,
			PreparationCursor& cursor,
			std::chrono::steady_clock::time_point deadline);
		/** Clips one projected sprite quad into fixed Section-owned triangle storage.
		 * @param worldQuad Original finite XYZ quad in source-corner order.
		 * @param sectionRegion Exact half-open Section rectangle in the same world/map units.
		 * @param triangles Receives up to six triangles with canonical sprite UVs in [0, 1].
		 * @param triangleCount Receives the drawable triangle count; zero is valid geometric exclusion.
		 * @return False only for invalid bounds or non-finite/interpolation failure.
		 */
		static bool TryClipSpriteToSection(
			const SpriteWorldQuad& worldQuad,
			const BuildingCollectionRegion& sectionRegion,
			std::array<TexturedTriangle, 6>& triangles,
			std::size_t& triangleCount) noexcept;

	private:
		/** Identifies the four ordered half-plane clips of a Section rectangle. */
		enum class SectionClipBoundary
		{
			Left,
			Right,
			Top,
			Bottom
		};

		/** Validates finite map-unit bounds with representable positive half-open edges. */
		static bool IsValidSectionRegion(const BuildingCollectionRegion& sectionRegion) noexcept;
		/** Confirms a continuation uses the exact same Section rectangle as its first slice. */
		static bool MatchesSectionRegion(
			const BuildingCollectionRegion& first,
			const BuildingCollectionRegion& second) noexcept;
		/** Clips one polygon vertex buffer against all four Section boundaries. */
		static bool TryClipPolygonToSection(
			const std::array<TexturedTriangleVertex, 8>& input,
			std::size_t inputCount,
			const BuildingCollectionRegion& sectionRegion,
			std::array<TexturedTriangleVertex, 8>& output,
			std::size_t& outputCount) noexcept;
		/** Clips one ordered polygon against a single rectangle half-plane. */
		static bool TryClipPolygonAgainstBoundary(
			const std::array<TexturedTriangleVertex, 8>& input,
			std::size_t inputCount,
			const BuildingCollectionRegion& sectionRegion,
			SectionClipBoundary boundary,
			std::array<TexturedTriangleVertex, 8>& output,
			std::size_t& outputCount) noexcept;
		/** Interpolates world XYZ and UV at an exact Section boundary crossing. */
		static bool TryInterpolateSectionBoundaryVertex(
			const TexturedTriangleVertex& first,
			const TexturedTriangleVertex& second,
			const BuildingCollectionRegion& sectionRegion,
			SectionClipBoundary boundary,
			TexturedTriangleVertex& output) noexcept;
		/** Appends one finite polygon vertex while removing exact adjacent duplicates. */
		static bool TryAppendClipVertex(
			const TexturedTriangleVertex& vertex,
			std::array<TexturedTriangleVertex, 8>& output,
			std::size_t& outputCount) noexcept;
		/** Triangulates a clipped sprite polygon and drops zero-area XY triangles. */
		static bool TryTriangulateSpritePolygon(
			const std::array<TexturedTriangleVertex, 8>& polygon,
			std::size_t polygonCount,
			std::array<TexturedTriangle, 6>& triangles,
			std::size_t& triangleCount) noexcept;
		/** Clips converted spline triangles into reusable Section-owned context vectors. */
		static bool ClipVisibleTrianglesToSection(
			const BuildingCollectionRegion& sectionRegion,
			BuildingVisualsContext& scratch);
		/** Clips one solid triangle, preserving XYZ and deterministic fan ordering. */
		static bool ClipSolidTriangleToSection(
			const SolidTriangle& triangle,
			const BuildingCollectionRegion& sectionRegion,
			std::vector<SolidTriangle>& output);
		/** Clips one textured triangle, interpolating every XYZ and UV component. */
		static bool ClipTexturedTriangleToSection(
			const TexturedTriangle& triangle,
			const BuildingCollectionRegion& sectionRegion,
			std::vector<TexturedTriangle>& output);
		/** Reports whether a world-space XYZ triangle has finite, nonzero projected XY area. */
		static bool HasPositiveFiniteProjectedArea(const SolidTriangle& triangle) noexcept;
		/** Reports whether a textured triangle has finite, nonzero projected XY area. */
		static bool HasPositiveFiniteProjectedArea(const TexturedTriangle& triangle) noexcept;
		/** Calculates signed doubled XY area for one textured triangle. */
		static double ProjectedSignedArea(const TexturedTriangle& triangle) noexcept;
		/** Advances the four half-plane clips without changing source coordinate orientation. */
		static bool IsInsideSectionBoundary(
			const TexturedTriangleVertex& vertex,
			const BuildingCollectionRegion& sectionRegion,
			SectionClipBoundary boundary) noexcept;
		/** Returns the X/Y coordinate of one selected Section edge. */
		static double SectionBoundaryCoordinate(
			const BuildingCollectionRegion& sectionRegion,
			SectionClipBoundary boundary) noexcept;
		/** Tests whether all XYZ and UV attributes of one clipping vertex are finite. */
		static bool IsFiniteClipVertex(const TexturedTriangleVertex& vertex) noexcept;
		/** Compares every interpolated attribute to suppress exact adjacent polygon duplicates. */
		static bool AreClipVerticesEqual(
			const TexturedTriangleVertex& first,
			const TexturedTriangleVertex& second) noexcept;
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
			const BuildingCollectionRegion& sectionRegion,
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
			const BuildingCollectionRegion& sectionRegion,
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
		/** Records a valid surface with no post-occlusion area inside this Section. */
		static Status FinishSplineSectionExclusion(
			const BuildingSplineRenderInput& spline,
			std::size_t splineIndex,
			bool alternative,
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
			const BuildingCollectionRegion& sectionRegion,
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
