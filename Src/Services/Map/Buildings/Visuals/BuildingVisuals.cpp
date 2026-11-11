// Implements synchronous and cursor-resumable numerical scene passes over borrowed Building Map inputs.
#if defined(MODLOADER_CLIENT_BUILD)

#include "BuildingVisuals.h"

#include <cmath>
#include <cstdint>
#include <chrono>

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
		SplineIndex = 0;
		SplineSurfaceIndex = 0;
		OverlaySpriteIndex = 0;
		PendingSpriteWorldQuad = {};
		PendingSpriteImageGeometry = {};
		PendingOutcome = {};
		Submission.Reset();
		PendingOpacity = 0;
		Initialized = false;
		SpriteGeometryPrepared = false;
		SplineSamplingPrepared = false;
		SplineSurfacePreparationComplete = false;
		SplineSurfaceClippingComplete = false;
		TriangleConversionComplete = false;
		SurfaceReadyForSubmission = false;
		AlternativeSelected = false;
		DestinationSubmissionActive = false;
		Terminal = false;
	}

	/** Initializes operation reports and reusable context capacity for one scene lifetime. */
	void BuildingVisuals::InitializeScene(
		const BuildingRenderSceneInput& scene,
		BuildingVisualsContext& context,
		SceneCursor& cursor)
	{
		ResetOperation(context);
		context.result_.OrdinarySprites.reserve(scene.OrdinarySprites.size());
		context.occluders_.reserve(scene.OrdinarySprites.size());
		context.preparedSplines_.resize(scene.Splines.size());
		context.result_.OverlayLastSprites.reserve(scene.OverlayLastSprites.size());
		cursor.Initialized = true;
	}

	/** Advances one borrowed scene call while charging only completed deterministic work units. */
	BuildingVisuals::SceneSliceResult BuildingVisuals::StepScene(
		const BuildingRenderSceneInput& scene,
		Rendering::Rgba16fImageDestination& destination,
		BuildingVisualsContext& context,
		SceneCursor& cursor,
		std::chrono::steady_clock::time_point deadline
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		, BuildingVisualsPhaseTimings& phaseTimings
#endif
		)
	{
		SceneSliceResult result;
		if (cursor.Terminal)
		{
			result.Complete = true;
			return result;
		}
		if (std::chrono::steady_clock::now() >= deadline)
		{
			return result;
		}
		if (!cursor.Initialized)
		{
			InitializeScene(scene, context, cursor);
		}
		while (!cursor.Terminal)
		{
			if (std::chrono::steady_clock::now() >= deadline &&
				!CanAdvanceScenePhaseWithoutWork(scene, cursor))
			{
				break;
			}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			const SceneCursor::Phase measuredPhase = cursor.CurrentPhase;
			const auto phaseStarted = std::chrono::steady_clock::now();
#endif
			const SceneAdvanceResult advanceResult = AdvanceSceneStep(
				scene,
				destination,
				context,
				cursor,
				result,
				deadline);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			AccumulateScenePhaseTiming(
				measuredPhase,
				std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - phaseStarted).count(),
				phaseTimings);
#endif
			if (advanceResult == SceneAdvanceResult::YieldWithoutWork)
			{
				break;
			}
		}
		result.Complete = cursor.Terminal;
		return result;
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

	/** Allows only exhausted-phase transitions after the absolute deadline has been consumed. */
	bool BuildingVisuals::CanAdvanceScenePhaseWithoutWork(
		const BuildingRenderSceneInput& scene,
		const SceneCursor& cursor) noexcept
	{
		switch (cursor.CurrentPhase)
		{
		case SceneCursor::Phase::OrdinarySprites:
			return cursor.OrdinarySpriteIndex == scene.OrdinarySprites.size();
		case SceneCursor::Phase::Splines:
			return cursor.SplineIndex == scene.Splines.size();
		case SceneCursor::Phase::OverlayLastSprites:
			return cursor.OverlaySpriteIndex == scene.OverlayLastSprites.size();
		case SceneCursor::Phase::OcclusionIndex:
		case SceneCursor::Phase::Complete:
			return false;
		}
		return false;
	}

	/** Dispatches one work operation or a single zero-cost state transition. */
	BuildingVisuals::SceneAdvanceResult BuildingVisuals::AdvanceSceneStep(
		const BuildingRenderSceneInput& scene,
		Rendering::Rgba16fImageDestination& destination,
		BuildingVisualsContext& context,
		SceneCursor& cursor,
		SceneSliceResult& result,
		std::chrono::steady_clock::time_point deadline)
	{
		switch (cursor.CurrentPhase)
		{
		case SceneCursor::Phase::OrdinarySprites:
			if (cursor.OrdinarySpriteIndex == scene.OrdinarySprites.size())
			{
				cursor.CurrentPhase = SceneCursor::Phase::OcclusionIndex;
				++result.WorkUnitsConsumed;
				return SceneAdvanceResult::Progressed;
			}
			if (std::chrono::steady_clock::now() >= deadline)
			{
				return SceneAdvanceResult::YieldWithoutWork;
			}
			return AdvanceOrdinarySprite(scene, destination, context, cursor, result, deadline);
		case SceneCursor::Phase::OcclusionIndex:
			if (std::chrono::steady_clock::now() >= deadline)
			{
				return SceneAdvanceResult::YieldWithoutWork;
			}
			PrepareSceneOcclusionIndex(context, cursor, result);
			return SceneAdvanceResult::Progressed;
		case SceneCursor::Phase::Splines:
			if (cursor.SplineIndex == scene.Splines.size())
			{
				cursor.CurrentPhase = SceneCursor::Phase::OverlayLastSprites;
				++result.WorkUnitsConsumed;
				return SceneAdvanceResult::Progressed;
			}
			if (std::chrono::steady_clock::now() >= deadline)
			{
				return SceneAdvanceResult::YieldWithoutWork;
			}
			return AdvanceSpline(scene, destination, context, cursor, result, deadline);
		case SceneCursor::Phase::OverlayLastSprites:
			if (cursor.OverlaySpriteIndex == scene.OverlayLastSprites.size())
			{
				cursor.CurrentPhase = SceneCursor::Phase::Complete;
				cursor.Terminal = true;
				++result.WorkUnitsConsumed;
				return SceneAdvanceResult::Progressed;
			}
			if (std::chrono::steady_clock::now() >= deadline)
			{
				return SceneAdvanceResult::YieldWithoutWork;
			}
			return AdvanceOverlaySprite(scene, destination, context, cursor, result, deadline);
		case SceneCursor::Phase::Complete:
			cursor.Terminal = true;
			++result.WorkUnitsConsumed;
			return SceneAdvanceResult::Progressed;
		}
		return SceneAdvanceResult::Progressed;
	}

	/** Prepares one sprite once, resumes its image command and records its terminal result once. */
	BuildingVisuals::SceneAdvanceResult BuildingVisuals::AdvanceOrdinarySprite(
		const BuildingRenderSceneInput& scene,
		Rendering::Rgba16fImageDestination& destination,
		BuildingVisualsContext& context,
		SceneCursor& cursor,
		SceneSliceResult& result,
		std::chrono::steady_clock::time_point deadline)
	{
		const std::size_t index = cursor.OrdinarySpriteIndex;
		const auto& sprite = scene.OrdinarySprites[index];
		if (!cursor.SpriteGeometryPrepared)
		{
			ImageSubmissionCommand command;
			BuildingVisualsContributorOutcome outcome;
			outcome.ContributorIndex = index;
			if (!BuildImageCommand(sprite, command, cursor.PendingSpriteWorldQuad))
			{
				outcome.FailureStage = BuildingVisualsFailureStage::SpriteGeometry;
				MarkIncompleteOnFailure(context.result_, outcome);
				context.result_.OrdinarySprites.push_back(outcome);
				++cursor.OrdinarySpriteIndex;
				ResetSpriteProgress(cursor);
			}
			else
			{
				cursor.PendingSpriteImageGeometry = command.Geometry;
				cursor.PendingOutcome = outcome;
				cursor.PendingOutcome.GeometryPrepared = true;
				cursor.SpriteGeometryPrepared = true;
			}
			++result.WorkUnitsConsumed;
			return SceneAdvanceResult::Progressed;
		}
		return SubmitPreparedSprite(
			sprite,
			index,
			true,
			destination,
			context,
			cursor,
			result,
			deadline);
	}

	/** Reconstructs call-local source pixels and completes one ordinary or overlay sprite. */
	BuildingVisuals::SceneAdvanceResult BuildingVisuals::SubmitPreparedSprite(
		const BuildingSpriteRenderInput& sprite,
		std::size_t index,
		bool ordinary,
		Rendering::Rgba16fImageDestination& destination,
		BuildingVisualsContext& context,
		SceneCursor& cursor,
		SceneSliceResult& result,
		std::chrono::steady_clock::time_point deadline)
	{
		ImageSubmissionCommand command;
		command.Source = {sprite.Pixels, sprite.PixelWidth, sprite.PixelHeight, sprite.PixelRegion};
		command.Geometry = cursor.PendingSpriteImageGeometry;
		cursor.PendingOutcome.SubmissionAttempted = true;
		cursor.DestinationSubmissionActive = true;
		const auto slice = destination.StepSubmitImage(
			command,
			cursor.Submission,
			deadline);
		result.WorkUnitsConsumed += slice.WorkUnitsConsumed;
		if (!slice.CompletedOutcome.has_value())
		{
			return std::chrono::steady_clock::now() >= deadline
				? SceneAdvanceResult::YieldWithoutWork
				: SceneAdvanceResult::Progressed;
		}
		if (slice.WorkUnitsConsumed == 0)
		{
			++result.WorkUnitsConsumed;
		}
		cursor.PendingOutcome.Submission = *slice.CompletedOutcome;
		if (ordinary && cursor.PendingOutcome.Submission == ImageSubmission::Submitted)
		{
			cursor.PendingOutcome.OccluderGeometryPrepared = AddOccluder(
				sprite,
				cursor.PendingSpriteWorldQuad,
				index,
				context);
			if (!cursor.PendingOutcome.OccluderGeometryPrepared)
			{
				cursor.PendingOutcome.FailureStage = BuildingVisualsFailureStage::OccluderGeometry;
			}
		}
		else if (cursor.PendingOutcome.Submission != ImageSubmission::Submitted &&
			cursor.PendingOutcome.Submission != ImageSubmission::Culled)
		{
			cursor.PendingOutcome.FailureStage = BuildingVisualsFailureStage::DestinationSubmission;
		}
		MarkIncompleteOnFailure(context.result_, cursor.PendingOutcome);
		if (ordinary)
		{
			context.result_.OrdinarySprites.push_back(cursor.PendingOutcome);
			++cursor.OrdinarySpriteIndex;
		}
		else
		{
			context.result_.OverlayLastSprites.push_back(cursor.PendingOutcome);
			++cursor.OverlaySpriteIndex;
		}
		ResetSpriteProgress(cursor);
		return SceneAdvanceResult::Progressed;
	}

	/** Prepares the one context-owned occlusion index before any spline phase work. */
	void BuildingVisuals::PrepareSceneOcclusionIndex(
		BuildingVisualsContext& context,
		SceneCursor& cursor,
		SceneSliceResult& result)
	{
		context.result_.OcclusionPreparationStatus = PrepareOcclusionIndex(context);
		if (context.result_.OcclusionPreparationStatus != SplineOcclusionStatus::Complete)
		{
			context.result_.Complete = false;
		}
		cursor.CurrentPhase = SceneCursor::Phase::Splines;
		++result.WorkUnitsConsumed;
	}

	/** Clears one terminal or not-yet-started sprite's retained value state. */
	void BuildingVisuals::ResetSpriteProgress(SceneCursor& cursor) noexcept
	{
		cursor.PendingSpriteWorldQuad = {};
		cursor.PendingSpriteImageGeometry = {};
		cursor.PendingOutcome = {};
		cursor.Submission.Reset();
		cursor.SpriteGeometryPrepared = false;
		cursor.DestinationSubmissionActive = false;
	}

	/** Clears one completed spline surface's progress without touching context-owned prepared geometry. */
	void BuildingVisuals::ResetSplineSurfaceProgress(SceneCursor& cursor) noexcept
	{
		cursor.PendingOutcome = {};
		cursor.Submission.Reset();
		cursor.PendingOpacity = 0;
		cursor.SplineSurfacePreparationComplete = false;
		cursor.SplineSurfaceClippingComplete = false;
		cursor.TriangleConversionComplete = false;
		cursor.SurfaceReadyForSubmission = false;
		cursor.DestinationSubmissionActive = false;
	}

	/** Clears one finished spline's sampling and ordered-alternative progress. */
	void BuildingVisuals::ResetSplineProgress(SceneCursor& cursor) noexcept
	{
		ResetSplineSurfaceProgress(cursor);
		cursor.SplineSamplingPrepared = false;
		cursor.SplineSurfaceIndex = 0;
		cursor.AlternativeSelected = false;
	}

	/** Samples one spline once and advances it through the current surface position. */
	BuildingVisuals::SceneAdvanceResult BuildingVisuals::AdvanceSpline(
		const BuildingRenderSceneInput& scene,
		Rendering::Rgba16fImageDestination& destination,
		BuildingVisualsContext& context,
		SceneCursor& cursor,
		SceneSliceResult& result,
		std::chrono::steady_clock::time_point deadline)
	{
		const auto& spline = scene.Splines[cursor.SplineIndex];
		if (spline.SurfaceSelectionMode == BuildingSplineSurfaceSelectionMode::DrawAllSurfaces &&
			spline.Surfaces.empty())
		{
			++cursor.SplineIndex;
			ResetSplineProgress(cursor);
			++result.WorkUnitsConsumed;
			return SceneAdvanceResult::YieldWithoutWork;
		}
		if (!cursor.SplineSamplingPrepared)
		{
			SplineGeometry::Prepare(
				spline.Segments,
				spline.SamplingPolicy,
				context.preparedSplines_[cursor.SplineIndex]);
			cursor.SplineSamplingPrepared = true;
			++result.WorkUnitsConsumed;
			return SceneAdvanceResult::Progressed;
		}
		return AdvanceSplineSurface(spline, destination, context, cursor, result, deadline);
	}

	/** Advances one selected surface, rejected candidate or no-drawable contributor result. */
	BuildingVisuals::SceneAdvanceResult BuildingVisuals::AdvanceSplineSurface(
		const BuildingSplineRenderInput& spline,
		Rendering::Rgba16fImageDestination& destination,
		BuildingVisualsContext& context,
		SceneCursor& cursor,
		SceneSliceResult& result,
		std::chrono::steady_clock::time_point deadline)
	{
		const bool alternative = spline.SurfaceSelectionMode ==
			BuildingSplineSurfaceSelectionMode::FirstNumericallyDrawableAlternative;
		if (alternative && !cursor.AlternativeSelected &&
			cursor.SplineSurfaceIndex == spline.Surfaces.size())
		{
			CompleteNoDrawableAlternative(
				context.preparedSplines_[cursor.SplineIndex],
				context,
				cursor);
			++result.WorkUnitsConsumed;
			return SceneAdvanceResult::Progressed;
		}
		if (!alternative && cursor.SplineSurfaceIndex == spline.Surfaces.size())
		{
			++cursor.SplineIndex;
			ResetSplineProgress(cursor);
			++result.WorkUnitsConsumed;
			return SceneAdvanceResult::Progressed;
		}
		const auto& surface = spline.Surfaces[cursor.SplineSurfaceIndex];
		if (!cursor.SplineSurfacePreparationComplete)
		{
			const SurfacePreparationResult preparation = PrepareIncrementalSplineSurface(
				spline,
				context.preparedSplines_[cursor.SplineIndex],
				surface,
				context,
				cursor,
				cursor.SplineSurfaceIndex,
				alternative);
			++result.WorkUnitsConsumed;
			if (preparation == SurfacePreparationResult::CandidateRejected)
			{
				context.result_.SplineSurfaces.push_back(cursor.PendingOutcome);
				++cursor.SplineSurfaceIndex;
				ResetSplineSurfaceProgress(cursor);
				return SceneAdvanceResult::Progressed;
			}
			if (preparation == SurfacePreparationResult::ContributorCompleted)
			{
				CompleteSplineSurface(spline, context, cursor);
			}
			return SceneAdvanceResult::Progressed;
		}
		return SubmitPreparedSplineSurface(
			spline,
			surface,
			destination,
			context,
			cursor,
			result,
			deadline);
	}

	/** Prepares and clips one surface, then converts its visible triangles exactly once. */
	BuildingVisuals::SurfacePreparationResult BuildingVisuals::PrepareIncrementalSplineSurface(
		const BuildingSplineRenderInput& spline,
		const PreparedSpline& preparedSpline,
		const BuildingSplineSurfaceRenderInput& surface,
		BuildingVisualsContext& context,
		SceneCursor& cursor,
		std::size_t surfaceIndex,
		bool alternative)
	{
		cursor.PendingOutcome = {};
		cursor.PendingOutcome.ContributorIndex = cursor.SplineIndex;
		cursor.PendingOutcome.SurfaceIndex = surfaceIndex;
		cursor.PendingOutcome.SamplingIssue = preparedSpline.Issue;
		cursor.PendingOutcome.SamplingComplete = preparedSpline.IsComplete();
		cursor.PendingOutcome.AlternativeStatus = alternative
			? BuildingSplineAlternativeStatus::CandidateRejected
			: BuildingSplineAlternativeStatus::NotApplicable;
		if (preparedSpline.Issue != SplineNumericalIssue::None || !preparedSpline.IsComplete())
		{
			cursor.PendingOutcome.FailureStage = BuildingVisualsFailureStage::SplineSampling;
		}
		PrepareSplineSurfaceGeometry(spline, preparedSpline, surface, context, cursor.PendingOutcome);
		if (alternative && !HasDrawableSplineSurface(context.preparedSurface_))
		{
			if (cursor.PendingOutcome.FailureStage == BuildingVisualsFailureStage::None)
			{
				cursor.PendingOutcome.FailureStage = BuildingVisualsFailureStage::SurfacePreparation;
			}
			cursor.SplineSurfacePreparationComplete = true;
			cursor.SplineSurfaceClippingComplete = true;
			return SurfacePreparationResult::CandidateRejected;
		}
		if (alternative)
		{
			cursor.PendingOutcome.AlternativeStatus = BuildingSplineAlternativeStatus::Selected;
			cursor.AlternativeSelected = true;
		}
		if (!cursor.PendingOutcome.GeometryPrepared)
		{
			cursor.SplineSurfacePreparationComplete = true;
			cursor.SplineSurfaceClippingComplete = true;
			return SurfacePreparationResult::ContributorCompleted;
		}
		cursor.PendingOutcome.OcclusionAttempted = true;
		cursor.PendingOutcome.OcclusionStatus = SplineOcclusionGeometry::ClipSurface(
			context.preparedSurface_.Vertices,
			context.preparedSurface_.Indices,
			context.preparedSurface_.Cells,
			context.occlusionIndex_,
			context.occlusionWorkspace_);
		cursor.SplineSurfaceClippingComplete = true;
		if (cursor.PendingOutcome.OcclusionStatus != SplineOcclusionStatus::Complete)
		{
			cursor.PendingOutcome.FailureStage = BuildingVisualsFailureStage::Occlusion;
			cursor.SplineSurfacePreparationComplete = true;
			return SurfacePreparationResult::ContributorCompleted;
		}
		return PrepareIncrementalSurfaceTriangles(surface, context, cursor);
	}

	/** Validates appearance and converts the visible geometry into retained context-owned triangles. */
	BuildingVisuals::SurfacePreparationResult BuildingVisuals::PrepareIncrementalSurfaceTriangles(
		const BuildingSplineSurfaceRenderInput& surface,
		BuildingVisualsContext& context,
		SceneCursor& cursor)
	{
		bool converted = false;
		if (surface.Appearance.Mode == SplineSurfaceAppearanceMode::SolidColor)
		{
			converted = ConvertSolidSurfaceTriangles(surface, context, cursor);
		}
		else if (surface.Appearance.Mode == SplineSurfaceAppearanceMode::Textured)
		{
			converted = ConvertTexturedSurfaceTriangles(surface, context, cursor);
		}
		if (!converted)
		{
			cursor.PendingOutcome.FailureStage = BuildingVisualsFailureStage::AppearanceValidation;
			cursor.SplineSurfacePreparationComplete = true;
			return SurfacePreparationResult::ContributorCompleted;
		}
		cursor.TriangleConversionComplete = true;
		cursor.SurfaceReadyForSubmission = true;
		cursor.SplineSurfacePreparationComplete = true;
		return SurfacePreparationResult::ReadyForSubmission;
	}

	/** Converts canonical visible positions to solid triangles without submitting them. */
	bool BuildingVisuals::ConvertSolidSurfaceTriangles(
		const BuildingSplineSurfaceRenderInput& surface,
		BuildingVisualsContext& context,
		SceneCursor& cursor)
	{
		if (!TryConvertOpacity(surface.Appearance.Opacity, cursor.PendingOpacity))
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
		SceneCursor& cursor)
	{
		if (!TryConvertOpacity(surface.Appearance.Opacity, cursor.PendingOpacity))
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

	/** Resumes one prepared solid or textured batch until the shared deadline or terminal result. */
	BuildingVisuals::SceneAdvanceResult BuildingVisuals::SubmitPreparedSplineSurface(
		const BuildingSplineRenderInput& spline,
		const BuildingSplineSurfaceRenderInput& surface,
		Rendering::Rgba16fImageDestination& destination,
		BuildingVisualsContext& context,
		SceneCursor& cursor,
		SceneSliceResult& result,
		std::chrono::steady_clock::time_point deadline)
	{
		if (!cursor.SurfaceReadyForSubmission || std::chrono::steady_clock::now() >= deadline)
		{
			return SceneAdvanceResult::YieldWithoutWork;
		}
		auto slice = StepPreparedSplineSurface(
			surface,
			destination,
			context,
			cursor,
			deadline);
		result.WorkUnitsConsumed += slice.WorkUnitsConsumed;
		if (!slice.CompletedOutcome.has_value())
		{
			return std::chrono::steady_clock::now() >= deadline
				? SceneAdvanceResult::YieldWithoutWork
				: SceneAdvanceResult::Progressed;
		}
		if (slice.WorkUnitsConsumed == 0)
		{
			++result.WorkUnitsConsumed;
		}
		cursor.PendingOutcome.Submission = *slice.CompletedOutcome;
		if (cursor.PendingOutcome.Submission != ImageSubmission::Submitted &&
			cursor.PendingOutcome.Submission != ImageSubmission::Culled)
		{
			cursor.PendingOutcome.FailureStage = BuildingVisualsFailureStage::SurfaceSubmission;
		}
		CompleteSplineSurface(
			spline,
			context,
			cursor);
		return SceneAdvanceResult::Progressed;
	}

	/** Advances one retained triangle batch using only this call's borrowed appearance view. */
	Rendering::Rgba16fImageDestination::SubmissionSliceResult
	BuildingVisuals::StepPreparedSplineSurface(
		const BuildingSplineSurfaceRenderInput& surface,
		Rendering::Rgba16fImageDestination& destination,
		BuildingVisualsContext& context,
		SceneCursor& cursor,
		std::chrono::steady_clock::time_point deadline)
	{
		cursor.PendingOutcome.SubmissionAttempted = true;
		cursor.DestinationSubmissionActive = true;
		if (surface.Appearance.Mode == SplineSurfaceAppearanceMode::SolidColor)
		{
			const SolidTriangleBatch batch{
				context.solidTriangles_,
				surface.Appearance.SolidRgb,
				cursor.PendingOpacity};
			return destination.StepSubmitSolidTriangles(batch, cursor.Submission, deadline);
		}
		const Rgba8ImageSource texture{
			surface.Appearance.Texture.Pixels,
			surface.Appearance.Texture.Width,
			surface.Appearance.Texture.Height,
			surface.Appearance.Texture.PixelRegion};
		const TexturedTriangleBatch batch{
			context.texturedTriangles_,
			texture,
			surface.Appearance.UvScale,
			surface.Appearance.UvOffset,
			surface.Appearance.AddressU,
			surface.Appearance.AddressV,
			surface.Appearance.ModulationRgb,
			cursor.PendingOpacity};
		return destination.StepSubmitTexturedTriangles(batch, cursor.Submission, deadline);
	}

	/** Records the selected/all-surface result and advances the matching painter-order position. */
	void BuildingVisuals::CompleteSplineSurface(
		const BuildingSplineRenderInput& spline,
		BuildingVisualsContext& context,
		SceneCursor& cursor)
	{
		if (context.occlusionIndex_.Status() != SplineOcclusionStatus::Complete)
		{
			cursor.PendingOutcome.OcclusionStatus = context.occlusionIndex_.Status();
			cursor.PendingOutcome.FailureStage = BuildingVisualsFailureStage::Occlusion;
		}
		MarkIncompleteOnFailure(context.result_, cursor.PendingOutcome);
		context.result_.SplineSurfaces.push_back(cursor.PendingOutcome);
		if (spline.SurfaceSelectionMode ==
			BuildingSplineSurfaceSelectionMode::FirstNumericallyDrawableAlternative)
		{
			++cursor.SplineIndex;
			ResetSplineProgress(cursor);
			return;
		}
		++cursor.SplineSurfaceIndex;
		ResetSplineSurfaceProgress(cursor);
	}

	/** Records the synchronous contributor-level failure after every alternative is rejected. */
	void BuildingVisuals::CompleteNoDrawableAlternative(
		const PreparedSpline& preparedSpline,
		BuildingVisualsContext& context,
		SceneCursor& cursor)
	{
		BuildingVisualsContributorOutcome outcome;
		outcome.ContributorIndex = cursor.SplineIndex;
		outcome.HasSurfaceIndex = false;
		outcome.AlternativeStatus = BuildingSplineAlternativeStatus::NoDrawableAlternative;
		outcome.FailureStage = BuildingVisualsFailureStage::SurfacePreparation;
		outcome.SamplingIssue = preparedSpline.Issue;
		outcome.SamplingComplete = preparedSpline.IsComplete();
		MarkIncompleteOnFailure(context.result_, outcome);
		context.result_.SplineSurfaces.push_back(outcome);
		++cursor.SplineIndex;
		ResetSplineProgress(cursor);
	}

	/** Prepares one overlay once, resumes its image command and records its outcome without occlusion. */
	BuildingVisuals::SceneAdvanceResult BuildingVisuals::AdvanceOverlaySprite(
		const BuildingRenderSceneInput& scene,
		Rendering::Rgba16fImageDestination& destination,
		BuildingVisualsContext& context,
		SceneCursor& cursor,
		SceneSliceResult& result,
		std::chrono::steady_clock::time_point deadline)
	{
		const std::size_t index = cursor.OverlaySpriteIndex;
		const auto& sprite = scene.OverlayLastSprites[index];
		if (!cursor.SpriteGeometryPrepared)
		{
			ImageSubmissionCommand command;
			BuildingVisualsContributorOutcome outcome;
			outcome.ContributorIndex = index;
			if (!BuildImageCommand(sprite, command, cursor.PendingSpriteWorldQuad))
			{
				outcome.FailureStage = BuildingVisualsFailureStage::SpriteGeometry;
				MarkIncompleteOnFailure(context.result_, outcome);
				context.result_.OverlayLastSprites.push_back(outcome);
				++cursor.OverlaySpriteIndex;
				ResetSpriteProgress(cursor);
			}
			else
			{
				cursor.PendingSpriteImageGeometry = command.Geometry;
				cursor.PendingOutcome = outcome;
				cursor.PendingOutcome.GeometryPrepared = true;
				cursor.SpriteGeometryPrepared = true;
			}
			++result.WorkUnitsConsumed;
			return SceneAdvanceResult::Progressed;
		}
		return SubmitPreparedSprite(
			sprite,
			index,
			false,
			destination,
			context,
			cursor,
			result,
			deadline);
	}

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
	/** Adds one invocation's elapsed time to the phase that performed its work. */
	void BuildingVisuals::AccumulateScenePhaseTiming(
		SceneCursor::Phase phase,
		double elapsedMs,
		BuildingVisualsPhaseTimings& phaseTimings) noexcept
	{
		BuildingVisualsPhaseTimings::Phase* timing = nullptr;
		switch (phase)
		{
		case SceneCursor::Phase::OrdinarySprites:
			timing = &phaseTimings.OrdinarySprites;
			break;
		case SceneCursor::Phase::OcclusionIndex:
			timing = &phaseTimings.OcclusionIndexPreparation;
			break;
		case SceneCursor::Phase::Splines:
			timing = &phaseTimings.SplineDrawing;
			break;
		case SceneCursor::Phase::OverlayLastSprites:
			timing = &phaseTimings.OverlaySprites;
			break;
		case SceneCursor::Phase::Complete:
			return;
		}
		timing->Invoked = true;
		timing->ElapsedMs += elapsedMs;
	}
#endif

	const BuildingVisualsResult& BuildingVisuals::DrawScene(
		const BuildingRenderSceneInput& scene,
		IImageDestination& destination,
		BuildingVisualsContext& context
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		, BuildingVisualsPhaseTimings& phaseTimings
#endif
		)
	{
		ResetOperation(context);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const auto ordinarySpritesStarted = std::chrono::steady_clock::now();
#endif
		DrawOrdinarySprites(scene, destination, context);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		phaseTimings.OrdinarySprites.Invoked = true;
		phaseTimings.OrdinarySprites.ElapsedMs = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - ordinarySpritesStarted).count();
		const auto occlusionPreparationStarted = std::chrono::steady_clock::now();
#endif
		context.result_.OcclusionPreparationStatus = PrepareOcclusionIndex(context);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		phaseTimings.OcclusionIndexPreparation.Invoked = true;
		phaseTimings.OcclusionIndexPreparation.ElapsedMs = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - occlusionPreparationStarted).count();
#endif
		if (context.result_.OcclusionPreparationStatus != SplineOcclusionStatus::Complete)
		{
			context.result_.Complete = false;
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const auto splineDrawingStarted = std::chrono::steady_clock::now();
#endif
		DrawSplineSurfaces(scene, destination, context);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		phaseTimings.SplineDrawing.Invoked = true;
		phaseTimings.SplineDrawing.ElapsedMs = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - splineDrawingStarted).count();
		const auto overlaySpritesStarted = std::chrono::steady_clock::now();
#endif
		DrawOverlaySprites(scene, destination, context);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		phaseTimings.OverlaySprites.Invoked = true;
		phaseTimings.OverlaySprites.ElapsedMs = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - overlaySpritesStarted).count();
#endif
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
