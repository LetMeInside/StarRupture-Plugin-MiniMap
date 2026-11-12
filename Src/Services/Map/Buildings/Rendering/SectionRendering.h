	// Owns a deadline-resumable Section image operation over prepared world-space geometry.
#pragma once

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <memory>
#include <optional>
#include <vector>
#include "../IImageDestination.h"
#include "../Visuals/BuildingVisualsResult.h"

namespace Services::Map::Buildings::Visuals
{
	struct BuildingRenderSceneInput;
	class BuildingVisualsContext;
	class PreparedBuildingGeometry;
}

namespace Services::Map::Buildings::Rendering
{
	/** Reports cursor-resumable Section-image construction from prepared geometry. */
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
			/** Copies the neutral grid into a fresh operation; interface destination allocation is deferred. */
			explicit IncrementalOperation(const ImageGrid& grid);
			IncrementalOperation(const IncrementalOperation&) = delete;
			IncrementalOperation& operator=(const IncrementalOperation&) = delete;
			IncrementalOperation(IncrementalOperation&&) = delete;
			IncrementalOperation& operator=(IncrementalOperation&&) = delete;
			/** Destroys any unfinished destination, cursors and partial output together. */
			~IncrementalOperation();
			/** Reports whether ConstructDestination, DrawScene or EvaluateDrawing may inspect the next scene input.
			 * @return True only before or during scene-dependent phases; false during finalization and terminal state.
			 */
			bool RequiresSceneInput() const noexcept;

			/** Advances drawing and finalization using fresh borrowed views of the same logical scene.
			 * @param scene Fresh borrowed views of the same immutable ordered scene for this call only.
			 * @param context The same Section-owned numerical workspace for the operation's full lifetime.
			 * @param deadline Absolute end of the shared scheduling allowance.
			 * @return Consumed units and a terminal status, or no status while the operation remains pending.
			 */
			StepResult Advance(
				const Visuals::BuildingRenderSceneInput& scene,
				const Visuals::PreparedBuildingGeometry& geometry,
				Visuals::BuildingVisualsContext& context,
				std::chrono::steady_clock::time_point deadline);
			/** Moves fully finalized bytes and copies omission metadata once after successful completion.
			 * @param output Receives the completed RGBA8 bytes only on success.
			 * @param omissionSummary Receives fixed-size accepted terminal omission metadata only on success.
			 * @param imageFullyTransparent Receives exact clear-pixel classification from completed finalization.
			 * @return True only once after successful finalization; failure leaves all outputs unchanged.
			 */
			bool TakeCompletedImage(
				std::vector<std::uint8_t>& output,
				Visuals::BuildingVisualsTerminalOmissionSummary& omissionSummary,
				bool& imageFullyTransparent) noexcept;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			/** Provides read-only accumulated diagnostics for later scheduler reporting. */
			const Visuals::BuildingVisualsPhaseTimings& PhaseTimings() const noexcept;
			/** Borrows the failed raster, finalization or handoff decision for this operation. */
			const Visuals::SectionFailureDetails& FailureDetails() const noexcept;
#endif

		private:
			/** Advances the completed scene through existing failure and omission classification. */
			void EvaluateDrawing(
				const Visuals::BuildingRenderSceneInput& scene,
				const Visuals::PreparedBuildingGeometry& geometry,
				Visuals::BuildingVisualsContext& context) noexcept;
			/** Advances scene cursors until completion or the absolute deadline. */
			bool AdvanceDrawing(
				const Visuals::BuildingRenderSceneInput& scene,
				const Visuals::PreparedBuildingGeometry& geometry,
				Visuals::BuildingVisualsContext& context,
				std::chrono::steady_clock::time_point deadline,
				StepResult& result);
			/** Creates the interface destination once and resumes transparent-pixel initialization. */
			bool ConstructDestination(std::chrono::steady_clock::time_point deadline) noexcept;
			/** Advances destination finalization and records its outcome without exposing partial bytes. */
			void AdvanceFinalization(std::chrono::steady_clock::time_point deadline, StepResult& result) noexcept;
			/** Stores an immutable terminal status and moves the operation into its terminal phase. */
			void Terminate(Status status) noexcept;

			struct State;
			std::unique_ptr<State> state_;
		};

	private:
		/** Checks destination grid fields that can be rejected without recomputing world bounds. */
		static bool HasValidGridInputs(const ImageGrid& grid) noexcept;

		/** Finds a destination submission outcome in one positional result vector. */
		static bool HasSubmissionOutcome(
			const std::vector<Visuals::BuildingVisualsContributorOutcome>& outcomes,
			ImageSubmission submission) noexcept;

		/** Identifies destination availability or raster rejection from retained drawing outcomes. */
		static Status GetDrawingFailureStatus(const Visuals::BuildingVisualsResult& result) noexcept;

	};
}
