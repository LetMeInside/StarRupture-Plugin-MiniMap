#if defined(MODLOADER_CLIENT_BUILD)

// Implements synchronous and cursor-resumable numerical Section-image construction.
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
#include <memory>
#include <new>
#include <optional>
#include <utility>
#include <vector>

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
		explicit State(const Rgba16fImageGrid& grid)
			: Grid(grid)
		{
		}

		const Rgba16fImageGrid Grid;
		std::unique_ptr<Rgba16fImageDestination> Destination;
		Visuals::BuildingVisuals::SceneCursor SceneProgress;
		Rgba16fImageDestination::FinalizationCursor FinalizationProgress;
		std::vector<std::uint8_t> Output;
		Visuals::BuildingVisualsTerminalOmissionSummary OmissionSummary{};
		std::optional<Status> TerminalStatus;
		Status DrawingStatus = Status::Complete;
		Phase CurrentPhase = Phase::ConstructDestination;
		bool FinalizationComplete = false;
		bool ImageTaken = false;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		Visuals::BuildingVisualsPhaseTimings PhaseTimings{};
#endif
	};

	/** Copies grid policy into a fresh operation and defers destination construction until positive work. */
	SectionRendering::IncrementalOperation::IncrementalOperation(const Rgba16fImageGrid& grid)
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

	/** Advances destination creation, scene execution, classification and finalization within one quota. */
	SectionRendering::StepResult SectionRendering::IncrementalOperation::Advance(
		const Visuals::BuildingRenderSceneInput& scene,
		Visuals::BuildingVisualsContext& context,
		std::size_t maximumWorkUnits)
	{
		StepResult result;
		if (state_->TerminalStatus.has_value())
		{
			result.TerminalStatus = state_->TerminalStatus;
			return result;
		}
		if (maximumWorkUnits == 0)
		{
			return result;
		}
		while (result.WorkUnitsConsumed < maximumWorkUnits &&
			state_->CurrentPhase != State::Phase::Terminal)
		{
			if (state_->CurrentPhase == State::Phase::ConstructDestination)
			{
				if (ConstructDestination())
				{
					++result.WorkUnitsConsumed;
					continue;
				}
				break;
			}
			if (state_->CurrentPhase == State::Phase::DrawScene)
			{
				const std::size_t remaining = maximumWorkUnits - result.WorkUnitsConsumed;
				if (!AdvanceDrawing(scene, context, remaining, result))
				{
					break;
				}
				continue;
			}
			if (state_->CurrentPhase == State::Phase::EvaluateDrawing)
			{
				EvaluateDrawing(scene, context);
				++result.WorkUnitsConsumed;
				continue;
			}
			AdvanceFinalization(maximumWorkUnits - result.WorkUnitsConsumed, result);
			break;
		}
		result.TerminalStatus = state_->TerminalStatus;
		return result;
	}

	/** Runs one scene slice and advances only when BuildingVisuals reports terminal execution. */
	bool SectionRendering::IncrementalOperation::AdvanceDrawing(
		const Visuals::BuildingRenderSceneInput& scene,
		Visuals::BuildingVisualsContext& context,
		std::size_t maximumWorkUnits,
		StepResult& result)
	{
		const auto sceneSlice = Visuals::BuildingVisuals::StepScene(
			scene,
			*state_->Destination,
			context,
			state_->SceneProgress,
			maximumWorkUnits
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			, state_->PhaseTimings
#endif
			);
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
		Visuals::BuildingVisualsTerminalOmissionSummary& omissionSummary) noexcept
	{
		if (!state_->TerminalStatus.has_value() || !state_->FinalizationComplete || state_->ImageTaken ||
			(*state_->TerminalStatus != Status::Complete &&
				*state_->TerminalStatus != Status::CompleteWithOmissions))
		{
			return false;
		}
		output = std::move(state_->Output);
		omissionSummary = state_->OmissionSummary;
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
#endif

	/** Validates the retained grid and constructs the destination directly at its stable owned address. */
	bool SectionRendering::IncrementalOperation::ConstructDestination() noexcept
	{
		if (!SectionRendering::HasValidGridInputs(state_->Grid))
		{
			Terminate(Status::InvalidDestinationConfiguration);
			return false;
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const auto started = std::chrono::steady_clock::now();
#endif
		state_->Destination.reset(new (std::nothrow) Rgba16fImageDestination(state_->Grid));
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		state_->PhaseTimings.DestinationConstruction.Invoked = true;
		state_->PhaseTimings.DestinationConstruction.ElapsedMs +=
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
#endif
		if (!state_->Destination)
		{
			Terminate(Status::DestinationUnavailable);
			return false;
		}
		state_->CurrentPhase = State::Phase::DrawScene;
		return true;
	}

	/** Applies the synchronous completed-scene failure and terminal-omission classification. */
	void SectionRendering::IncrementalOperation::EvaluateDrawing(
		const Visuals::BuildingRenderSceneInput& scene,
		Visuals::BuildingVisualsContext& context) noexcept
	{
		const Visuals::BuildingVisualsResult* drawingResult =
			Visuals::BuildingVisuals::TryGetCompletedSceneResult(context, state_->SceneProgress);
		if (drawingResult == nullptr)
		{
			Terminate(Status::IncompleteDrawing);
			return;
		}
		if (drawingResult->Complete)
		{
			state_->DrawingStatus = Status::Complete;
			state_->CurrentPhase = State::Phase::Finalize;
			return;
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		state_->PhaseTimings.CaptureFailureEvidence(*drawingResult);
#endif
		if (SectionRendering::TrySummarizeTerminalSplineOmissions(
			*drawingResult,
			scene,
			state_->OmissionSummary))
		{
			state_->DrawingStatus = Status::CompleteWithOmissions;
			state_->CurrentPhase = State::Phase::Finalize;
			return;
		}
		Terminate(SectionRendering::GetDrawingFailureStatus(*drawingResult));
	}

	/** Stores terminal status once and prevents any later phase from restarting. */
	void SectionRendering::IncrementalOperation::Terminate(Status status) noexcept
	{
		state_->TerminalStatus = status;
		state_->CurrentPhase = State::Phase::Terminal;
	}

	/** Advances one bounded output-resolution slice and records only actual resolved pixels. */
	void SectionRendering::IncrementalOperation::AdvanceFinalization(
		std::size_t maximumPixels,
		StepResult& result) noexcept
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const auto started = std::chrono::steady_clock::now();
#endif
		const auto finalization = state_->Destination->StepFinalize(
			state_->Output,
			state_->FinalizationProgress,
			maximumPixels);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		state_->PhaseTimings.DestinationFinalization.Invoked = true;
		state_->PhaseTimings.DestinationFinalization.ElapsedMs +=
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
#endif
		result.WorkUnitsConsumed += finalization.PixelsResolved;
		if (finalization.Status == Rgba16fImageDestination::FinalizationStepStatus::Failed)
		{
			state_->Output.clear();
			state_->OmissionSummary = {};
			Terminate(Status::FinalizationFailed);
			return;
		}
		if (finalization.Status == Rgba16fImageDestination::FinalizationStepStatus::Complete)
		{
			state_->FinalizationComplete = true;
			Terminate(state_->DrawingStatus);
		}
	}

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
