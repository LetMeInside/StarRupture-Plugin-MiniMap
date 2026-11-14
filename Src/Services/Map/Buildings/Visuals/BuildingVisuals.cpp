// Submits Section-owned prepared Building Map geometry to an image destination.
#if defined(MODLOADER_CLIENT_BUILD)

#include "BuildingVisuals.h"

#include <cmath>
#include <cstdint>
#include <chrono>
#include <utility>

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
#define RECORD_RASTER_FAILURE(...) cursor.RecordFailure(SectionFailureDetails{__VA_ARGS__})
#else
#define RECORD_RASTER_FAILURE(...) ((void)0)
#endif

namespace Services::Map::Buildings::Visuals
{
	/** Resets a fresh or terminal cursor without treating reset as active-work cancellation. */
	void BuildingVisuals::SceneCursor::Reset() noexcept
	{
		if (Initialized && !Terminal)
		{
			return;
		}
		CurrentPhase = Phase::OrdinarySprites;
		OrdinarySpriteIndex = 0;
		SplineSurfaceIndex = 0;
		OverlaySpriteIndex = 0;
		PendingOutcome = {};
		Submission.Reset();
		Initialized = false;
		DestinationSubmissionActive = false;
		Terminal = false;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		Failure = {};
#endif
	}

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
	/** Keeps one failure for the operation even if further contributors fail. */
	void BuildingVisuals::SceneCursor::RecordFailure(const SectionFailureDetails& failure) noexcept
	{
		if (Failure.Condition == nullptr)
		{
			Failure = failure;
		}
	}
#endif

	/** Resumes raster submission over immutable Section geometry and fresh borrowed image resources. */
	BuildingVisuals::SceneSliceResult BuildingVisuals::StepPreparedScene(
		const PreparedBuildingGeometry& geometry,
		const BuildingRenderSceneInput& resourceScene,
		IImageDestination& destination,
		BuildingVisualsContext& context,
		SceneCursor& cursor,
		std::chrono::steady_clock::time_point deadline)
	{
		SceneSliceResult result;
		if (cursor.Terminal)
		{
			result.Complete = true;
			return result;
		}
		if (!geometry.IsComplete())
		{
			return result;
		}
		if (!cursor.Initialized)
		{
			InitializePreparedSceneSubmission(geometry, resourceScene, context, cursor);
		}
		while (!cursor.Terminal && std::chrono::steady_clock::now() < deadline)
		{
			const auto step = AdvancePreparedSceneStep(
				geometry, resourceScene, destination, context, cursor, result, deadline);
			if (step == SceneAdvanceResult::YieldWithoutWork)
			{
				break;
			}
		}
		result.Complete = cursor.Terminal;
		return result;
	}

	/** Initializes only raster outcomes and pass cursors; geometry remains borrowed and immutable. */
	void BuildingVisuals::InitializePreparedSceneSubmission(
		const PreparedBuildingGeometry& geometry,
		const BuildingRenderSceneInput& resourceScene,
		BuildingVisualsContext& context,
		SceneCursor& cursor)
	{
		ResetOperation(context);
		context.result_.OcclusionPreparationStatus = SplineOcclusionStatus::Complete;
		context.result_.OrdinarySprites.reserve(geometry.OrdinarySprites().size());
		context.result_.SplineSurfaces.reserve(geometry.SplineSurfaces().size());
		context.result_.OverlayLastSprites.reserve(geometry.OverlayLastSprites().size());
		cursor.CurrentPhase = SceneCursor::Phase::OrdinarySprites;
		cursor.Initialized = true;
		if (resourceScene.OrdinarySprites.size() != geometry.OrdinarySprites().size() ||
			resourceScene.OverlayLastSprites.size() != geometry.OverlayLastSprites().size())
		{
			RECORD_RASTER_FAILURE("prepared sprite descriptor correspondence",
				resourceScene.OrdinarySprites.size() != geometry.OrdinarySprites().size()
					? "ordinary occurrence and descriptor counts differ" : "overlay occurrence and descriptor counts differ",
				-1, -1, resourceScene.OrdinarySprites.size() != geometry.OrdinarySprites().size()
					? resourceScene.OrdinarySprites.size() : resourceScene.OverlayLastSprites.size(),
				resourceScene.OrdinarySprites.size() != geometry.OrdinarySprites().size()
					? geometry.OrdinarySprites().size() : geometry.OverlayLastSprites().size());
			context.result_.Complete = false;
		}
	}

	/** Advances ordered image passes without sampling, projecting, clipping or converting geometry. */
	BuildingVisuals::SceneAdvanceResult BuildingVisuals::AdvancePreparedSceneStep(
		const PreparedBuildingGeometry& geometry,
		const BuildingRenderSceneInput& resourceScene,
		IImageDestination& destination,
		BuildingVisualsContext& context,
		SceneCursor& cursor,
		SceneSliceResult& result,
		std::chrono::steady_clock::time_point deadline)
	{
		if (cursor.CurrentPhase == SceneCursor::Phase::OrdinarySprites)
		{
			return AdvancePreparedSpritePass(geometry, resourceScene, destination, context, cursor, result, deadline, false);
		}
		if (cursor.CurrentPhase == SceneCursor::Phase::Splines)
		{
			return AdvancePreparedSplinePass(geometry, resourceScene, destination, context, cursor, result, deadline);
		}
		if (cursor.CurrentPhase == SceneCursor::Phase::OverlayLastSprites)
		{
			return AdvancePreparedSpritePass(geometry, resourceScene, destination, context, cursor, result, deadline, true);
		}
		cursor.Terminal = true;
		return SceneAdvanceResult::Progressed;
	}

	/** Records a mismatched ordinal or canonical key as a terminal source-side failure. */
	BuildingVisuals::SceneAdvanceResult BuildingVisuals::FailPreparedSpriteInput(
		bool overlay,
		std::size_t index,
		BuildingVisualsContext& context,
		SceneCursor& cursor,
		SceneSliceResult& result)
	{
		BuildingVisualsContributorOutcome outcome;
		outcome.ContributorIndex = index;
		outcome.FailureStage = BuildingVisualsFailureStage::SpriteGeometry;
		MarkIncompleteOnFailure(context.result_, outcome);
		(overlay ? context.result_.OverlayLastSprites : context.result_.OrdinarySprites).push_back(outcome);
		(overlay ? cursor.OverlaySpriteIndex : cursor.OrdinarySpriteIndex)++;
		++result.WorkUnitsConsumed;
		return SceneAdvanceResult::Progressed;
	}

	/** Submits one borrowed sprite image over its exact prepared world quad, resuming its destination cursor. */
	BuildingVisuals::SceneAdvanceResult BuildingVisuals::SubmitPreparedSpriteImage(
		const PreparedBuildingGeometry::SpriteOccurrence& occurrence,
		const BuildingSpriteRenderInput& source,
		bool overlay,
		IImageDestination& destination,
		BuildingVisualsContext& context,
		SceneCursor& cursor,
		SceneSliceResult& result,
		std::chrono::steady_clock::time_point deadline)
	{
		ImageSubmissionCommand command;
		command.Source = {source.Pixels, source.PixelWidth, source.PixelHeight, source.PixelRegion};
		for (std::size_t corner = 0; corner < occurrence.SourceWorldQuad.Corners.size(); ++corner)
		{
			command.Geometry.Corners[corner] = {
				occurrence.SourceWorldQuad.Corners[corner][0], occurrence.SourceWorldQuad.Corners[corner][1]};
		}
		if (!cursor.DestinationSubmissionActive)
		{
			cursor.PendingOutcome = {};
			cursor.PendingOutcome.ContributorIndex = occurrence.SceneIndex.value();
			cursor.PendingOutcome.GeometryPrepared = true;
			cursor.PendingOutcome.SubmissionAttempted = true;
			cursor.DestinationSubmissionActive = true;
		}
		const auto slice = destination.StepSubmitImage(command, cursor.Submission, deadline);
		result.WorkUnitsConsumed += slice.WorkUnitsConsumed;
		if (!slice.CompletedOutcome.has_value())
		{
			return std::chrono::steady_clock::now() >= deadline
				? SceneAdvanceResult::YieldWithoutWork
				: SceneAdvanceResult::Progressed;
		}
		cursor.PendingOutcome.Submission = *slice.CompletedOutcome;
		if (!overlay && cursor.PendingOutcome.Submission == ImageSubmission::Submitted && !occurrence.IsOccluder)
		{
			cursor.PendingOutcome.FailureStage = BuildingVisualsFailureStage::OccluderGeometry;
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		if (cursor.PendingOutcome.FailureStage != BuildingVisualsFailureStage::None ||
			(cursor.PendingOutcome.Submission != ImageSubmission::Submitted &&
				cursor.PendingOutcome.Submission != ImageSubmission::Culled))
		{
			RECORD_RASTER_FAILURE(overlay ? "prepared overlay raster submission" : "prepared ordinary raster submission",
				cursor.PendingOutcome.FailureStage == BuildingVisualsFailureStage::OccluderGeometry
					? "submitted ordinary sprite has no prepared occluder" : "destination did not submit or cull sprite",
				static_cast<long long>(occurrence.SectionLocalOrdinal), static_cast<long long>(*occurrence.SceneIndex),
				0, 0, -1, static_cast<int>(cursor.PendingOutcome.Submission));
		}
#endif
		return FinishPreparedSpriteSubmission(overlay, context, cursor, result, slice.WorkUnitsConsumed);
	}

	/** Stores one completed image outcome and advances its ordinary or overlay source cursor. */
	BuildingVisuals::SceneAdvanceResult BuildingVisuals::FinishPreparedSpriteSubmission(
		bool overlay,
		BuildingVisualsContext& context,
		SceneCursor& cursor,
		SceneSliceResult& result,
		std::size_t consumed)
	{
		if (consumed == 0)
		{
			++result.WorkUnitsConsumed;
		}
		if (cursor.PendingOutcome.Submission != ImageSubmission::Submitted &&
			cursor.PendingOutcome.Submission != ImageSubmission::Culled &&
			cursor.PendingOutcome.FailureStage == BuildingVisualsFailureStage::None)
		{
			cursor.PendingOutcome.FailureStage = BuildingVisualsFailureStage::DestinationSubmission;
		}
		MarkIncompleteOnFailure(context.result_, cursor.PendingOutcome);
		(overlay ? context.result_.OverlayLastSprites : context.result_.OrdinarySprites).push_back(cursor.PendingOutcome);
		(overlay ? cursor.OverlaySpriteIndex : cursor.OrdinarySpriteIndex)++;
		ResetSubmissionProgress(cursor);
		return SceneAdvanceResult::Progressed;
	}

	/** Resolves one source surface through its exact sorted-descriptor index and validates occurrence identity. */
	const BuildingSplineSurfaceRenderInput* BuildingVisuals::FindPreparedSurfaceSource(
		const PreparedBuildingGeometry::SplineSurface& surface,
		const BuildingRenderSceneInput& resourceScene,
		SceneCursor& cursor) noexcept
	{
		if (surface.SourceDescriptorIndex >= resourceScene.Splines.size())
		{
			RECORD_RASTER_FAILURE("prepared spline descriptor correspondence", "descriptor index outside spline array",
				static_cast<long long>(surface.SectionLocalOrdinal), static_cast<long long>(surface.SourceDescriptorIndex),
				resourceScene.Splines.size(), surface.SourceDescriptorIndex, static_cast<long long>(surface.SurfaceIndex));
			return nullptr;
		}
		const auto& spline = resourceScene.Splines[surface.SourceDescriptorIndex];
		if (spline.SectionLocalOrdinal != surface.SectionLocalOrdinal || surface.SurfaceIndex >= spline.Surfaces.size())
		{
			RECORD_RASTER_FAILURE("prepared spline descriptor correspondence",
				spline.SectionLocalOrdinal != surface.SectionLocalOrdinal
					? "publication ordinal mismatch" : "surface index outside descriptor surfaces",
				static_cast<long long>(surface.SectionLocalOrdinal), static_cast<long long>(surface.SourceDescriptorIndex),
				spline.SectionLocalOrdinal != surface.SectionLocalOrdinal ? spline.SectionLocalOrdinal : spline.Surfaces.size(),
				spline.SectionLocalOrdinal != surface.SectionLocalOrdinal ? surface.SectionLocalOrdinal : surface.SurfaceIndex,
				static_cast<long long>(surface.SurfaceIndex));
			return nullptr;
		}
		const auto& source = spline.Surfaces[surface.SurfaceIndex];
		const bool textured = source.Appearance.Mode == SplineSurfaceAppearanceMode::Textured;
		if (textured != surface.IsTextured)
		{
			RECORD_RASTER_FAILURE("prepared spline descriptor correspondence", "prepared and descriptor texture modes differ",
				static_cast<long long>(surface.SectionLocalOrdinal), static_cast<long long>(surface.SourceDescriptorIndex),
				static_cast<unsigned>(textured), static_cast<unsigned>(surface.IsTextured),
				static_cast<long long>(surface.SurfaceIndex));
			return nullptr;
		}
		return &source;
	}

	/** Submits the next retained spline surface without resampling or re-clipping its triangles. */
	BuildingVisuals::SceneAdvanceResult BuildingVisuals::AdvancePreparedSplinePass(
		const PreparedBuildingGeometry& geometry,
		const BuildingRenderSceneInput& resourceScene,
		IImageDestination& destination,
		BuildingVisualsContext& context,
		SceneCursor& cursor,
		SceneSliceResult& result,
		std::chrono::steady_clock::time_point deadline)
	{
		const auto& surfaces = geometry.SplineSurfaces();
		if (cursor.SplineSurfaceIndex == surfaces.size())
		{
			cursor.CurrentPhase = SceneCursor::Phase::OverlayLastSprites;
			return SceneAdvanceResult::Progressed;
		}
		const auto& prepared = surfaces[cursor.SplineSurfaceIndex];
		const auto* source = FindPreparedSurfaceSource(prepared, resourceScene, cursor);
		if (source == nullptr)
		{
			return FailPreparedSplineSource(prepared, context, cursor, result);
		}
		return SubmitPreparedSplineTriangles(
			prepared, *source, destination, context, cursor, result, deadline);
	}

	/** Marks missing or incompatible textured source data as a hard image-consumer failure. */
	BuildingVisuals::SceneAdvanceResult BuildingVisuals::FailPreparedSplineSource(
		const PreparedBuildingGeometry::SplineSurface& surface,
		BuildingVisualsContext& context,
		SceneCursor& cursor,
		SceneSliceResult& result)
	{
		BuildingVisualsContributorOutcome outcome;
		outcome.ContributorIndex = surface.SourceDescriptorIndex;
		outcome.SurfaceIndex = surface.SurfaceIndex;
		outcome.FailureStage = BuildingVisualsFailureStage::AppearanceValidation;
		outcome.HasSurfaceIndex = true;
		outcome.GeometryPrepared = true;
		MarkIncompleteOnFailure(context.result_, outcome);
		context.result_.SplineSurfaces.push_back(outcome);
		++cursor.SplineSurfaceIndex;
		++result.WorkUnitsConsumed;
		return SceneAdvanceResult::Progressed;
	}

	/** Borrows owned triangle storage and its source appearance for one resumable destination batch. */
	BuildingVisuals::SceneAdvanceResult BuildingVisuals::SubmitPreparedSplineTriangles(
		const PreparedBuildingGeometry::SplineSurface& prepared,
		const BuildingSplineSurfaceRenderInput& source,
		IImageDestination& destination,
		BuildingVisualsContext& context,
		SceneCursor& cursor,
		SceneSliceResult& result,
		std::chrono::steady_clock::time_point deadline)
	{
		if (!cursor.DestinationSubmissionActive)
		{
			cursor.PendingOutcome = {};
			cursor.PendingOutcome.ContributorIndex = prepared.SourceDescriptorIndex;
			cursor.PendingOutcome.SurfaceIndex = prepared.SurfaceIndex;
			cursor.PendingOutcome.HasSurfaceIndex = true;
			cursor.PendingOutcome.GeometryPrepared = true;
			cursor.PendingOutcome.OcclusionAttempted = true;
			cursor.PendingOutcome.OcclusionStatus = SplineOcclusionStatus::Complete;
			cursor.PendingOutcome.AlternativeStatus = prepared.SelectedAlternative
				? BuildingSplineAlternativeStatus::Selected
				: BuildingSplineAlternativeStatus::NotApplicable;
			cursor.PendingOutcome.SubmissionAttempted = true;
			cursor.DestinationSubmissionActive = true;
		}
		const auto slice = StepPreparedSplineBatch(prepared, source, destination, cursor, deadline);
		result.WorkUnitsConsumed += slice.WorkUnitsConsumed;
		if (!slice.CompletedOutcome.has_value())
		{
			return std::chrono::steady_clock::now() >= deadline
				? SceneAdvanceResult::YieldWithoutWork
				: SceneAdvanceResult::Progressed;
		}
		cursor.PendingOutcome.Submission = *slice.CompletedOutcome;
		if (cursor.PendingOutcome.Submission != ImageSubmission::Submitted &&
			cursor.PendingOutcome.Submission != ImageSubmission::Culled)
		{
			RECORD_RASTER_FAILURE("prepared spline raster submission", "destination did not submit or cull spline surface",
				static_cast<long long>(prepared.SectionLocalOrdinal), static_cast<long long>(prepared.SourceDescriptorIndex),
				0, 0, static_cast<long long>(prepared.SurfaceIndex), static_cast<int>(cursor.PendingOutcome.Submission));
			cursor.PendingOutcome.FailureStage = BuildingVisualsFailureStage::SurfaceSubmission;
		}
		if (slice.WorkUnitsConsumed == 0)
		{
			++result.WorkUnitsConsumed;
		}
		MarkIncompleteOnFailure(context.result_, cursor.PendingOutcome);
		context.result_.SplineSurfaces.push_back(cursor.PendingOutcome);
		++cursor.SplineSurfaceIndex;
		ResetSubmissionProgress(cursor);
		return SceneAdvanceResult::Progressed;
	}

	/** Submits the prepared solid or textured triangle span with the current borrowed source values. */
	IImageDestination::SubmissionSliceResult BuildingVisuals::StepPreparedSplineBatch(
		const PreparedBuildingGeometry::SplineSurface& prepared,
		const BuildingSplineSurfaceRenderInput& source,
		IImageDestination& destination,
		SceneCursor& cursor,
		std::chrono::steady_clock::time_point deadline)
	{
		if (!prepared.IsTextured)
		{
			return destination.StepSubmitSolidTriangles(
				{prepared.SolidTriangles, prepared.SolidRgb, prepared.Opacity}, cursor.Submission, deadline);
		}
		const Rgba8ImageSource texture{
			source.Appearance.Texture.Pixels,
			source.Appearance.Texture.Width,
			source.Appearance.Texture.Height,
			source.Appearance.Texture.PixelRegion};
		return destination.StepSubmitTexturedTriangles(
			{prepared.TexturedTriangles, texture, prepared.UvScale, prepared.UvOffset,
				prepared.AddressU, prepared.AddressV, prepared.ModulationRgb, prepared.Opacity},
			cursor.Submission,
			deadline);
	}

	/** Submits the next painter-ordered sprite occurrence, or advances to the next render pass. */
	BuildingVisuals::SceneAdvanceResult BuildingVisuals::AdvancePreparedSpritePass(
		const PreparedBuildingGeometry& geometry,
		const BuildingRenderSceneInput& resourceScene,
		IImageDestination& destination,
		BuildingVisualsContext& context,
		SceneCursor& cursor,
		SceneSliceResult& result,
		std::chrono::steady_clock::time_point deadline,
		bool overlay)
	{
		auto& sources = overlay ? resourceScene.OverlayLastSprites : resourceScene.OrdinarySprites;
		auto& index = overlay ? cursor.OverlaySpriteIndex : cursor.OrdinarySpriteIndex;
		const auto& painterOrder = geometry.SpritePainterOrder();
		while (index < geometry.SpritePainterCount())
		{
			const std::uint64_t ordinal = painterOrder[index];
			bool occurrenceIsOverlay = false;
			const auto* occurrence = geometry.SpriteOccurrenceAtOrdinal(ordinal, occurrenceIsOverlay);
			if (occurrence == nullptr || occurrenceIsOverlay != overlay)
			{
				++index;
				continue;
			}
			if (!occurrence->SceneIndex.has_value() || occurrence->SceneIndex.value() >= sources.size())
			{
				RECORD_RASTER_FAILURE(overlay ? "prepared overlay descriptor correspondence" : "prepared ordinary descriptor correspondence",
					occurrence->SceneIndex.has_value() ? "SceneIndex outside descriptor array" : "SceneIndex missing",
					static_cast<long long>(occurrence->SectionLocalOrdinal),
					occurrence->SceneIndex.has_value() ? static_cast<long long>(*occurrence->SceneIndex) : -1, sources.size());
				return FailPreparedSpriteInput(
					overlay, static_cast<std::size_t>(occurrence->SectionLocalOrdinal), context, cursor, result);
			}
			const std::size_t sceneIndex = occurrence->SceneIndex.value();
			const auto& source = sources[sceneIndex];
			if (occurrence->SectionLocalOrdinal != source.SectionLocalOrdinal ||
				occurrence->SourceKey.Bytes() != source.CanonicalSpriteKey)
			{
				RECORD_RASTER_FAILURE(overlay ? "prepared overlay descriptor correspondence" : "prepared ordinary descriptor correspondence",
					occurrence->SectionLocalOrdinal != source.SectionLocalOrdinal ? "publication ordinal mismatch" : "canonical sprite key mismatch",
					static_cast<long long>(occurrence->SectionLocalOrdinal), static_cast<long long>(sceneIndex),
					source.SectionLocalOrdinal, occurrence->SectionLocalOrdinal);
				return FailPreparedSpriteInput(overlay, sceneIndex, context, cursor, result);
			}
			if (occurrence->SectionTriangleCount == 0)
			{
				return FinishSectionExcludedSprite(sceneIndex, overlay, context, cursor, result);
			}
			return SubmitPreparedSpriteImage(
				*occurrence, source, overlay, destination, context, cursor, result, deadline);
		}
		cursor.CurrentPhase = overlay ? SceneCursor::Phase::Complete : SceneCursor::Phase::Splines;
		return SceneAdvanceResult::Progressed;
	}

	/** Completes a source-valid sprite whose clipped Section polygon has no positive area. */
	BuildingVisuals::SceneAdvanceResult BuildingVisuals::FinishSectionExcludedSprite(
		std::size_t sceneIndex,
		bool overlay,
		BuildingVisualsContext& context,
		SceneCursor& cursor,
		SceneSliceResult& result)
	{
		cursor.PendingOutcome = {};
		cursor.PendingOutcome.ContributorIndex = sceneIndex;
		cursor.PendingOutcome.GeometryPrepared = true;
		cursor.PendingOutcome.SubmissionAttempted = true;
		cursor.PendingOutcome.Submission = ImageSubmission::Culled;
		return FinishPreparedSpriteSubmission(overlay, context, cursor, result, 0);
	}

	/** Exposes a read-only completed result owned by the caller's unchanged context. */
	const BuildingVisualsResult* BuildingVisuals::TryGetCompletedSceneResult(
		const BuildingVisualsContext& context,
		const SceneCursor& cursor) noexcept
	{
		if (!cursor.Initialized || !cursor.Terminal)
		{
			return nullptr;
		}
		return &context.result_;
	}

	/** Clears the current destination continuation state after one drawable completes. */
	void BuildingVisuals::ResetSubmissionProgress(SceneCursor& cursor) noexcept
	{
		cursor.PendingOutcome = {};
		cursor.Submission.Reset();
		cursor.DestinationSubmissionActive = false;
	}

	/** Converts visible solid triangles into reusable context scratch values. */
	bool BuildingVisuals::ConvertSolidSurfaceTriangles(
		const BuildingSplineSurfaceRenderInput& surface,
		BuildingVisualsContext& context,
		std::uint8_t& opacity)
	{
		if (!TryConvertOpacity(surface.Appearance.Opacity, opacity))
		{
			return false;
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
		return true;
	}

	/** Converts canonical visible positions and UVs to retained textured triangles. */
	bool BuildingVisuals::ConvertTexturedSurfaceTriangles(
		const BuildingSplineSurfaceRenderInput& surface,
		BuildingVisualsContext& context,
		std::uint8_t& opacity)
	{
		if (!TryConvertOpacity(surface.Appearance.Opacity, opacity))
		{
			return false;
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
		return true;
	}

	/** Clears raster outcomes and occluder scratch before one image operation. */
	void BuildingVisuals::ResetOperation(BuildingVisualsContext& context)
	{
		context.result_.Complete = true;
		context.result_.OrdinarySprites.clear();
		context.result_.SplineSurfaces.clear();
		context.result_.OverlayLastSprites.clear();
		context.occluders_.clear();
		context.result_.OcclusionPreparationStatus = SplineOcclusionStatus::NotPrepared;
	}

	/** Adds one valid captured sprite volume to the reusable occlusion scratch. */
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

	/** Reports whether the numerical surface contains accepted indexed triangles. */
	bool BuildingVisuals::HasDrawableSplineSurface(const PreparedSplineSurface& preparedSurface) noexcept
	{
		return preparedSurface.Issue == SplineSurfaceIssue::None && !preparedSurface.Cells.empty() &&
			preparedSurface.Indices.size() >= 3 && preparedSurface.Indices.size() % 3 == 0;
	}

	/** Prepares one surface using its component override or spline transform fallback. */
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

	/** Marks the aggregate raster result incomplete when a drawable failed. */
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

#undef RECORD_RASTER_FAILURE

#endif // MODLOADER_CLIENT_BUILD
