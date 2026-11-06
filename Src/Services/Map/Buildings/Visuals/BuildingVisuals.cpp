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
			if (spline.Surfaces.empty())
			{
				continue;
			}
			auto& preparedSpline = context.preparedSplines_[splineIndex];
			SplineGeometry::Prepare(spline.Segments, spline.SamplingPolicy, preparedSpline);
			for (std::size_t surfaceIndex = 0; surfaceIndex < spline.Surfaces.size(); ++surfaceIndex)
			{
				DrawSplineSurface(spline, preparedSpline, spline.Surfaces[surfaceIndex],
					splineIndex, surfaceIndex, context, destination);
			}
		}
	}

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
		if (preparedSpline.Issue != SplineNumericalIssue::None || !outcome.SamplingComplete)
		{
			outcome.FailureStage = BuildingVisualsFailureStage::SplineSampling;
		}
		PrepareSplineSurfaceGeometry(spline, preparedSpline, surface, context, outcome);
		if (outcome.GeometryPrepared)
		{
			ClipAndSubmitSurface(surface, context, destination, outcome);
		}
		if (context.occlusionIndex_.Status() != SplineOcclusionStatus::Complete)
		{
			outcome.OcclusionStatus = context.occlusionIndex_.Status();
			outcome.FailureStage = BuildingVisualsFailureStage::Occlusion;
		}
		MarkIncompleteOnFailure(context.result_, outcome);
		context.result_.SplineSurfaces.push_back(outcome);
	}

	void BuildingVisuals::PrepareSplineSurfaceGeometry(
		const BuildingSplineRenderInput& spline,
		const PreparedSpline& preparedSpline,
		const BuildingSplineSurfaceRenderInput& surface,
		BuildingVisualsContext& context,
		BuildingVisualsContributorOutcome& outcome)
	{
		SplineSurfaceGeometry::Prepare(spline.Segments, preparedSpline, spline.RootTransform,
			spline.ComponentTransform, surface.Bounds, surface.CrossSection, context.preparedSurface_);
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
