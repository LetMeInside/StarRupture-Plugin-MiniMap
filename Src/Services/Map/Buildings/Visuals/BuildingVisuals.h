// Coordinates one synchronous, game-independent Building Map scene drawing pass.
#pragma once

#include "BuildingRenderSceneInput.h"
#include "BuildingVisualsContext.h"

#include <cstddef>
#include <cstdint>

namespace Services::Map::Buildings::Visuals
{
	/** Draws borrowed neutral scene inputs through the supplied destination without owning rendering state.
	 * All reusable numerical state and positional outcomes remain in the caller-owned context.
	 */
	class BuildingVisuals final
	{
	public:
		/** Draws ordinary sprites, clipped spline surfaces, then overlay sprites synchronously.
		 * Scene spans and their pixel/texture views must remain valid until this call returns.
		 * The returned report belongs to context and remains valid until its next DrawScene call.
		 * @param scene Borrowed neutral contributors in established drawing order.
		 * @param destination Borrowed synchronous image and geometry submission target.
		 * @param context Caller-owned numerical storage for this independent render sequence.
		 * @return Aggregate and positional outcomes owned by context.
		 */
		static const BuildingVisualsResult& DrawScene(
			const BuildingRenderSceneInput& scene,
			IImageDestination& destination,
			BuildingVisualsContext& context);

	private:
		/** Clears this operation's reports and candidate occluders without releasing vector capacity.
		 * @param context Caller-owned storage being prepared for one new operation.
		 */
		static void ResetOperation(BuildingVisualsContext& context);
		/** Submits ordinary images in order and records successful submissions as potential occluders.
		 * @param scene Borrowed ordered sprite inputs.
		 * @param destination Synchronous image destination.
		 * @param context Receives outcomes and reusable occluder values.
		 */
		static void DrawOrdinarySprites(
			const BuildingRenderSceneInput& scene,
			IImageDestination& destination,
			BuildingVisualsContext& context);
		/** Builds one image command and projected XYZ quad using SpriteGeometry.
		 * @param sprite Borrowed neutral image and placement values.
		 * @param command Receives the borrowed source and projected XY submission geometry.
		 * @param worldQuad Receives all four projected XYZ corners for possible occlusion.
		 * @return True when the existing geometry operation accepts the inputs.
		 */
		static bool BuildImageCommand(
			const BuildingSpriteRenderInput& sprite,
			ImageSubmissionCommand& command,
			SpriteWorldQuad& worldQuad);
		/** Adds full capture-volume occlusion geometry after a successful ordinary image call.
		 * @param sprite Source placement and full capture bounds.
		 * @param worldQuad Projected corners corresponding to the submitted image.
		 * @param ordinal Per-operation ordinary-sprite order key.
		 * @param context Receives the owned numerical occluder value.
		 * @return True when top height and quad are valid.
		 */
		static bool AddOccluder(
			const BuildingSpriteRenderInput& sprite,
			const SpriteWorldQuad& worldQuad,
			std::size_t ordinal,
			BuildingVisualsContext& context);
		/** Prepares one BVH from the eligible ordinary sprite set.
		 * @param context Supplies occluders and receives reusable index storage.
		 * @return The numerical index preparation status.
		 */
		static SplineOcclusionStatus PrepareOcclusionIndex(BuildingVisualsContext& context);
		/** Samples each spline once, then processes all surfaces or its ordered alternatives.
		 * @param scene Borrowed ordered spline and surface inputs.
		 * @param destination Synchronous triangle destination.
		 * @param context Reuses sampled and surface numerical storage plus positional outcomes.
		 */
		static void DrawSplineSurfaces(
			const BuildingRenderSceneInput& scene,
			IImageDestination& destination,
			BuildingVisualsContext& context);
		/** Prepares and clips one surface, then submits its canonical visible triangles.
		 * @param spline Borrowed neutral source and transform values.
		 * @param preparedSpline Reusable samples prepared once for this contributor.
		 * @param surface Borrowed dimensions and appearance for this surface.
		 * @param splineIndex Per-scene spline position for positional outcome reporting.
		 * @param surfaceIndex Per-spline surface position for positional outcome reporting.
		 * @param context Reusable geometry, clipping and outcome storage.
		 * @param destination Synchronous triangle destination.
		 */
		static void DrawSplineSurface(
			const BuildingSplineRenderInput& spline,
			const PreparedSpline& preparedSpline,
			const BuildingSplineSurfaceRenderInput& surface,
			std::size_t splineIndex,
			std::size_t surfaceIndex,
			BuildingVisualsContext& context,
			IImageDestination& destination);
		/** Tries alternatives in input order and draws only the first with prepared indexed triangles.
		 * Rejected candidate outcomes are retained without affecting aggregate completeness if a later
		 * alternative is selected; clipping or submission after selection never causes fallback.
		 * @param spline Borrowed common curve, sampling policy and root transform with ordered candidates.
		 * @param preparedSpline One centerline sampling result shared by all candidates.
		 * @param splineIndex Positional contributor index in the scene.
		 * @param context Reusable candidate geometry, clipping storage and result owner.
		 * @param destination Synchronous triangle destination.
		 */
		static void DrawFirstDrawableSplineAlternative(
			const BuildingSplineRenderInput& spline,
			const PreparedSpline& preparedSpline,
			std::size_t splineIndex,
			BuildingVisualsContext& context,
			IImageDestination& destination);
		/** Reports whether preparation produced at least one accepted indexed surface cell.
		 * SplineSurfaceGeometry appends accepted cells only after its ribbon geometry checks.
		 * @param preparedSurface Reusable candidate geometry to inspect.
		 * @return True when the candidate contains geometry that can be clipped and submitted.
		 */
		static bool HasDrawableSplineSurface(const PreparedSplineSurface& preparedSurface) noexcept;
		/** Clips/submits a prepared surface and stores its actual positional outcome.
		 * @param surface Borrowed candidate appearance and numerical values.
		 * @param splineIndex Positional contributor index in the scene.
		 * @param surfaceIndex Candidate index, or all-surfaces position, in the input span.
		 * @param context Reusable clipping storage and result owner.
		 * @param destination Synchronous triangle destination.
		 * @param outcome Actual sampled, preparation, selection and submission outcome.
		 */
		static void DrawPreparedSplineSurface(
			const BuildingSplineSurfaceRenderInput& surface,
			std::size_t splineIndex,
			std::size_t surfaceIndex,
			BuildingVisualsContext& context,
			IImageDestination& destination,
			BuildingVisualsContributorOutcome outcome);
		/** Prepares one surface and preserves sampling, rejection and structural status separately.
		 * @param spline Borrowed curve, policy and common root/component transforms.
		 * @param preparedSpline Numerical samples for the same contributor.
		 * @param surface Borrowed bounds, cross-section and optional component-transform override.
		 * @param context Receives reusable prepared surface values.
		 * @param outcome Receives actual preparation outcomes.
		 */
		static void PrepareSplineSurfaceGeometry(
			const BuildingSplineRenderInput& spline,
			const PreparedSpline& preparedSpline,
			const BuildingSplineSurfaceRenderInput& surface,
			BuildingVisualsContext& context,
			BuildingVisualsContributorOutcome& outcome);
		/** Clips prepared geometry and submits visible canonical triangles on successful clipping.
		 * @param surface Borrowed appearance values.
		 * @param context Supplies prepared geometry and receives clipping/conversion scratch.
		 * @param destination Synchronous triangle destination.
		 * @param outcome Receives occlusion and destination results.
		 */
		static void ClipAndSubmitSurface(
			const BuildingSplineSurfaceRenderInput& surface,
			BuildingVisualsContext& context,
			IImageDestination& destination,
			BuildingVisualsContributorOutcome& outcome);
		/** Converts visible XYZ/UV values and submits the selected supplied appearance.
		 * @param surface Borrowed appearance and texture source.
		 * @param context Supplies visible triangles and reusable conversion vectors.
		 * @param destination Synchronous triangle destination.
		 * @param destinationAttempted Reports whether validated input reached the destination call.
		 * @return The local validation or destination submission outcome.
		 */
		static ImageSubmission SubmitVisibleSurface(
			const BuildingSplineSurfaceRenderInput& surface,
			BuildingVisualsContext& context,
			IImageDestination& destination,
			bool& destinationAttempted);
		/** Converts canonical positions for the existing solid-color destination call.
		 * @param surface Borrowed color and opacity values.
		 * @param context Supplies visible geometry and reusable conversion storage.
		 * @param destination Synchronous solid-triangle destination.
		 * @param destinationAttempted Set true immediately before calling the destination.
		 * @return The destination result or local opacity-validation failure.
		 */
		static ImageSubmission SubmitSolidSurface(
			const BuildingSplineSurfaceRenderInput& surface,
			BuildingVisualsContext& context,
			IImageDestination& destination,
			bool& destinationAttempted);
		/** Converts canonical XYZ/UV values for the borrowed-texture destination call.
		 * @param surface Borrowed texture, UV and modulation values.
		 * @param context Supplies visible geometry and reusable conversion storage.
		 * @param destination Synchronous textured-triangle destination.
		 * @param destinationAttempted Set true immediately before calling the destination.
		 * @return The destination result or local opacity-validation failure.
		 */
		static ImageSubmission SubmitTexturedSurface(
			const BuildingSplineSurfaceRenderInput& surface,
			BuildingVisualsContext& context,
			IImageDestination& destination,
			bool& destinationAttempted);
		/** Submits overlay images after every spline surface has been processed.
		 * @param scene Borrowed ordered overlay sprites.
		 * @param destination Synchronous image destination.
		 * @param context Receives positional outcomes.
		 */
		static void DrawOverlaySprites(
			const BuildingRenderSceneInput& scene,
			IImageDestination& destination,
			BuildingVisualsContext& context);
		/** Submits one overlay sprite without adding it to the occlusion index.
		 * @param sprite Borrowed overlay image and placement values.
		 * @param destination Synchronous image destination.
		 * @return Geometry and destination outcomes for this overlay.
		 */
		static BuildingVisualsContributorOutcome DrawOverlaySprite(
			const BuildingSpriteRenderInput& sprite,
			IImageDestination& destination);
		/** Marks the aggregate scene result incomplete when one contributor reports a failure.
		 * @param result Aggregate result owned by the caller context.
		 * @param outcome Positional outcome to incorporate.
		 */
		static void MarkIncompleteOnFailure(
			BuildingVisualsResult& result,
			const BuildingVisualsContributorOutcome& outcome);
		/** Converts normalized straight opacity to destination byte precision.
		 * @param opacity Finite normalized value in the inclusive range [0, 1].
		 * @param output Receives the rounded 8-bit value on success.
		 * @return True when the supplied opacity is in range.
		 */
		static bool TryConvertOpacity(double opacity, std::uint8_t& output) noexcept;
	};
}
