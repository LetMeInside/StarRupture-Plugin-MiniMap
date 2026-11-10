#if defined(MODLOADER_CLIENT_BUILD)

// Implements synchronous construction of one complete numerical Section image.
#include "SectionRendering.h"

#include "Rgba16fImageDestination.h"
#include "../Visuals/BuildingVisuals.h"
#include "../Visuals/BuildingRenderSceneInput.h"
#include "../Visuals/BuildingVisualsContext.h"
#include "../Visuals/BuildingVisualsResult.h"

#include <cmath>
#include <cstdint>
#include <climits>
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
#include <chrono>
#endif
#include <vector>

namespace Services::Map::Buildings::Rendering
{
	/** Draws once into a fresh destination and exposes bytes only after complete drawing and finalization. */
	SectionRendering::Status SectionRendering::RenderSection(
		const Visuals::BuildingRenderSceneInput& scene,
		Visuals::BuildingVisualsContext& context,
		const Rgba16fImageGrid& grid,
		std::vector<std::uint8_t>& output,
		Visuals::BuildingVisualsTerminalOmissionSummary& omissionSummary
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		, Visuals::BuildingVisualsPhaseTimings& phaseTimings
#endif
		)
	{
		output.clear();
		omissionSummary = {};
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		phaseTimings = {};
#endif
		if (!HasValidGridInputs(grid))
		{
			return Status::InvalidDestinationConfiguration;
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const auto destinationConstructionStarted = std::chrono::steady_clock::now();
#endif
		Rgba16fImageDestination destination(grid);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		phaseTimings.DestinationConstruction.Invoked = true;
		phaseTimings.DestinationConstruction.ElapsedMs = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - destinationConstructionStarted).count();
#endif
		const Visuals::BuildingVisualsResult& drawingResult =
			Visuals::BuildingVisuals::DrawScene(scene, destination, context
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
				, phaseTimings
#endif
				);
		if (!drawingResult.Complete)
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			phaseTimings.CaptureFailureEvidence(drawingResult);
#endif
		}
		const bool completedWithOmissions = !drawingResult.Complete &&
			TrySummarizeTerminalSplineOmissions(drawingResult, scene, omissionSummary);
		if (!drawingResult.Complete && !completedWithOmissions)
		{
			return GetDrawingFailureStatus(drawingResult);
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const auto finalizationStarted = std::chrono::steady_clock::now();
#endif
		const bool finalized = destination.Finalize(output);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		phaseTimings.DestinationFinalization.Invoked = true;
		phaseTimings.DestinationFinalization.ElapsedMs = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - finalizationStarted).count();
#endif
		if (!finalized)
		{
			output.clear();
			omissionSummary = {};
			return Status::FinalizationFailed;
		}
		return completedWithOmissions ? Status::CompleteWithOmissions : Status::Complete;
	}

	/** Rejects malformed dimensions, non-finite origins and invalid pixel scale at the caller boundary. */
	bool SectionRendering::HasValidGridInputs(const Rgba16fImageGrid& grid) noexcept
	{
		return grid.Width != 0 && grid.Height != 0 && grid.Width <= INT_MAX && grid.Height <= INT_MAX &&
			std::isfinite(grid.WorldOriginX) && std::isfinite(grid.WorldOriginY) &&
			std::isfinite(grid.WorldUnitsPerPixel) && grid.WorldUnitsPerPixel > 0.0;
	}

	/** Searches positional outcomes for one concrete destination submission result. */
	bool SectionRendering::HasSubmissionOutcome(
		const std::vector<Visuals::BuildingVisualsContributorOutcome>& outcomes,
		ImageSubmission submission) noexcept
	{
		for (const auto& outcome : outcomes)
		{
			if (outcome.SubmissionAttempted && outcome.Submission == submission)
			{
				return true;
			}
		}
		return false;
	}

	/** Preserves destination unavailability and raster rejection separately from other numerical failures. */
	SectionRendering::Status SectionRendering::GetDrawingFailureStatus(
		const Visuals::BuildingVisualsResult& result) noexcept
	{
		const bool destinationUnavailable =
			HasSubmissionOutcome(result.OrdinarySprites, ImageSubmission::Unavailable) ||
			HasSubmissionOutcome(result.SplineSurfaces, ImageSubmission::Unavailable) ||
			HasSubmissionOutcome(result.OverlayLastSprites, ImageSubmission::Unavailable);
		if (destinationUnavailable)
		{
			return Status::DestinationUnavailable;
		}
		const bool destinationRejected =
			HasSubmissionOutcome(result.OrdinarySprites, ImageSubmission::Rejected) ||
			HasSubmissionOutcome(result.SplineSurfaces, ImageSubmission::Rejected) ||
			HasSubmissionOutcome(result.OverlayLastSprites, ImageSubmission::Rejected);
		if (destinationRejected)
		{
			return Status::DestinationRejected;
		}
		return Status::IncompleteDrawing;
	}

	/** Accepts only completed destination work with one or more no-drawable spline outcomes. */
	bool SectionRendering::TrySummarizeTerminalSplineOmissions(
		const Visuals::BuildingVisualsResult& result,
		const Visuals::BuildingRenderSceneInput& scene,
		Visuals::BuildingVisualsTerminalOmissionSummary& summary) noexcept
	{
		using Alternative = Visuals::BuildingSplineAlternativeStatus;
		using Failure = Visuals::BuildingVisualsFailureStage;
		Visuals::BuildingVisualsTerminalOmissionSummary candidateSummary;
		if (result.OcclusionPreparationStatus != Visuals::SplineOcclusionStatus::Complete ||
			!HasNoContributorFailures(result.OrdinarySprites) ||
			!HasNoContributorFailures(result.OverlayLastSprites))
		{
			return false;
		}
		for (const auto& outcome : result.SplineSurfaces)
		{
			if (outcome.AlternativeStatus == Alternative::CandidateRejected)
			{
				if (outcome.SubmissionAttempted)
				{
					return false;
				}
				continue;
			}
			if (outcome.FailureStage == Failure::None)
			{
				continue;
			}
			if (outcome.AlternativeStatus != Alternative::NoDrawableAlternative ||
				outcome.FailureStage != Failure::SurfacePreparation || outcome.HasSurfaceIndex ||
				outcome.SubmissionAttempted || outcome.ContributorIndex >= scene.Splines.size())
			{
				return false;
			}
			if (candidateSummary.SplineContributorCount == 0)
			{
				candidateSummary.FirstSectionLocalOrdinal =
					scene.Splines[outcome.ContributorIndex].SectionLocalOrdinal;
				candidateSummary.FailureStage = outcome.FailureStage;
				candidateSummary.AlternativeStatus = outcome.AlternativeStatus;
			}
			++candidateSummary.SplineContributorCount;
		}
		if (candidateSummary.SplineContributorCount == 0)
		{
			return false;
		}
		summary = candidateSummary;
		return true;
	}

	/** Checks one positional contributor category for any fatal drawing result. */
	bool SectionRendering::HasNoContributorFailures(
		const std::vector<Visuals::BuildingVisualsContributorOutcome>& outcomes) noexcept
	{
		for (const auto& outcome : outcomes)
		{
			if (outcome.FailureStage != Visuals::BuildingVisualsFailureStage::None)
			{
				return false;
			}
		}
		return true;
	}
}

#endif // MODLOADER_CLIENT_BUILD
