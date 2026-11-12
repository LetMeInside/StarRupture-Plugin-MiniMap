// Submits immutable Section-owned Building Map geometry to an image destination.
#pragma once

#include "BuildingRenderSceneInput.h"
#include "BuildingVisualsContext.h"
#include "PreparedBuildingGeometry.h"
#include "../IImageDestination.h"

#include <cstddef>
#include <chrono>
#include <cstdint>

namespace Services::Map::Buildings::Visuals
{
	class BuildingGeometryPreparation;

	/** Submits prepared scene geometry in painter order without recalculating world geometry.
	 * Resource views are borrowed for each call; the caller retains their immutable owners across continuation.
	 */
	class BuildingVisuals final
	{
		friend class BuildingGeometryPreparation;

	public:
		/** Reports destination work consumed and whether the raster submission reached its terminal phase. */
		struct SceneSliceResult
		{
			std::size_t WorkUnitsConsumed = 0;
			bool Complete = false;
		};

		/** Retains raster pass and destination continuation state for one image operation. */
		struct SceneCursor
		{
			/** Creates a fresh raster cursor. */
			SceneCursor() noexcept = default;
			SceneCursor(const SceneCursor&) = delete;
			SceneCursor& operator=(const SceneCursor&) = delete;
			SceneCursor(SceneCursor&&) = delete;
			SceneCursor& operator=(SceneCursor&&) = delete;
			/** Resets a fresh or terminal cursor; active submission cannot be reset. */
			void Reset() noexcept;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			/** Returns the first failure observed by this prepared raster operation, if any. */
			const SectionFailureDetails& FailureDetails() const noexcept
			{
				return Failure;
			}
#endif

		private:
			friend class BuildingVisuals;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			/** Copies the first failed raster decision without retaining contributor views. */
			void RecordFailure(const SectionFailureDetails& failure) noexcept;
			SectionFailureDetails Failure;
#endif
			/** Identifies the next ordered raster pass. */
			enum class Phase
			{
				OrdinarySprites,
				Splines,
				OverlayLastSprites,
				Complete
			};
			Phase CurrentPhase = Phase::OrdinarySprites;
			std::size_t OrdinarySpriteIndex = 0;
			std::size_t SplineSurfaceIndex = 0;
			std::size_t OverlaySpriteIndex = 0;
			BuildingVisualsContributorOutcome PendingOutcome{};
			IImageDestination::SubmissionCursor Submission{};
			bool Initialized = false;
			bool DestinationSubmissionActive = false;
			bool Terminal = false;
		};

		/** Resumes raster submission over immutable Section geometry and fresh borrowed resource views.
		 * The caller retains sprite pixels and texture owners across calls and uses the same cursor and destination.
		 */
		static SceneSliceResult StepPreparedScene(
			const PreparedBuildingGeometry& geometry,
			const BuildingRenderSceneInput& resourceScene,
			IImageDestination& destination,
			BuildingVisualsContext& context,
			SceneCursor& cursor,
			std::chrono::steady_clock::time_point deadline);
		/** Returns the caller-owned result only after the supplied raster cursor completed.
		 * The pointer remains valid while the context result is unchanged.
		 */
		static const BuildingVisualsResult* TryGetCompletedSceneResult(
			const BuildingVisualsContext& context,
			const SceneCursor& cursor) noexcept;

	private:
		/** Distinguishes progress from a safe yield when no destination work was consumed. */
		enum class SceneAdvanceResult
		{
			Progressed,
			YieldWithoutWork
		};
		/** Clears destination continuation values after a drawable reaches its terminal outcome. */
		static void ResetSubmissionProgress(SceneCursor& cursor) noexcept;
		/** Clears raster reports and reusable occluder scratch before one image operation. */
		static void ResetOperation(BuildingVisualsContext& context);
		/** Initializes result capacity and pass cursors for the completed immutable scene. */
		static void InitializePreparedSceneSubmission(
			const PreparedBuildingGeometry& geometry,
			const BuildingRenderSceneInput& resourceScene,
			BuildingVisualsContext& context,
			SceneCursor& cursor);
		/** Advances one raster pass over prepared geometry and borrowed resource views. */
		static SceneAdvanceResult AdvancePreparedSceneStep(
			const PreparedBuildingGeometry& geometry,
			const BuildingRenderSceneInput& resourceScene,
			IImageDestination& destination,
			BuildingVisualsContext& context,
			SceneCursor& cursor,
			SceneSliceResult& result,
			std::chrono::steady_clock::time_point deadline);
		/** Advances one sprite pass using direct occurrence correspondence. */
		static SceneAdvanceResult AdvancePreparedSpritePass(
			const PreparedBuildingGeometry& geometry,
			const BuildingRenderSceneInput& resourceScene,
			IImageDestination& destination,
			BuildingVisualsContext& context,
			SceneCursor& cursor,
			SceneSliceResult& result,
			std::chrono::steady_clock::time_point deadline,
			bool overlay);
		/** Records a valid zero-area Section sprite as culled without destination work. */
		static SceneAdvanceResult FinishSectionExcludedSprite(
			std::size_t sceneIndex,
			bool overlay,
			BuildingVisualsContext& context,
			SceneCursor& cursor,
			SceneSliceResult& result);
		/** Records an exact source/geometry correspondence failure without drawing a substitute. */
		static SceneAdvanceResult FailPreparedSpriteInput(
			bool overlay,
			std::size_t index,
			BuildingVisualsContext& context,
			SceneCursor& cursor,
			SceneSliceResult& result);
		/** Resumes one sprite image command using its prepared world-space quad. */
		static SceneAdvanceResult SubmitPreparedSpriteImage(
			const PreparedBuildingGeometry::SpriteOccurrence& occurrence,
			const BuildingSpriteRenderInput& source,
			bool overlay,
			IImageDestination& destination,
			BuildingVisualsContext& context,
			SceneCursor& cursor,
			SceneSliceResult& result,
			std::chrono::steady_clock::time_point deadline);
		/** Records a completed destination outcome and advances the sprite pass once. */
		static SceneAdvanceResult FinishPreparedSpriteSubmission(
			bool overlay,
			BuildingVisualsContext& context,
			SceneCursor& cursor,
			SceneSliceResult& result,
			std::size_t consumed);
		/** Resolves a source spline through its stable descriptor index and validates correspondence. */
		static const BuildingSplineSurfaceRenderInput* FindPreparedSurfaceSource(
			const PreparedBuildingGeometry::SplineSurface& surface,
			const BuildingRenderSceneInput& resourceScene,
			SceneCursor& cursor) noexcept;
		/** Advances one prepared spline surface or moves to the overlay pass. */
		static SceneAdvanceResult AdvancePreparedSplinePass(
			const PreparedBuildingGeometry& geometry,
			const BuildingRenderSceneInput& resourceScene,
			IImageDestination& destination,
			BuildingVisualsContext& context,
			SceneCursor& cursor,
			SceneSliceResult& result,
			std::chrono::steady_clock::time_point deadline);
		/** Records a missing or mismatched spline appearance as a terminal raster failure. */
		static SceneAdvanceResult FailPreparedSplineSource(
			const PreparedBuildingGeometry::SplineSurface& surface,
			BuildingVisualsContext& context,
			SceneCursor& cursor,
			SceneSliceResult& result);
		/** Resumes submission over immutable prepared triangle arrays. */
		static SceneAdvanceResult SubmitPreparedSplineTriangles(
			const PreparedBuildingGeometry::SplineSurface& prepared,
			const BuildingSplineSurfaceRenderInput& source,
			IImageDestination& destination,
			BuildingVisualsContext& context,
			SceneCursor& cursor,
			SceneSliceResult& result,
			std::chrono::steady_clock::time_point deadline);
		/** Borrows prepared triangles and the exact appearance resource for one destination call. */
		static IImageDestination::SubmissionSliceResult StepPreparedSplineBatch(
			const PreparedBuildingGeometry::SplineSurface& prepared,
			const BuildingSplineSurfaceRenderInput& source,
			IImageDestination& destination,
			SceneCursor& cursor,
			std::chrono::steady_clock::time_point deadline);
		/** Converts visible world triangles into reusable destination batch scratch. */
		static bool ConvertSolidSurfaceTriangles(
			const BuildingSplineSurfaceRenderInput& surface,
			BuildingVisualsContext& context,
			std::uint8_t& opacity);
		/** Converts visible world triangles and UVs into reusable destination batch scratch. */
		static bool ConvertTexturedSurfaceTriangles(
			const BuildingSplineSurfaceRenderInput& surface,
			BuildingVisualsContext& context,
			std::uint8_t& opacity);
		/** Adds a valid captured sprite volume to occlusion scratch for shared preparation. */
		static bool AddOccluder(
			const BuildingSpriteRenderInput& sprite,
			const SpriteWorldQuad& worldQuad,
			std::size_t ordinal,
			BuildingVisualsContext& context);
		/** Builds an occlusion index from already prepared sprite occluders. */
		static SplineOcclusionStatus PrepareOcclusionIndex(BuildingVisualsContext& context);
		/** Reports whether the reusable numerical surface contains accepted indexed triangles. */
		static bool HasDrawableSplineSurface(const PreparedSplineSurface& preparedSurface) noexcept;
		/** Prepares a surface using its component override or spline transform fallback. */
		static void PrepareSplineSurfaceGeometry(
			const BuildingSplineRenderInput& spline,
			const PreparedSpline& preparedSpline,
			const BuildingSplineSurfaceRenderInput& surface,
			BuildingVisualsContext& context,
			BuildingVisualsContributorOutcome& outcome);
		/** Marks the aggregate raster result incomplete when a drawable failed. */
		static void MarkIncompleteOnFailure(
			BuildingVisualsResult& result,
			const BuildingVisualsContributorOutcome& outcome);
		/** Converts normalized opacity into the destination's 8-bit precision. */
		static bool TryConvertOpacity(double opacity, std::uint8_t& output) noexcept;
	};
}
