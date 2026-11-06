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
#include <vector>

namespace Services::Map::Buildings::Rendering
{
	/** Draws once into a fresh destination and exposes bytes only after complete drawing and finalization. */
	SectionRendering::Status SectionRendering::RenderSection(
		const Visuals::BuildingRenderSceneInput& scene,
		Visuals::BuildingVisualsContext& context,
		const Rgba16fImageGrid& grid,
		std::vector<std::uint8_t>& output)
	{
		output.clear();
		if (!HasValidGridInputs(grid))
		{
			return Status::InvalidDestinationConfiguration;
		}
		Rgba16fImageDestination destination(grid);
		const Visuals::BuildingVisualsResult& drawingResult =
			Visuals::BuildingVisuals::DrawScene(scene, destination, context);
		if (!drawingResult.Complete)
		{
			return GetDrawingFailureStatus(drawingResult);
		}
		if (!destination.Finalize(output))
		{
			output.clear();
			return Status::FinalizationFailed;
		}
		return Status::Complete;
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
}

#endif // MODLOADER_CLIENT_BUILD
