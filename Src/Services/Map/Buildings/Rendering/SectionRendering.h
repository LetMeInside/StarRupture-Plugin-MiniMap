// Builds one complete numerical Section image from already prepared neutral scene inputs.
#pragma once

#include <cstdint>
#include <vector>

namespace Services::Map::Buildings
{
	enum class ImageSubmission : std::uint8_t;
}

namespace Services::Map::Buildings::Visuals
{
	struct BuildingRenderSceneInput;
	struct BuildingVisualsContributorOutcome;
	class BuildingVisualsContext;
	class BuildingVisualsResult;
}

namespace Services::Map::Buildings::Rendering
{
	struct Rgba16fImageGrid;

	/** Reports the outcome of one synchronous numerical Section-image construction. */
	class SectionRendering final
	{
	public:
		/** Describes whether drawing, destination work, or finalization prevented a complete output. */
		enum class Status
		{
			Complete,
			InvalidDestinationConfiguration,
			IncompleteDrawing,
			DestinationUnavailable,
			DestinationRejected,
			FinalizationFailed
		};

		/**
		 * Draws prepared neutral contributors once and finalizes one complete RGBA16F image.
		 * The scene, grid and Section-owned context are borrowed for this synchronous call.
		 * Output is cleared before work begins and contains bytes only when Status::Complete is returned.
		 *
		 * @param scene Prepared neutral contributors and borrowed source spans.
		 * @param context Numerical workspace borrowed from the owning BuildingMapSection.
		 * @param grid Destination cell and pixel configuration supplied by the coordinator.
		 * @param output Caller-owned final row-major little-endian RGBA16F bytes.
		 * @return The numerical rendering, destination or finalization outcome.
		 */
		static Status RenderSection(
			const Visuals::BuildingRenderSceneInput& scene,
			Visuals::BuildingVisualsContext& context,
			const Rgba16fImageGrid& grid,
			std::vector<std::uint8_t>& output);

	private:
		/** Checks destination grid fields that can be rejected without recomputing world bounds. */
		static bool HasValidGridInputs(const Rgba16fImageGrid& grid) noexcept;

		/** Finds a destination submission outcome in one positional result vector. */
		static bool HasSubmissionOutcome(
			const std::vector<Visuals::BuildingVisualsContributorOutcome>& outcomes,
			ImageSubmission submission) noexcept;

		/** Identifies destination availability or raster rejection from retained drawing outcomes. */
		static Status GetDrawingFailureStatus(const Visuals::BuildingVisualsResult& result) noexcept;
	};
}
