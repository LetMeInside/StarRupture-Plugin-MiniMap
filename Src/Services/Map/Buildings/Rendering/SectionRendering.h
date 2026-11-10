	// Builds synchronous images or advances one persistent numerical Section operation over neutral scene inputs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
#include "../Visuals/BuildingVisualsResult.h"

namespace Services::Map::Buildings
{
	enum class ImageSubmission : std::uint8_t;
}

namespace Services::Map::Buildings::Visuals
{
	struct BuildingRenderSceneInput;
	class BuildingVisualsContext;
}

namespace Services::Map::Buildings::Rendering
{
	struct Rgba16fImageGrid;

	/** Reports synchronous and cursor-resumable numerical Section-image construction. */
	class SectionRendering final
	{
	public:
		/** Describes whether drawing, destination work, or finalization prevented a complete output. */
		enum class Status
		{
			Complete,
			CompleteWithOmissions,
			InvalidDestinationConfiguration,
			IncompleteDrawing,
			DestinationUnavailable,
			DestinationRejected,
			FinalizationFailed
		};

		/** Reports deterministic work consumed and the optional terminal result of one advancement. */
		struct StepResult
		{
			std::size_t WorkUnitsConsumed = 0;
			std::optional<Status> TerminalStatus;
		};

		/** Owns one persistent numerical Section operation without owning scene or Section state. */
		class IncrementalOperation final
		{
		public:
			/** Copies the grid configuration into a fresh operation; destination allocation is deferred. */
			explicit IncrementalOperation(const Rgba16fImageGrid& grid);
			IncrementalOperation(const IncrementalOperation&) = delete;
			IncrementalOperation& operator=(const IncrementalOperation&) = delete;
			IncrementalOperation(IncrementalOperation&&) = delete;
			IncrementalOperation& operator=(IncrementalOperation&&) = delete;
			/** Destroys any unfinished destination, cursors and partial output together. */
			~IncrementalOperation();

			/** Advances drawing and finalization using fresh borrowed views of the same logical scene.
			 * @param scene Fresh borrowed views of the same immutable ordered scene for this call only.
			 * @param context The same Section-owned numerical workspace for the operation's full lifetime.
			 * @param maximumWorkUnits Deterministic quota for destination construction, scene work and resolved pixels.
			 * @return Consumed units and a terminal status, or no status while the operation remains pending.
			 */
			StepResult Advance(
				const Visuals::BuildingRenderSceneInput& scene,
				Visuals::BuildingVisualsContext& context,
				std::size_t maximumWorkUnits);
			/** Moves fully finalized bytes and copies omission metadata once after successful completion.
			 * @param output Receives the completed RGBA16F bytes only on success.
			 * @param omissionSummary Receives fixed-size accepted terminal omission metadata only on success.
			 * @return True only once after successful finalization; failure leaves both outputs unchanged.
			 */
			bool TakeCompletedImage(
				std::vector<std::uint8_t>& output,
				Visuals::BuildingVisualsTerminalOmissionSummary& omissionSummary) noexcept;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			/** Provides read-only accumulated diagnostics for later scheduler reporting. */
			const Visuals::BuildingVisualsPhaseTimings& PhaseTimings() const noexcept;
#endif

		private:
			/** Advances the completed scene through existing failure and omission classification. */
			void EvaluateDrawing(
				const Visuals::BuildingRenderSceneInput& scene,
				Visuals::BuildingVisualsContext& context) noexcept;
			/** Advances one bounded scene slice and retains completion state for the next phase. */
			bool AdvanceDrawing(
				const Visuals::BuildingRenderSceneInput& scene,
				Visuals::BuildingVisualsContext& context,
				std::size_t maximumWorkUnits,
				StepResult& result);
			/** Validates and constructs the stable destination as one indivisible work unit. */
			bool ConstructDestination() noexcept;
			/** Advances output resolution and records its terminal outcome without exposing partial bytes. */
			void AdvanceFinalization(std::size_t maximumPixels, StepResult& result) noexcept;
			/** Stores an immutable terminal status and moves the operation into its terminal phase. */
			void Terminate(Status status) noexcept;

			struct State;
			std::unique_ptr<State> state_;
		};

		/**
		 * Draws prepared neutral contributors once and finalizes one complete RGBA16F image.
		 * The scene, grid and Section-owned context are borrowed for this synchronous call.
		 * Output is cleared before work begins and contains bytes only after successful finalization.
		 *
		 * @param scene Prepared neutral contributors and borrowed source spans.
		 * @param context Numerical workspace borrowed from the owning BuildingMapSection.
		 * @param grid Destination cell and pixel configuration supplied by the coordinator.
		 * @param output Caller-owned final row-major little-endian RGBA16F bytes.
		 * @param omissionSummary Receives fixed-size metadata only for accepted terminal spline omissions.
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		 * @param phaseTimings Receives elapsed CPU wall time and invocation flags for the six measured phases.
#endif
		 * @return The numerical rendering, destination or finalization outcome.
		 */
		static Status RenderSection(
			const Visuals::BuildingRenderSceneInput& scene,
			Visuals::BuildingVisualsContext& context,
			const Rgba16fImageGrid& grid,
			std::vector<std::uint8_t>& output,
			Visuals::BuildingVisualsTerminalOmissionSummary& omissionSummary
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			, Visuals::BuildingVisualsPhaseTimings& phaseTimings
#endif
			);

	private:
		/** Checks destination grid fields that can be rejected without recomputing world bounds. */
		static bool HasValidGridInputs(const Rgba16fImageGrid& grid) noexcept;

		/** Finds a destination submission outcome in one positional result vector. */
		static bool HasSubmissionOutcome(
			const std::vector<Visuals::BuildingVisualsContributorOutcome>& outcomes,
			ImageSubmission submission) noexcept;

		/** Identifies destination availability or raster rejection from retained drawing outcomes. */
		static Status GetDrawingFailureStatus(const Visuals::BuildingVisualsResult& result) noexcept;

		/** Accepts only no-drawable spline omissions and no other fatal scene outcome. */
		static bool TrySummarizeTerminalSplineOmissions(
			const Visuals::BuildingVisualsResult& result,
			const Visuals::BuildingRenderSceneInput& scene,
			Visuals::BuildingVisualsTerminalOmissionSummary& summary) noexcept;

		/** Verifies that a contributor category contains no fatal drawing outcomes. */
		static bool HasNoContributorFailures(
			const std::vector<Visuals::BuildingVisualsContributorOutcome>& outcomes) noexcept;
	};
}
