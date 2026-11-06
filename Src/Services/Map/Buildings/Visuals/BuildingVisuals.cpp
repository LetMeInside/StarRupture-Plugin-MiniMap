// Coordinates one stateless synchronous numerical scene pass over borrowed Building Map inputs.
#if defined(MODLOADER_CLIENT_BUILD)

#include "BuildingVisuals.h"

#include <cmath>
#include <cstdint>

namespace Services::Map::Buildings::Visuals
{
	const BuildingVisualsResult& BuildingVisuals::DrawScene(
		const BuildingRenderSceneInput& scene,
		IImageDestination& destination,
		BuildingVisualsContext& context)
	{
		ResetOperation(context);
		DrawOrdinarySprites(scene, destination, context);
		context.result_.OcclusionPreparationStatus = PrepareOcclusionIndex(context);
		if (context.result_.OcclusionPreparationStatus != SplineOcclusionStatus::Complete)
		{
			context.result_.Complete = false;
		}
		DrawSplineSurfaces(scene, destination, context);
		DrawOverlaySprites(scene, destination, context);
		return context.result_;
	}

	void BuildingVisuals::ResetOperation(BuildingVisualsContext& context)
	{
		context.result_.Complete = true;
		context.result_.OrdinarySprites.clear();
		context.result_.SplineSurfaces.clear();
		context.result_.OverlayLastSprites.clear();
		context.occluders_.clear();
		context.result_.OcclusionPreparationStatus = SplineOcclusionStatus::NotPrepared;
	}

	void BuildingVisuals::DrawOrdinarySprites(
		const BuildingRenderSceneInput& scene,
		IImageDestination& destination,
		BuildingVisualsContext& context)
	{
		context.result_.OrdinarySprites.reserve(scene.OrdinarySprites.size());
		context.occluders_.reserve(scene.OrdinarySprites.size());
		for (std::size_t index = 0; index < scene.OrdinarySprites.size(); ++index)
		{
			const auto& sprite = scene.OrdinarySprites[index];
			BuildingVisualsContributorOutcome outcome;
			outcome.ContributorIndex = index;
			ImageSubmissionCommand command;
			SpriteWorldQuad worldQuad;
			outcome.GeometryPrepared = BuildImageCommand(sprite, command, worldQuad);
			if (!outcome.GeometryPrepared)
			{
				outcome.FailureStage = BuildingVisualsFailureStage::SpriteGeometry;
			}
			else
			{
				outcome.SubmissionAttempted = true;
				outcome.Submission = destination.SubmitImage(command);
				if (outcome.Submission == ImageSubmission::Submitted)
				{
					outcome.OccluderGeometryPrepared = AddOccluder(sprite, worldQuad, index, context);
					if (!outcome.OccluderGeometryPrepared)
					{
						outcome.FailureStage = BuildingVisualsFailureStage::OccluderGeometry;
					}
				}
				else if (outcome.Submission != ImageSubmission::Culled)
				{
					outcome.FailureStage = BuildingVisualsFailureStage::DestinationSubmission;
				}
			}
			MarkIncompleteOnFailure(context.result_, outcome);
			context.result_.OrdinarySprites.push_back(outcome);
		}
	}

	bool BuildingVisuals::BuildImageCommand(
		const BuildingSpriteRenderInput& sprite,
		ImageSubmissionCommand& command,
		SpriteWorldQuad& worldQuad)
	{
		const SpriteProjectionRectangle projection{
			sprite.Projection.Left,
			sprite.Projection.Top,
			sprite.Projection.Right,
			sprite.Projection.Bottom};
		if (!SpriteGeometry::BuildWorldQuad(sprite.Placement, sprite.ProjectionSourceSize,
			sprite.CaptureBounds, projection, worldQuad))
		{
			return false;
		}
		command.Source = {sprite.Pixels, sprite.PixelWidth, sprite.PixelHeight, sprite.PixelRegion};
		for (std::size_t corner = 0; corner < worldQuad.Corners.size(); ++corner)
		{
			command.Geometry.Corners[corner] = {worldQuad.Corners[corner][0], worldQuad.Corners[corner][1]};
		}
		return true;
	}

	bool BuildingVisuals::AddOccluder(
		const BuildingSpriteRenderInput& sprite,
		const SpriteWorldQuad& worldQuad,
		std::size_t ordinal,
		BuildingVisualsContext& context)
	{
		SplineOcclusionSpriteQuad occluder;
		if (!SpriteGeometry::TryGetCaptureBoundsTopHeight(sprite.Placement, sprite.CaptureBounds,
			occluder.TopHeight))
		{
			return false;
		}
		for (std::size_t corner = 0; corner < worldQuad.Corners.size(); ++corner)
		{
			occluder.Corners[corner] = {worldQuad.Corners[corner][0], worldQuad.Corners[corner][1]};
		}
		occluder.Key = static_cast<std::uint64_t>(ordinal);
		context.occluders_.push_back(occluder);
		return true;
	}

	SplineOcclusionStatus BuildingVisuals::PrepareOcclusionIndex(BuildingVisualsContext& context)
	{
		return SplineOcclusionGeometry::PrepareOccluders(context.occluders_, context.occlusionIndex_);
	}

	/** Samples each spline once, then follows its selected surface policy synchronously.
	 * @param scene Borrowed spline records and ordered surface spans.
	 * @param destination Destination used after numerical surface preparation.
	 * @param context Caller-owned reusable samples, surface geometry and outcomes.
	 */
	void BuildingVisuals::DrawSplineSurfaces(
		const BuildingRenderSceneInput& scene,
		IImageDestination& destination,
		BuildingVisualsContext& context)
	{
		context.preparedSplines_.resize(scene.Splines.size());
		std::size_t surfaceCount = 0;
		for (const auto& spline : scene.Splines)
		{
			surfaceCount += spline.Surfaces.size();
		}
		context.result_.SplineSurfaces.reserve(surfaceCount);
		for (std::size_t splineIndex = 0; splineIndex < scene.Splines.size(); ++splineIndex)
		{
			const auto& spline = scene.Splines[splineIndex];
			if (spline.Surfaces.empty() &&
				spline.SurfaceSelectionMode == BuildingSplineSurfaceSelectionMode::DrawAllSurfaces)
			{
				continue;
			}
			auto& preparedSpline = context.preparedSplines_[splineIndex];
			SplineGeometry::Prepare(spline.Segments, spline.SamplingPolicy, preparedSpline);
			if (spline.SurfaceSelectionMode ==
				BuildingSplineSurfaceSelectionMode::FirstNumericallyDrawableAlternative)
			{
				DrawFirstDrawableSplineAlternative(spline, preparedSpline, splineIndex, context, destination);
				continue;
			}

			for (std::size_t surfaceIndex = 0; surfaceIndex < spline.Surfaces.size(); ++surfaceIndex)
			{
				DrawSplineSurface(spline, preparedSpline, spline.Surfaces[surfaceIndex],
					splineIndex, surfaceIndex, context, destination);
			}
		}
	}

	/** Prepares one all-surfaces input and records its positional numerical/submission outcome.
	 * @param spline Borrowed common source values and surface-selection policy.
	 * @param preparedSpline Shared samples already prepared for this contributor.
	 * @param surface Borrowed surface values and optional component-transform override.
	 * @param splineIndex Positional contributor index in the input scene.
	 * @param surfaceIndex Position in the supplied surface span.
	 * @param context Reusable preparation, clipping and result storage.
	 * @param destination Synchronous triangle destination.
	 */
	void BuildingVisuals::DrawSplineSurface(
		const BuildingSplineRenderInput& spline,
		const PreparedSpline& preparedSpline,
		const BuildingSplineSurfaceRenderInput& surface,
		std::size_t splineIndex,
		std::size_t surfaceIndex,
		BuildingVisualsContext& context,
		IImageDestination& destination)
	{
		BuildingVisualsContributorOutcome outcome;
		outcome.ContributorIndex = splineIndex;
		outcome.SurfaceIndex = surfaceIndex;
		outcome.SamplingIssue = preparedSpline.Issue;
		outcome.SamplingComplete = preparedSpline.IsComplete();
		outcome.AlternativeStatus = BuildingSplineAlternativeStatus::NotApplicable;
		if (preparedSpline.Issue != SplineNumericalIssue::None || !outcome.SamplingComplete)
		{
			outcome.FailureStage = BuildingVisualsFailureStage::SplineSampling;
		}
		PrepareSplineSurfaceGeometry(spline, preparedSpline, surface, context, outcome);
		DrawPreparedSplineSurface(surface, splineIndex, surfaceIndex, context, destination, outcome);
	}

	/** Tries ordered alternatives until preparation yields accepted indexed surface triangles.
	 * Earlier candidate failures are recorded but do not affect aggregate completeness after selection.
	 * @param spline Borrowed common curve, policy, transforms and ordered candidate span.
	 * @param preparedSpline One centerline sampling result shared by all candidate preparations.
	 * @param splineIndex Positional contributor index in the scene.
	 * @param context Reusable candidate geometry and positional result storage.
	 * @param destination Synchronous triangle destination used only after a candidate is selected.
	 */
	void BuildingVisuals::DrawFirstDrawableSplineAlternative(
		const BuildingSplineRenderInput& spline,
		const PreparedSpline& preparedSpline,
		std::size_t splineIndex,
		BuildingVisualsContext& context,
		IImageDestination& destination)
	{
		for (std::size_t surfaceIndex = 0; surfaceIndex < spline.Surfaces.size(); ++surfaceIndex)
		{
			BuildingVisualsContributorOutcome outcome;
			outcome.ContributorIndex = splineIndex;
			outcome.SurfaceIndex = surfaceIndex;
			outcome.AlternativeStatus = BuildingSplineAlternativeStatus::CandidateRejected;
			outcome.SamplingIssue = preparedSpline.Issue;
			outcome.SamplingComplete = preparedSpline.IsComplete();
			if (preparedSpline.Issue != SplineNumericalIssue::None || !outcome.SamplingComplete)
			{
				outcome.FailureStage = BuildingVisualsFailureStage::SplineSampling;
			}

			const auto& surface = spline.Surfaces[surfaceIndex];
			PrepareSplineSurfaceGeometry(spline, preparedSpline, surface, context, outcome);
			if (!HasDrawableSplineSurface(context.preparedSurface_))
			{
				if (outcome.FailureStage == BuildingVisualsFailureStage::None)
				{
					outcome.FailureStage = BuildingVisualsFailureStage::SurfacePreparation;
				}
				context.result_.SplineSurfaces.push_back(outcome);
				continue;
			}

			outcome.AlternativeStatus = BuildingSplineAlternativeStatus::Selected;
			DrawPreparedSplineSurface(surface, splineIndex, surfaceIndex, context, destination, outcome);
			return;
		}

		BuildingVisualsContributorOutcome noDrawableOutcome;
		noDrawableOutcome.ContributorIndex = splineIndex;
		noDrawableOutcome.HasSurfaceIndex = false;
		noDrawableOutcome.AlternativeStatus = BuildingSplineAlternativeStatus::NoDrawableAlternative;
		noDrawableOutcome.FailureStage = BuildingVisualsFailureStage::SurfacePreparation;
		noDrawableOutcome.SamplingIssue = preparedSpline.Issue;
		noDrawableOutcome.SamplingComplete = preparedSpline.IsComplete();
		MarkIncompleteOnFailure(context.result_, noDrawableOutcome);
		context.result_.SplineSurfaces.push_back(noDrawableOutcome);
	}

	/** Recognizes a cell already accepted and indexed by SplineSurfaceGeometry.
	 * @param preparedSurface Reusable numerical candidate result.
	 * @return Whether an accepted cell contributes at least one indexed triangle batch.
	 */
	bool BuildingVisuals::HasDrawableSplineSurface(const PreparedSplineSurface& preparedSurface) noexcept
	{
		return preparedSurface.Issue == SplineSurfaceIssue::None && !preparedSurface.Cells.empty() &&
			preparedSurface.Indices.size() >= 3 && preparedSurface.Indices.size() % 3 == 0;
	}

	/** Clips and submits prepared geometry, then stores the selected or ordinary surface outcome.
	 * @param surface Borrowed surface appearance and optional candidate transform.
	 * @param splineIndex Positional contributor index in the scene.
	 * @param surfaceIndex Input span position for this surface.
	 * @param context Caller-owned clipping storage and positional outcomes.
	 * @param destination Synchronous triangle destination.
	 * @param outcome Numerical and selection data to complete with actual clipping/submission results.
	 */
	void BuildingVisuals::DrawPreparedSplineSurface(
		const BuildingSplineSurfaceRenderInput& surface,
		std::size_t splineIndex,
		std::size_t surfaceIndex,
		BuildingVisualsContext& context,
		IImageDestination& destination,
		BuildingVisualsContributorOutcome outcome)
	{
		if (outcome.GeometryPrepared)
		{
			ClipAndSubmitSurface(surface, context, destination, outcome);
		}
		if (context.occlusionIndex_.Status() != SplineOcclusionStatus::Complete)
		{
			outcome.OcclusionStatus = context.occlusionIndex_.Status();
			outcome.FailureStage = BuildingVisualsFailureStage::Occlusion;
		}
		outcome.ContributorIndex = splineIndex;
		outcome.SurfaceIndex = surfaceIndex;
		MarkIncompleteOnFailure(context.result_, outcome);
		context.result_.SplineSurfaces.push_back(outcome);
	}

	/** Prepares one surface using its explicit component override or the spline-wide fallback.
	 * The reusable context owns output geometry only until the next candidate preparation.
	 * @param spline Borrowed common curve, root and fallback component transforms.
	 * @param preparedSpline Shared sampled centerline for this spline contributor.
	 * @param surface Borrowed profile dimensions and optional component transform.
	 * @param context Receives prepared geometry in reusable caller-owned storage.
	 * @param outcome Receives sampling-independent surface preparation details.
	 */
	void BuildingVisuals::PrepareSplineSurfaceGeometry(
		const BuildingSplineRenderInput& spline,
		const PreparedSpline& preparedSpline,
		const BuildingSplineSurfaceRenderInput& surface,
		BuildingVisualsContext& context,
		BuildingVisualsContributorOutcome& outcome)
	{
		const auto& componentTransform = surface.ComponentTransformOverride.has_value()
			? *surface.ComponentTransformOverride
			: spline.ComponentTransform;
		SplineSurfaceGeometry::Prepare(spline.Segments, preparedSpline, spline.RootTransform,
			componentTransform, surface.Bounds, surface.CrossSection, context.preparedSurface_);
		outcome.SurfaceIssue = context.preparedSurface_.Issue;
		outcome.RejectedCells = context.preparedSurface_.RejectedCells;
		outcome.GeometryPrepared = context.preparedSurface_.Issue == SplineSurfaceIssue::None;
		if (!outcome.GeometryPrepared)
		{
			outcome.FailureStage = BuildingVisualsFailureStage::SurfacePreparation;
		}
		else if (outcome.RejectedCells != 0 && outcome.FailureStage == BuildingVisualsFailureStage::None)
		{
			outcome.FailureStage = BuildingVisualsFailureStage::SurfacePreparation;
		}
	}

	void BuildingVisuals::ClipAndSubmitSurface(
		const BuildingSplineSurfaceRenderInput& surface,
		BuildingVisualsContext& context,
		IImageDestination& destination,
		BuildingVisualsContributorOutcome& outcome)
	{
		outcome.OcclusionAttempted = true;
		outcome.OcclusionStatus = SplineOcclusionGeometry::ClipSurface(
			context.preparedSurface_.Vertices, context.preparedSurface_.Indices,
			context.preparedSurface_.Cells, context.occlusionIndex_, context.occlusionWorkspace_);
		if (outcome.OcclusionStatus != SplineOcclusionStatus::Complete)
		{
			outcome.FailureStage = BuildingVisualsFailureStage::Occlusion;
			return;
		}
		outcome.Submission = SubmitVisibleSurface(surface, context, destination, outcome.SubmissionAttempted);
		if (!outcome.SubmissionAttempted)
		{
			outcome.FailureStage = BuildingVisualsFailureStage::AppearanceValidation;
		}
		else if (outcome.Submission != ImageSubmission::Submitted && outcome.Submission != ImageSubmission::Culled)
		{
			outcome.FailureStage = BuildingVisualsFailureStage::SurfaceSubmission;
		}
	}

	ImageSubmission BuildingVisuals::SubmitVisibleSurface(
		const BuildingSplineSurfaceRenderInput& surface,
		BuildingVisualsContext& context,
		IImageDestination& destination,
		bool& destinationAttempted)
	{
		destinationAttempted = false;
		if (surface.Appearance.Mode == SplineSurfaceAppearanceMode::SolidColor)
		{
			return SubmitSolidSurface(surface, context, destination, destinationAttempted);
		}
		if (surface.Appearance.Mode == SplineSurfaceAppearanceMode::Textured)
		{
			return SubmitTexturedSurface(surface, context, destination, destinationAttempted);
		}
		return ImageSubmission::InvalidInput;
	}

	ImageSubmission BuildingVisuals::SubmitSolidSurface(
		const BuildingSplineSurfaceRenderInput& surface,
		BuildingVisualsContext& context,
		IImageDestination& destination,
		bool& destinationAttempted)
	{
		std::uint8_t opacity = 0;
		if (!TryConvertOpacity(surface.Appearance.Opacity, opacity))
		{
			return ImageSubmission::InvalidInput;
		}
		const auto& visible = context.occlusionWorkspace_.VisibleTriangles();
		context.solidTriangles_.clear();
		context.solidTriangles_.reserve(visible.size());
		for (const auto& triangle : visible)
		{
			SolidTriangle converted;
			for (std::size_t vertex = 0; vertex < converted.Vertices.size(); ++vertex)
			{
				converted.Vertices[vertex] = triangle.Vertices[vertex].Position;
			}
			context.solidTriangles_.push_back(converted);
		}
		const SolidTriangleBatch batch{context.solidTriangles_, surface.Appearance.SolidRgb, opacity};
		destinationAttempted = true;
		return destination.SubmitSolidTriangles(batch);
	}

	ImageSubmission BuildingVisuals::SubmitTexturedSurface(
		const BuildingSplineSurfaceRenderInput& surface,
		BuildingVisualsContext& context,
		IImageDestination& destination,
		bool& destinationAttempted)
	{
		std::uint8_t opacity = 0;
		if (!TryConvertOpacity(surface.Appearance.Opacity, opacity))
		{
			return ImageSubmission::InvalidInput;
		}
		const auto& visible = context.occlusionWorkspace_.VisibleTriangles();
		context.texturedTriangles_.clear();
		context.texturedTriangles_.reserve(visible.size());
		for (const auto& triangle : visible)
		{
			TexturedTriangle converted;
			for (std::size_t vertex = 0; vertex < converted.Vertices.size(); ++vertex)
			{
				converted.Vertices[vertex] = {
					triangle.Vertices[vertex].Position,
					triangle.Vertices[vertex].TextureCoordinates};
			}
			context.texturedTriangles_.push_back(converted);
		}
		const Rgba8ImageSource texture{
			surface.Appearance.Texture.Pixels,
			surface.Appearance.Texture.Width,
			surface.Appearance.Texture.Height,
			surface.Appearance.Texture.PixelRegion};
		const TexturedTriangleBatch batch{
			context.texturedTriangles_, texture, surface.Appearance.UvScale, surface.Appearance.UvOffset,
			surface.Appearance.AddressU, surface.Appearance.AddressV,
			surface.Appearance.ModulationRgb, opacity};
		destinationAttempted = true;
		return destination.SubmitTexturedTriangles(batch);
	}

	void BuildingVisuals::DrawOverlaySprites(
		const BuildingRenderSceneInput& scene,
		IImageDestination& destination,
		BuildingVisualsContext& context)
	{
		context.result_.OverlayLastSprites.reserve(scene.OverlayLastSprites.size());
		for (std::size_t index = 0; index < scene.OverlayLastSprites.size(); ++index)
		{
			auto outcome = DrawOverlaySprite(scene.OverlayLastSprites[index], destination);
			outcome.ContributorIndex = index;
			MarkIncompleteOnFailure(context.result_, outcome);
			context.result_.OverlayLastSprites.push_back(outcome);
		}
	}

	BuildingVisualsContributorOutcome BuildingVisuals::DrawOverlaySprite(
		const BuildingSpriteRenderInput& sprite,
		IImageDestination& destination)
	{
		BuildingVisualsContributorOutcome outcome;
		ImageSubmissionCommand command;
		SpriteWorldQuad worldQuad;
		outcome.GeometryPrepared = BuildImageCommand(sprite, command, worldQuad);
		if (!outcome.GeometryPrepared)
		{
			outcome.FailureStage = BuildingVisualsFailureStage::SpriteGeometry;
			return outcome;
		}
		outcome.SubmissionAttempted = true;
		outcome.Submission = destination.SubmitImage(command);
		if (outcome.Submission != ImageSubmission::Submitted && outcome.Submission != ImageSubmission::Culled)
		{
			outcome.FailureStage = BuildingVisualsFailureStage::DestinationSubmission;
		}
		return outcome;
	}

	void BuildingVisuals::MarkIncompleteOnFailure(
		BuildingVisualsResult& result,
		const BuildingVisualsContributorOutcome& outcome)
	{
		if (outcome.FailureStage != BuildingVisualsFailureStage::None)
		{
			result.Complete = false;
		}
	}

	bool BuildingVisuals::TryConvertOpacity(double opacity, std::uint8_t& output) noexcept
	{
		if (!std::isfinite(opacity) || opacity < 0.0 || opacity > 1.0)
		{
			return false;
		}
		output = static_cast<std::uint8_t>(std::lround(opacity * 255.0));
		return true;
	}
}

#endif // MODLOADER_CLIENT_BUILD
