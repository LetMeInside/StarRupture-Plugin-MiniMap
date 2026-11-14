#if defined(MODLOADER_CLIENT_BUILD)

// Implements synchronous and cursor-resumable numerical Section-image construction.
#include "SectionRendering.h"

#include "../Visuals/BuildingVisuals.h"
#include "../Visuals/BuildingRenderSceneInput.h"
#include "../Visuals/BuildingVisualsContext.h"
#include "../Visuals/BuildingVisualsResult.h"

#include <cmath>
#include <cstdint>
#include <climits>
#include <chrono>
#include <memory>
#include <new>
#include <optional>
#include <utility>
#include <vector>

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
#define RECORD_IMAGE_FAILURE(...) (state_->Failure = Visuals::SectionFailureDetails{__VA_ARGS__})
#else
#define RECORD_IMAGE_FAILURE(...) ((void)0)
#endif

namespace Services::Map::Buildings::Rendering
{
	/** Owns all stable numerical resources and progress for one resumable Section render. */
	struct SectionRendering::IncrementalOperation::State
	{
		/** Identifies the indivisible operations advanced by the persistent render. */
		enum class Phase
		{
			ConstructDestination,
			DrawScene,
			EvaluateDrawing,
			Finalize,
			Terminal
		};

		/** Copies the immutable rendering grid retained across every advancement. */
		explicit State(const ImageGrid& grid)
			: Grid(grid)
		{
		}

		const ImageGrid Grid;
		std::unique_ptr<IImageDestination> Destination;
		Visuals::BuildingVisuals::SceneCursor SceneProgress;
		IImageDestination::FinalizationCursor FinalizationProgress;
		std::vector<std::uint8_t> Output;
		Visuals::BuildingVisualsTerminalOmissionSummary OmissionSummary{};
		std::optional<Status> TerminalStatus;
		Status DrawingStatus = Status::Complete;
		Phase CurrentPhase = Phase::ConstructDestination;
		bool FinalizationComplete = false;
		bool ImageTaken = false;
		bool CompletedImageTransparent = false;
		bool DestinationConstructionStarted = false;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		Visuals::BuildingVisualsPhaseTimings PhaseTimings{};
		Visuals::SectionFailureDetails Failure;
#endif
	};

	/** Copies grid policy into a fresh operation and defers destination construction until positive work. */
	SectionRendering::IncrementalOperation::IncrementalOperation(const ImageGrid& grid)
		: state_(std::make_unique<State>(grid))
	{
	}

	/** Destroys unfinished numerical resources together without resetting active cursors. */
	SectionRendering::IncrementalOperation::~IncrementalOperation() = default;

	/** Identifies the numerical phases that may inspect this call's borrowed scene view. */
	bool SectionRendering::IncrementalOperation::RequiresSceneInput() const noexcept
	{
		return state_->CurrentPhase == State::Phase::ConstructDestination ||
			state_->CurrentPhase == State::Phase::DrawScene ||
			state_->CurrentPhase == State::Phase::EvaluateDrawing;
	}

	/** Advances destination creation, scene execution and finalization until the shared deadline. */
	SectionRendering::StepResult SectionRendering::IncrementalOperation::Advance(
		const Visuals::BuildingRenderSceneInput& scene,
		const Visuals::PreparedBuildingGeometry& geometry,
		Visuals::BuildingVisualsContext& context,
		std::chrono::steady_clock::time_point deadline)
	{
		StepResult result;
		if (state_->TerminalStatus.has_value())
		{
			result.TerminalStatus = state_->TerminalStatus;
			return result;
		}
		while (state_->CurrentPhase != State::Phase::Terminal &&
			std::chrono::steady_clock::now() < deadline)
		{
			if (state_->CurrentPhase == State::Phase::ConstructDestination)
			{
				if (ConstructDestination(deadline))
				{
					++result.WorkUnitsConsumed;
					continue;
				}
				break;
			}
			if (state_->CurrentPhase == State::Phase::DrawScene)
			{
				if (!AdvanceDrawing(scene, geometry, context, deadline, result))
				{
					break;
				}
				continue;
			}
			if (state_->CurrentPhase == State::Phase::EvaluateDrawing)
			{
				EvaluateDrawing(scene, geometry, context);
				++result.WorkUnitsConsumed;
				continue;
			}
			AdvanceFinalization(deadline, result);
			break;
		}
		result.TerminalStatus = state_->TerminalStatus;
		return result;
	}

	/** Runs one scene slice and advances only when BuildingVisuals reports terminal execution. */
	bool SectionRendering::IncrementalOperation::AdvanceDrawing(
		const Visuals::BuildingRenderSceneInput& scene,
		const Visuals::PreparedBuildingGeometry& geometry,
		Visuals::BuildingVisualsContext& context,
		std::chrono::steady_clock::time_point deadline,
		StepResult& result)
	{
		const auto sceneSlice = Visuals::BuildingVisuals::StepPreparedScene(
			geometry,
			scene,
			*state_->Destination,
			context,
			state_->SceneProgress,
			deadline);
		result.WorkUnitsConsumed += sceneSlice.WorkUnitsConsumed;
		if (!sceneSlice.Complete)
		{
			return false;
		}
		state_->CurrentPhase = State::Phase::EvaluateDrawing;
		return true;
	}

	/** Transfers only fully finalized bytes and fixed-size omission metadata, at most once. */
	bool SectionRendering::IncrementalOperation::TakeCompletedImage(
		std::vector<std::uint8_t>& output,
		Visuals::BuildingVisualsTerminalOmissionSummary& omissionSummary,
		bool& imageFullyTransparent) noexcept
	{
		if (!state_->TerminalStatus.has_value() || !state_->FinalizationComplete || state_->ImageTaken ||
			(*state_->TerminalStatus != Status::Complete &&
				*state_->TerminalStatus != Status::CompleteWithOmissions))
		{
			RECORD_IMAGE_FAILURE("image commitment",
				!state_->TerminalStatus.has_value() ? "TakeCompletedImage has no terminal status" :
				!state_->FinalizationComplete ? "TakeCompletedImage requires completed finalization" :
				state_->ImageTaken ? "TakeCompletedImage output already transferred" :
				"TakeCompletedImage requires successful terminal status", -1, -1, 0, 0, -1,
				state_->TerminalStatus.has_value() ? static_cast<int>(*state_->TerminalStatus) : -1);
			return false;
		}
		output = std::move(state_->Output);
		omissionSummary = state_->OmissionSummary;
		imageFullyTransparent = state_->CompletedImageTransparent;
		state_->ImageTaken = true;
		return true;
	}

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
	/** Returns the operation-owned accumulated numerical phase measurements. */
	const Visuals::BuildingVisualsPhaseTimings&
		SectionRendering::IncrementalOperation::PhaseTimings() const noexcept
	{
		return state_->PhaseTimings;
	}

	/** Returns the last terminal image-operation failure for the Section scheduler to report. */
	const Visuals::SectionFailureDetails&
		SectionRendering::IncrementalOperation::FailureDetails() const noexcept
	{
		return state_->Failure;
	}
#endif

	/** Validates the retained grid and obtains its destination through the neutral interface factory. */
	bool SectionRendering::IncrementalOperation::ConstructDestination(
		std::chrono::steady_clock::time_point deadline) noexcept
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const auto started = std::chrono::steady_clock::now();
#endif
		if (!state_->DestinationConstructionStarted)
		{
			if (!SectionRendering::HasValidGridInputs(state_->Grid))
			{
				RECORD_IMAGE_FAILURE("raster destination initialization", "invalid image grid dimensions, origin or scale");
				Terminate(Status::InvalidDestinationConfiguration);
				return false;
			}
			state_->Destination = IImageDestination::Create(state_->Grid, false);
			state_->DestinationConstructionStarted = true;
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		state_->PhaseTimings.DestinationConstruction.Invoked = true;
		state_->PhaseTimings.DestinationConstruction.ElapsedMs +=
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
#endif
		if (!state_->Destination)
		{
			RECORD_IMAGE_FAILURE("raster destination initialization", "destination factory returned null");
			Terminate(Status::DestinationUnavailable);
			return false;
		}
		if (!state_->Destination->StepInitializePixels(deadline))
		{
			if (state_->Destination->IsFailed())
			{
				RECORD_IMAGE_FAILURE("raster destination initialization", "destination pixel initialization failed");
				Terminate(Status::DestinationUnavailable);
			}
			return false;
		}
		state_->CurrentPhase = State::Phase::DrawScene;
		return true;
	}

	/** Applies the synchronous completed-scene failure and terminal-omission classification. */
	void SectionRendering::IncrementalOperation::EvaluateDrawing(
		const Visuals::BuildingRenderSceneInput&,
		const Visuals::PreparedBuildingGeometry& geometry,
		Visuals::BuildingVisualsContext& context) noexcept
	{
		const Visuals::BuildingVisualsResult* drawingResult =
			Visuals::BuildingVisuals::TryGetCompletedSceneResult(context, state_->SceneProgress);
		if (drawingResult == nullptr)
		{
			RECORD_IMAGE_FAILURE("prepared raster evaluation", "completed drawing result absent");
			Terminate(Status::IncompleteDrawing);
			return;
		}
		if (drawingResult->Complete)
		{
			state_->DrawingStatus = geometry.TerminalSplineOmissionOrdinals().empty()
				? Status::Complete
				: Status::CompleteWithOmissions;
			if (!geometry.TerminalSplineOmissionOrdinals().empty())
			{
				state_->OmissionSummary.FirstSectionLocalOrdinal =
					geometry.TerminalSplineOmissionOrdinals().front();
				state_->OmissionSummary.SplineContributorCount =
					geometry.TerminalSplineOmissionOrdinals().size();
				state_->OmissionSummary.FailureStage = Visuals::BuildingVisualsFailureStage::SurfacePreparation;
				state_->OmissionSummary.AlternativeStatus =
					Visuals::BuildingSplineAlternativeStatus::NoDrawableAlternative;
				state_->OmissionSummary.Reason = geometry.TerminalSplineOmissionReason();
			}
			state_->CurrentPhase = State::Phase::Finalize;
			return;
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		state_->PhaseTimings.CaptureFailureEvidence(*drawingResult);
		state_->Failure = state_->SceneProgress.FailureDetails();
		if (state_->Failure.Condition == nullptr)
		{
			RECORD_IMAGE_FAILURE("prepared raster evaluation", "drawing result is incomplete");
		}
#endif
		Terminate(SectionRendering::GetDrawingFailureStatus(*drawingResult));
	}

	/** Stores terminal status once and prevents any later phase from restarting. */
	void SectionRendering::IncrementalOperation::Terminate(Status status) noexcept
	{
		state_->TerminalStatus = status;
		state_->CurrentPhase = State::Phase::Terminal;
	}

	/** Advances one deadline-bounded destination finalization slice and records its result. */
	void SectionRendering::IncrementalOperation::AdvanceFinalization(
		std::chrono::steady_clock::time_point deadline,
		StepResult& result) noexcept
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const auto started = std::chrono::steady_clock::now();
#endif
		const auto finalization = state_->Destination->StepFinalize(
			state_->Output,
			state_->FinalizationProgress,
			deadline);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		state_->PhaseTimings.DestinationFinalization.Invoked = true;
		state_->PhaseTimings.DestinationFinalization.ElapsedMs +=
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
#endif
		result.WorkUnitsConsumed += finalization.PixelsResolved;
		if (finalization.Status == IImageDestination::FinalizationStepStatus::Failed)
		{
			state_->Output.clear();
			state_->OmissionSummary = {};
			RECORD_IMAGE_FAILURE("image finalization", "StepFinalize returned Failed",
				-1, -1, 0, 0, -1, static_cast<int>(finalization.Status));
			Terminate(Status::FinalizationFailed);
			return;
		}
		if (finalization.Status == IImageDestination::FinalizationStepStatus::Complete)
		{
			state_->FinalizationComplete = true;
			state_->CompletedImageTransparent = finalization.FullyTransparent;
			Terminate(state_->DrawingStatus);
		}
	}

	/** Rejects malformed dimensions, non-finite origins and invalid pixel scale at the caller boundary. */
	bool SectionRendering::HasValidGridInputs(const ImageGrid& grid) noexcept
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

#undef RECORD_IMAGE_FAILURE

#endif // MODLOADER_CLIENT_BUILD
