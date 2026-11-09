#if defined(MODLOADER_CLIENT_BUILD)

// Owns shared viewport demand, delegates per-Section work to SectionScheduler, and reports completed-image changes to the service.
#include "BuildingMapLoadingTrace.h"
#include "SectionSchedulingCoordinator.h"

#include "../BuildingMapSection.h"
#include "../BuildingMapService.h"
#include "SectionScheduler.h"

#include <utility>
#include <vector>

namespace Services::Map::Buildings::Scheduling
{
	/** Orders grid keys lexicographically for storage without defining a processing policy. */
	bool SectionSchedulingCoordinator::SectionCoordinate::operator<(
		const SectionCoordinate& other) const noexcept
	{
		return GridX < other.GridX || (GridX == other.GridX && GridY < other.GridY);
	}

	/** Creates exactly one scheduler and transfers cache-resource ownership beneath it. */
	SectionSchedulingCoordinator::SectionSchedulingCoordinator(
		double sectionSizeMapUnits,
		std::uint32_t sectionResolutionPixels,
		std::filesystem::path buildingsCacheDirectory)
		: sectionScheduler_(new SectionScheduler(
			sectionSizeMapUnits,
			sectionResolutionPixels,
			std::move(buildingsCacheDirectory)))
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		ResetDiagnostics();
#endif
	}

	/** Releases the owned scheduler after coordinator demand state is no longer accessible. */
	SectionSchedulingCoordinator::~SectionSchedulingCoordinator() = default;

	/** Adds one viewport's initial accepted rectangular demand. */
	bool SectionSchedulingCoordinator::AddViewportDemand(
		std::int64_t firstGridX,
		std::int64_t firstGridY,
		std::int64_t endGridX,
		std::int64_t endGridY)
	{
		if (!IsValidDemandRange(firstGridX, firstGridY, endGridX, endGridY))
		{
			return false;
		}
		AddDemandRange(firstGridX, firstGridY, endGridX, endGridY);
		return true;
	}

	/** Applies only newly entered and departed cells from one accepted viewport move. */
	bool SectionSchedulingCoordinator::ChangeViewportDemand(
		std::int64_t previousFirstGridX,
		std::int64_t previousFirstGridY,
		std::int64_t previousEndGridX,
		std::int64_t previousEndGridY,
		std::int64_t nextFirstGridX,
		std::int64_t nextFirstGridY,
		std::int64_t nextEndGridX,
		std::int64_t nextEndGridY)
	{
		if (!IsValidDemandRange(previousFirstGridX, previousFirstGridY, previousEndGridX, previousEndGridY) ||
			!IsValidDemandRange(nextFirstGridX, nextFirstGridY, nextEndGridX, nextEndGridY))
		{
			return false;
		}
		for (std::int64_t gridY = previousFirstGridY; gridY < previousEndGridY; ++gridY)
		{
			for (std::int64_t gridX = previousFirstGridX; gridX < previousEndGridX; ++gridX)
			{
				const SectionCoordinate coordinate{gridX, gridY};
				if (!ContainsCoordinate(nextFirstGridX, nextFirstGridY, nextEndGridX, nextEndGridY, coordinate))
				{
					RemoveDemandCoordinate(coordinate);
					RequestSectionWork(coordinate);
				}
			}
		}
		for (std::int64_t gridY = nextFirstGridY; gridY < nextEndGridY; ++gridY)
		{
			for (std::int64_t gridX = nextFirstGridX; gridX < nextEndGridX; ++gridX)
			{
				const SectionCoordinate coordinate{gridX, gridY};
				if (!ContainsCoordinate(
					previousFirstGridX,
					previousFirstGridY,
					previousEndGridX,
					previousEndGridY,
					coordinate))
				{
					AddDemandCoordinate(coordinate);
					RequestSectionWork(coordinate);
				}
			}
		}
		return true;
	}

	/** Withdraws one accepted viewport range without disturbing overlapping demand. */
	void SectionSchedulingCoordinator::WithdrawViewportDemand(
		std::int64_t firstGridX,
		std::int64_t firstGridY,
		std::int64_t endGridX,
		std::int64_t endGridY)
	{
		if (IsValidDemandRange(firstGridX, firstGridY, endGridX, endGridY))
		{
			RemoveDemandRange(firstGridX, firstGridY, endGridX, endGridY);
		}
	}

	/** Clears viewport demand bookkeeping without changing scheduler ownership or state. */
	void SectionSchedulingCoordinator::ClearViewportDemand() noexcept
	{
		demandCounts_.clear();
		waitingSections_.clear();
	}

	/** Requeues one demanded Section after a future dependency invalidation report. */
	void SectionSchedulingCoordinator::RequestSectionWork(const SectionCoordinate& coordinate)
	{
		if (IsDemanded(coordinate))
		{
			waitingSections_.insert(coordinate);
		}
	}

	/** Removes completed work from waiting while retaining its aggregate demand count. */
	void SectionSchedulingCoordinator::RecordSectionCompleted(const SectionCoordinate& coordinate)
	{
		waitingSections_.erase(coordinate);
	}

	/** Advances sprite acquisition once and attempts every initially waiting coordinate once. */
	void SectionSchedulingCoordinator::AdvanceWaitingSections(
		::Services::Map::Buildings::BuildingMapService& buildingMapService)
	{
		if (sectionScheduler_ == nullptr)
		{
			return;
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const auto advancementStarted = std::chrono::steady_clock::now();
		++diagnostics_.AdvancementCalls;
#endif
		const std::vector<SectionCoordinate> waitingSnapshot(waitingSections_.begin(), waitingSections_.end());
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		diagnostics_.InitialWaitingSections += waitingSnapshot.size();
		const auto spriteAcquisitionStarted = std::chrono::steady_clock::now();
#endif
		BM124D_RUN("AdvanceSpriteAcquisition", sectionScheduler_->AdvanceSpriteAcquisition());
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		diagnostics_.SpriteAcquisitionAdvancement.Add(std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - spriteAcquisitionStarted).count());
#endif
		for (const SectionCoordinate& coordinate : waitingSnapshot)
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			++diagnostics_.SnapshotCoordinatesExamined;
#endif
			BM124D_SECTION(coordinate.GridX, coordinate.GridY, ProcessWaitingSection(buildingMapService, coordinate));
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		diagnostics_.WaitingAfterAdvancement += waitingSections_.size();
		diagnostics_.CoordinatorAdvancement.Add(std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - advancementStarted).count());
#endif
	}

	/** Processes one still-demanded waiting coordinate from the current tick snapshot. */
	void SectionSchedulingCoordinator::ProcessWaitingSection(
		::Services::Map::Buildings::BuildingMapService& buildingMapService,
		const SectionCoordinate& coordinate)
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const auto attemptStarted = std::chrono::steady_clock::now();
#endif
		if (!IsDemanded(coordinate) || waitingSections_.find(coordinate) == waitingSections_.end())
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			BM124D_MARK("Section skipped: no longer eligible");
			++diagnostics_.NoLongerEligibleCoordinates;
#endif
			return;
		}
		::Services::Map::Buildings::BuildingMapService::SectionCandidateSource candidateSource;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const auto sourceSelectionStarted = std::chrono::steady_clock::now();
#endif
		const bool candidateSelected = BM124D_RUN("Section source selection", buildingMapService.TrySelectCandidatePublicationForSection(
			coordinate.GridX,
			coordinate.GridY,
			candidateSource));
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const double sourceSelectionMs = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - sourceSelectionStarted).count();
		diagnostics_.SourceSelection.Add(sourceSelectionMs);
#endif
		if (!candidateSelected)
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			BM124D_MARK("Section skipped: no candidate source");
			++diagnostics_.SourceSelectionMisses;
#endif
			return;
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		++diagnostics_.ProcessingAttempts;
#endif
		::Services::Map::Buildings::BuildingMapSection& section =
			buildingMapService.GetOrCreateSectionAt(coordinate.GridX, coordinate.GridY);
		SectionScheduler::KnownSpritePreparationResult spritePreparation;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const auto knownSpritePreparationStarted = std::chrono::steady_clock::now();
#endif
		BM124D_RUN("Section known sprite preparation", sectionScheduler_->PrepareKnownSpriteRequirements(candidateSource.PublicationOwner, spritePreparation));
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const double knownSpritePreparationMs = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - knownSpritePreparationStarted).count();
		diagnostics_.KnownSpritePreparation.Add(knownSpritePreparationMs);
#endif
		SectionScheduler::SectionImageCommitResult commitResult;
		BM124D_RUN("Section render and commit", sectionScheduler_->RenderAndCommitSectionImage(
			section,
			candidateSource.PublicationOwner,
			candidateSource.Observation,
			spritePreparation,
			buildingMapService.splinePolicies_,
			commitResult));
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const auto totalAttemptMs = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - attemptStarted).count();
		diagnostics_.WholeSectionAttempt.Add(totalAttemptMs);
		diagnostics_.RenderAndCommit.Add(commitResult.Timing.TotalMs);
		diagnostics_.ScenePreparation.Add(commitResult.Timing.ScenePreparationMs);
		const auto commitStatusIndex = static_cast<std::size_t>(commitResult.CommitStatus);
		if (commitStatusIndex < diagnostics_.CommitStatusCounts.size())
		{
			++diagnostics_.CommitStatusCounts[commitStatusIndex];
		}
		if (commitResult.SceneReadiness.has_value())
		{
			const auto readinessIndex = static_cast<std::size_t>(*commitResult.SceneReadiness);
			if (readinessIndex < diagnostics_.SceneReadinessCounts.size())
			{
				++diagnostics_.SceneReadinessCounts[readinessIndex];
			}
		}
		if (commitResult.Timing.NumericalRenderingInvoked && commitResult.NumericalStatus.has_value())
		{
			const auto numericalIndex = static_cast<std::size_t>(*commitResult.NumericalStatus);
			if (numericalIndex < diagnostics_.NumericalStatusCounts.size())
			{
				++diagnostics_.NumericalStatusCounts[numericalIndex];
			}
		}
		if (commitResult.Timing.NumericalRenderingInvoked)
		{
			diagnostics_.NumericalRendering.Add(commitResult.Timing.NumericalRenderingMs);
			const auto& phases = commitResult.Timing.NumericalPhases;
			if (phases.DestinationConstruction.Invoked)
			{
				diagnostics_.DestinationConstruction.Add(phases.DestinationConstruction.ElapsedMs);
			}
			if (phases.OrdinarySprites.Invoked)
			{
				diagnostics_.OrdinarySprites.Add(phases.OrdinarySprites.ElapsedMs);
			}
			if (phases.OcclusionIndexPreparation.Invoked)
			{
				diagnostics_.OcclusionIndexPreparation.Add(phases.OcclusionIndexPreparation.ElapsedMs);
			}
			if (phases.SplineDrawing.Invoked)
			{
				diagnostics_.SplineDrawing.Add(phases.SplineDrawing.ElapsedMs);
			}
			if (phases.OverlaySprites.Invoked)
			{
				diagnostics_.OverlaySprites.Add(phases.OverlaySprites.ElapsedMs);
			}
			if (phases.DestinationFinalization.Invoked)
			{
				diagnostics_.DestinationFinalization.Add(phases.DestinationFinalization.ElapsedMs);
			}
		}
		if (commitResult.Timing.TransparencyCheckInvoked)
		{
			diagnostics_.TransparencyCheck.Add(commitResult.Timing.TransparencyCheckMs);
		}
		if (commitResult.Timing.ImageAdoptionAttempted)
		{
			diagnostics_.ImageAdoption.Add(commitResult.Timing.ImageAdoptionMs);
		}
		if (commitResult.SceneReadiness.has_value() &&
			*commitResult.SceneReadiness == SectionScheduler::SectionScenePreparationResult::Readiness::CompleteSceneInputs &&
			!candidateSource.Observation.candidateOrdinals.empty())
		{
			diagnostics_.SceneBorrow.Add(commitResult.Timing.SceneBorrowMs);
		}
#endif
		const bool imageCommitted =
			commitResult.CommitStatus == SectionScheduler::SectionImageCommitResult::Status::Committed;
		const bool emptyCommitted =
			commitResult.CommitStatus == SectionScheduler::SectionImageCommitResult::Status::CommittedEmpty;
		if (imageCommitted || emptyCommitted)
		{
			RecordSectionCompleted(coordinate);
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		if (emptyCommitted)
		{
			if (commitResult.CompletedImageRemoved)
			{
				++diagnostics_.ExistingImagesRemoved;
			}
		}
		std::size_t diagnosticNotifications = 0;
#endif
		if (imageCommitted || (emptyCommitted && commitResult.CompletedImageRemoved))
		{
			buildingMapService.NotifySectionImageChangedToViewports(
				coordinate.GridX,
				coordinate.GridY
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
				, &diagnosticNotifications
#endif
				);
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		diagnostics_.ImageChangeNotificationsSent += diagnosticNotifications;
		if (totalAttemptMs >= diagnostics_.Worst.TotalMs)
		{
			diagnostics_.Worst.Present = true;
			diagnostics_.Worst.GridX = coordinate.GridX;
			diagnostics_.Worst.GridY = coordinate.GridY;
			diagnostics_.Worst.TotalMs = totalAttemptMs;
			diagnostics_.Worst.SourceSelectionMs = sourceSelectionMs;
			diagnostics_.Worst.KnownSpritePreparationMs = knownSpritePreparationMs;
			diagnostics_.Worst.RenderAndCommitMs = commitResult.Timing.TotalMs;
			diagnostics_.Worst.ScenePreparationMs = commitResult.Timing.ScenePreparationMs;
			diagnostics_.Worst.NumericalRenderingMs = commitResult.Timing.NumericalRenderingMs;
			diagnostics_.Worst.NumericalPhases = commitResult.Timing.NumericalPhases;
			diagnostics_.Worst.OrdinarySpriteInputCount = commitResult.Timing.OrdinarySpriteInputCount;
			diagnostics_.Worst.SplineContributorInputCount = commitResult.Timing.SplineContributorInputCount;
			diagnostics_.Worst.OverlayLastSpriteInputCount = commitResult.Timing.OverlayLastSpriteInputCount;
			diagnostics_.Worst.CommitStatus = static_cast<std::uint8_t>(commitResult.CommitStatus);
			diagnostics_.Worst.HasSceneReadiness = commitResult.SceneReadiness.has_value();
			diagnostics_.Worst.SceneReadiness = commitResult.SceneReadiness.has_value()
				? static_cast<std::uint8_t>(*commitResult.SceneReadiness)
				: 0;
			diagnostics_.Worst.HasNumericalStatus = commitResult.NumericalStatus.has_value();
			diagnostics_.Worst.NumericalStatus = commitResult.NumericalStatus.has_value()
				? static_cast<std::uint8_t>(*commitResult.NumericalStatus)
				: 0;
		}
#endif
	}

	/** Starts gameplay scheduling through the existing SectionScheduler. */
	bool SectionSchedulingCoordinator::BeginCaptureGameWorldSession()
	{
		if (sectionScheduler_ == nullptr || !sectionScheduler_->BeginCaptureGameWorldSession())
		{
			return false;
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		ResetDiagnostics();
#endif
		return true;
	}

	/** Stops gameplay scheduling and requests active capture cancellation once. */
	void SectionSchedulingCoordinator::StopCaptureGameWorldSession()
	{
		if (sectionScheduler_ != nullptr)
		{
			sectionScheduler_->StopCaptureGameWorldSession();
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		ResetDiagnostics();
#endif
	}

	/** Returns the persistent renderer without processing lifecycle work here. */
	Capture::BuildingSpriteRenderer* SectionSchedulingCoordinator::GetCapturerLifecycleTarget() noexcept
	{
		return sectionScheduler_ != nullptr
			? sectionScheduler_->GetCapturerLifecycleTarget()
			: nullptr;
	}

	/** Delegates explicit building-cache clearing to the owned SectionScheduler. */
	bool SectionSchedulingCoordinator::ClearCache()
	{
		return sectionScheduler_ != nullptr && sectionScheduler_->ClearCache();
	}

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
	/** Hands off one completed nonempty interval and resets only diagnostic observations. */
	bool SectionSchedulingCoordinator::TakeDiagnosticsSnapshot(
		SectionSchedulingDiagnosticsSnapshot& output) noexcept
	{
		const auto now = std::chrono::steady_clock::now();
		const double intervalMs = std::chrono::duration<double, std::milli>(
			now - diagnosticsIntervalStart_).count();
		if (intervalMs < 10000.0 || diagnostics_.AdvancementCalls == 0)
		{
			return false;
		}
		diagnostics_.IntervalMs = intervalMs;
		output = diagnostics_;
		ResetDiagnostics();
		return true;
	}

	/** Forwards the independent BuildingScheduler interval without touching Section aggregates. */
	bool SectionSchedulingCoordinator::TakeSpriteAcquisitionDiagnosticsSnapshot(
		BuildingScheduler::DiagnosticsSnapshot& output) noexcept
	{
		return sectionScheduler_ != nullptr &&
			sectionScheduler_->TakeSpriteAcquisitionDiagnosticsSnapshot(output);
	}

	/** Clears interval evidence while preserving every functional coordinator collection. */
	void SectionSchedulingCoordinator::ResetDiagnostics() noexcept
	{
		diagnostics_ = {};
		diagnosticsIntervalStart_ = std::chrono::steady_clock::now();
	}
#endif

	/** Tests one coordinate against both half-open range axes. */
	bool SectionSchedulingCoordinator::ContainsCoordinate(
		std::int64_t firstGridX,
		std::int64_t firstGridY,
		std::int64_t endGridX,
		std::int64_t endGridY,
		const SectionCoordinate& coordinate) noexcept
	{
		return coordinate.GridX >= firstGridX && coordinate.GridX < endGridX &&
			coordinate.GridY >= firstGridY && coordinate.GridY < endGridY;
	}

	/** Validates a nonempty signed half-open range. */
	bool SectionSchedulingCoordinator::IsValidDemandRange(
		std::int64_t firstGridX,
		std::int64_t firstGridY,
		std::int64_t endGridX,
		std::int64_t endGridY) noexcept
	{
		return firstGridX < endGridX && firstGridY < endGridY;
	}

	/** Increments aggregate demand once for every coordinate in one range. */
	void SectionSchedulingCoordinator::AddDemandRange(
		std::int64_t firstGridX,
		std::int64_t firstGridY,
		std::int64_t endGridX,
		std::int64_t endGridY)
	{
		for (std::int64_t gridY = firstGridY; gridY < endGridY; ++gridY)
		{
			for (std::int64_t gridX = firstGridX; gridX < endGridX; ++gridX)
			{
				AddDemandCoordinate(SectionCoordinate{gridX, gridY});
			}
		}
	}

	/** Decrements aggregate demand once for every coordinate in one range. */
	void SectionSchedulingCoordinator::RemoveDemandRange(
		std::int64_t firstGridX,
		std::int64_t firstGridY,
		std::int64_t endGridX,
		std::int64_t endGridY)
	{
		for (std::int64_t gridY = firstGridY; gridY < endGridY; ++gridY)
		{
			for (std::int64_t gridX = firstGridX; gridX < endGridX; ++gridX)
			{
				RemoveDemandCoordinate(SectionCoordinate{gridX, gridY});
			}
		}
	}

	/** Adds one viewport reference and requeues shared coordinates for source reselection. */
	void SectionSchedulingCoordinator::AddDemandCoordinate(const SectionCoordinate& coordinate)
	{
		auto demandCount = demandCounts_.find(coordinate);
		if (demandCount != demandCounts_.end())
		{
			++demandCount->second;
			RequestSectionWork(coordinate);
			return;
		}
		demandCounts_.emplace(coordinate, 1);
		waitingSections_.insert(coordinate);
	}

	/** Drops one reference, requeues shared demand, and withdraws work after the final departure. */
	void SectionSchedulingCoordinator::RemoveDemandCoordinate(const SectionCoordinate& coordinate)
	{
		const auto demandCount = demandCounts_.find(coordinate);
		if (demandCount == demandCounts_.end())
		{
			return;
		}
		if (demandCount->second > 1)
		{
			--demandCount->second;
			RequestSectionWork(coordinate);
			return;
		}
		demandCounts_.erase(demandCount);
		waitingSections_.erase(coordinate);
	}

	/** Checks the union count without consulting BuildingMapService's Section ownership. */
	bool SectionSchedulingCoordinator::IsDemanded(const SectionCoordinate& coordinate) const noexcept
	{
		return demandCounts_.find(coordinate) != demandCounts_.end();
	}
}

#endif // MODLOADER_CLIENT_BUILD
