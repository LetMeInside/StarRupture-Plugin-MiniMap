#if defined(MODLOADER_CLIENT_BUILD)

// Owns shared viewport demand, delegates per-Section work to SectionScheduler, and reports completed-image changes to the service.
#include "BuildingMapLoadingTrace.h"
#include "SectionSchedulingCoordinator.h"

#include "../BuildingMapSection.h"
#include "../BuildingMapService.h"
#include "SectionScheduler.h"

#include "Services/Log.h"

#include <algorithm>
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

	/** Clears viewport demand and waiting state, then releases all retained Section jobs. */
	void SectionSchedulingCoordinator::ClearViewportDemand() noexcept
	{
		demandCounts_.clear();
		waitingSections_.clear();
		ResetSchedulingBudgetState();
		if (sectionScheduler_ != nullptr)
		{
			sectionScheduler_->ClearSectionJobs();
		}
	}

	/** Requeues one demanded Section after a future dependency invalidation report. */
	void SectionSchedulingCoordinator::RequestSectionWork(const SectionCoordinate& coordinate)
	{
		if (IsDemanded(coordinate))
		{
			waitingSections_.insert(coordinate);
		}
	}

	/** Removes completed work and its retained job while preserving aggregate demand. */
	void SectionSchedulingCoordinator::RecordSectionCompleted(const SectionCoordinate& coordinate)
	{
		waitingSections_.erase(coordinate);
		if (sectionScheduler_ != nullptr)
		{
			sectionScheduler_->CancelSectionJob(coordinate.GridX, coordinate.GridY);
		}
	}

	/** Advances acquisition once, then spends the shared tick allowance on fair bounded Section slices. */
	void SectionSchedulingCoordinator::AdvanceWaitingSections(
		::Services::Map::Buildings::BuildingMapService& buildingMapService,
		const SectionPublicationBatch& publications)
	{
		const auto advancementStarted = std::chrono::steady_clock::now();
		InitializeTickBudget(buildingMapService);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		++diagnostics_.AdvancementCalls;
		const bool budgetWasAlreadyExhausted = consumedSchedulingMs_ >= nominalTickBudgetMs_;
#endif
		AdvanceSpriteAcquisitionOnce();
		AdvanceWaitingSectionSlices(buildingMapService, publications, advancementStarted);
		FinalizeTickBudget(advancementStarted
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			, budgetWasAlreadyExhausted
#endif
			);
	}

	/** Reads a new controller allowance only when its monotonic tick sequence changes. */
	void SectionSchedulingCoordinator::InitializeTickBudget(
		const ::Services::Map::Buildings::BuildingMapService& buildingMapService)
	{
		const std::uint64_t tickSequence = buildingMapService.frameBudgetController_.TickSequence();
		if (budgetTickAvailable_ && budgetTickSequence_ == tickSequence)
		{
			return;
		}
		budgetTickAvailable_ = true;
		budgetTickSequence_ = tickSequence;
		nominalTickBudgetMs_ = buildingMapService.frameBudgetController_.NominalBudgetMs();
		consumedSchedulingMs_ = 0.0;
		deferredSectionsThisTick_.clear();
	}

	/** Advances acquisition on every serialized coordinator call, even when Section time is spent. */
	void SectionSchedulingCoordinator::AdvanceSpriteAcquisitionOnce()
	{
		if (sectionScheduler_ == nullptr)
		{
			return;
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const auto spriteAcquisitionStarted = std::chrono::steady_clock::now();
#endif
		BM124D_RUN("AdvanceSpriteAcquisition", sectionScheduler_->AdvanceSpriteAcquisition());
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		diagnostics_.SpriteAcquisitionAdvancement.Add(std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - spriteAcquisitionStarted).count());
#endif
	}

	/** Rotates runnable coordinates and gives each one a turn before repeating a progressing job. */
	void SectionSchedulingCoordinator::AdvanceWaitingSectionSlices(
		::Services::Map::Buildings::BuildingMapService& buildingMapService,
		const SectionPublicationBatch& publications,
		std::chrono::steady_clock::time_point advancementStarted)
	{
		if (sectionScheduler_ == nullptr)
		{
			return;
		}
		std::set<SectionCoordinate> visitedThisPass;
		while (!waitingSections_.empty() && IsTickAllowanceAvailable(advancementStarted))
		{
			SectionCoordinate coordinate;
			if (!TrySelectNextWaitingSection(coordinate, visitedThisPass))
			{
				visitedThisPass.clear();
				if (!TrySelectNextWaitingSection(coordinate, visitedThisPass))
				{
					return;
				}
			}
			visitedThisPass.insert(coordinate);
			roundRobinCursor_ = coordinate;
			roundRobinCursorAvailable_ = true;
			if (AdvanceOneWaitingSection(buildingMapService, coordinate, publications))
			{
				deferredSectionsThisTick_.insert(coordinate);
			}
		}
	}

	/** Selects and advances the current publication once, returning true when a same-tick retry is blocked. */
	bool SectionSchedulingCoordinator::AdvanceOneWaitingSection(
		::Services::Map::Buildings::BuildingMapService& buildingMapService,
		const SectionCoordinate& coordinate,
		const SectionPublicationBatch& publications)
	{
		if (!IsDemanded(coordinate) || waitingSections_.find(coordinate) == waitingSections_.end())
		{
			sectionScheduler_->CancelSectionJob(coordinate.GridX, coordinate.GridY);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			++diagnostics_.NoLongerEligibleCoordinates;
#endif
			return true;
		}
		std::shared_ptr<const ::Services::Map::Buildings::BuildingCandidatePublication> publicationOwner;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const auto sourceSelectionStarted = std::chrono::steady_clock::now();
#endif
		{
			const auto publicationEntry = publications.find(std::make_pair(coordinate.GridX, coordinate.GridY));
			if (publicationEntry != publications.end())
			{
				publicationOwner = publicationEntry->second;
			}
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		diagnostics_.SourceSelection.Add(std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - sourceSelectionStarted).count());
#endif
		if (publicationOwner == nullptr)
		{
			sectionScheduler_->CancelSectionJob(coordinate.GridX, coordinate.GridY);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			++diagnostics_.SourceSelectionMisses;
#endif
			return true;
		}
		IncrementalSectionAdvanceResult result;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const auto sliceStarted = std::chrono::steady_clock::now();
#endif
		BM124D_SECTION(coordinate.GridX, coordinate.GridY, result = AdvanceSelectedSectionJob(
			buildingMapService,
			coordinate,
			publicationOwner,
			MaximumSectionVisitWorkUnits));
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const double sliceElapsedMs = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - sliceStarted).count();
		RecordIncrementalDiagnostics(coordinate, sliceElapsedMs, result);
#endif
		return result.CurrentState != IncrementalSectionAdvanceResult::State::Pending ||
			result.WorkUnitsConsumed == 0;
	}

	/** Reports whether elapsed work leaves room for another indivisible Section visit. */
	bool SectionSchedulingCoordinator::IsTickAllowanceAvailable(
		std::chrono::steady_clock::time_point advancementStarted) const noexcept
	{
		const double elapsedThisCallMs = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - advancementStarted).count();
		return consumedSchedulingMs_ + elapsedThisCallMs < nominalTickBudgetMs_;
	}

	/** Commits one call's actual duration into shared per-tick consumption and records bounded Debug evidence. */
	void SectionSchedulingCoordinator::FinalizeTickBudget(
		std::chrono::steady_clock::time_point advancementStarted
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		, bool budgetWasAlreadyExhausted
#endif
		)
	{
		const double completedAdvancementMs = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - advancementStarted).count();
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const double previousOverrunMs = consumedSchedulingMs_ > nominalTickBudgetMs_
			? consumedSchedulingMs_ - nominalTickBudgetMs_
			: 0.0;
#endif
		consumedSchedulingMs_ += completedAdvancementMs;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const double currentOverrunMs = consumedSchedulingMs_ > nominalTickBudgetMs_
			? consumedSchedulingMs_ - nominalTickBudgetMs_
			: 0.0;
		if (!budgetWasAlreadyExhausted && consumedSchedulingMs_ >= nominalTickBudgetMs_)
		{
			++diagnostics_.BudgetExhaustionEvents;
		}
		if (currentOverrunMs > previousOverrunMs)
		{
			++diagnostics_.BudgetOverrunEvents;
			diagnostics_.BudgetOverrunTotalMs += currentOverrunMs - previousOverrunMs;
			diagnostics_.MaximumBudgetOverrunMs = (std::max)(
				diagnostics_.MaximumBudgetOverrunMs,
				currentOverrunMs);
		}
		diagnostics_.CoordinatorAdvancement.Add(completedAdvancementMs);
#endif
	}
	/** Selects a coordinate by value in strict rotation and never exposes a waiting-set iterator. */
	bool SectionSchedulingCoordinator::TrySelectNextWaitingSection(
		SectionCoordinate& output,
		const std::set<SectionCoordinate>& visitedThisPass) const
	{
		const auto first = roundRobinCursorAvailable_
			? waitingSections_.upper_bound(roundRobinCursor_)
			: waitingSections_.begin();
		for (auto candidate = first; candidate != waitingSections_.end(); ++candidate)
		{
			if (visitedThisPass.find(*candidate) == visitedThisPass.end() &&
				deferredSectionsThisTick_.find(*candidate) == deferredSectionsThisTick_.end())
			{
				output = *candidate;
				return true;
			}
		}
		for (auto candidate = waitingSections_.begin(); candidate != first; ++candidate)
		{
			if (visitedThisPass.find(*candidate) == visitedThisPass.end() &&
				deferredSectionsThisTick_.find(*candidate) == deferredSectionsThisTick_.end())
			{
				output = *candidate;
				return true;
			}
		}
		return false;
	}

	/** Clears cursor and per-tick deferral values without replenishing an allowance within its tick. */
	void SectionSchedulingCoordinator::ResetSchedulingBudgetState() noexcept
	{
		deferredSectionsThisTick_.clear();
		roundRobinCursor_ = {};
		roundRobinCursorAvailable_ = false;
	}

	/** Advances one selected publication through the active round-robin visit adapter. */
	SectionSchedulingCoordinator::IncrementalSectionAdvanceResult
	SectionSchedulingCoordinator::AdvanceSelectedSectionJob(
		::Services::Map::Buildings::BuildingMapService& buildingMapService,
		const SectionCoordinate& coordinate,
		std::shared_ptr<const ::Services::Map::Buildings::BuildingCandidatePublication> publicationOwner,
		std::size_t maximumWorkUnits)
	{
		IncrementalSectionAdvanceResult result;
		if (maximumWorkUnits == 0)
		{
			return result;
		}
		if (sectionScheduler_ == nullptr || publicationOwner == nullptr)
		{
			result.CurrentState = IncrementalSectionAdvanceResult::State::RestartRequired;
			return result;
		}
		sectionScheduler_->EnsureSectionJob(coordinate.GridX, coordinate.GridY, publicationOwner);
		::Services::Map::Buildings::BuildingMapSection& section =
			buildingMapService.GetOrCreateSectionAt(coordinate.GridX, coordinate.GridY);
		const bool requiresKnownSpritePreparation =
			sectionScheduler_->RequiresKnownSpritePreparationForNextAdvance(
				coordinate.GridX,
				coordinate.GridY,
				publicationOwner);
		return AdvanceRetainedSectionJob(
			buildingMapService,
			coordinate,
			publicationOwner,
			section,
			requiresKnownSpritePreparation,
			maximumWorkUnits);
	}

	/** Advances one retained job and handles only its actual terminal commit result. */
	SectionSchedulingCoordinator::IncrementalSectionAdvanceResult
	SectionSchedulingCoordinator::AdvanceRetainedSectionJob(
		::Services::Map::Buildings::BuildingMapService& buildingMapService,
		const SectionCoordinate& coordinate,
		const std::shared_ptr<const ::Services::Map::Buildings::BuildingCandidatePublication>& publicationOwner,
		::Services::Map::Buildings::BuildingMapSection& section,
		bool requiresKnownSpritePreparation,
		std::size_t maximumWorkUnits)
	{
		IncrementalSectionAdvanceResult result;
		SectionScheduler::KnownSpritePreparationResult spritePreparation;
		if (requiresKnownSpritePreparation)
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			const auto knownSpritePreparationStarted = std::chrono::steady_clock::now();
#endif
			sectionScheduler_->PrepareKnownSpriteRequirements(publicationOwner, spritePreparation);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			diagnostics_.KnownSpritePreparation.Add(std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - knownSpritePreparationStarted).count());
#endif
		}
		SectionScheduler::SectionJobAdvanceResult jobResult = sectionScheduler_->AdvanceSectionJob(
			section,
			publicationOwner,
			spritePreparation,
			buildingMapService.splinePolicies_,
			maximumWorkUnits);
		result.WorkUnitsConsumed = jobResult.WorkUnitsConsumed;
		if (jobResult.CurrentState == SectionScheduler::SectionJobAdvanceResult::State::WaitingForResources)
		{
			result.CurrentState = IncrementalSectionAdvanceResult::State::WaitingForResources;
		}
		else if (jobResult.CurrentState == SectionScheduler::SectionJobAdvanceResult::State::RestartRequired)
		{
			result.CurrentState = IncrementalSectionAdvanceResult::State::RestartRequired;
		}
		else if (jobResult.CurrentState == SectionScheduler::SectionJobAdvanceResult::State::AttemptFinished)
		{
			result.CurrentState = IncrementalSectionAdvanceResult::State::AttemptFinished;
		}
		if (result.CurrentState != IncrementalSectionAdvanceResult::State::AttemptFinished)
		{
			return result;
		}
		if (!jobResult.FinishedAttempt.has_value())
		{
			result.CurrentState = IncrementalSectionAdvanceResult::State::RestartRequired;
			return result;
		}
		const SectionScheduler::SectionImageCommitResult& commitResult = *jobResult.FinishedAttempt;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		result.HasSceneReadiness = commitResult.SceneReadiness.has_value();
		result.SceneBorrowMeasured = commitResult.SceneReadiness.has_value() &&
			*commitResult.SceneReadiness == SectionScheduler::SectionScenePreparationResult::Readiness::CompleteSceneInputs &&
			!publicationOwner->BorrowCandidateData().occurrences.empty();
		result.SceneReadiness = commitResult.SceneReadiness.has_value()
			? static_cast<std::uint8_t>(*commitResult.SceneReadiness)
			: 0;
		result.HasCommitStatus = true;
		result.CommitStatus = static_cast<std::uint8_t>(commitResult.CommitStatus);
		result.HasNumericalStatus = commitResult.NumericalStatus.has_value();
		result.NumericalStatus = commitResult.NumericalStatus.has_value()
			? static_cast<std::uint8_t>(*commitResult.NumericalStatus)
			: 0;
		result.ScenePreparationMs = commitResult.Timing.ScenePreparationMs;
		result.SceneBorrowMs = commitResult.Timing.SceneBorrowMs;
		result.NumericalRenderingMs = commitResult.Timing.NumericalRenderingMs;
		result.TransparencyCheckMs = commitResult.Timing.TransparencyCheckMs;
		result.ImageAdoptionMs = commitResult.Timing.ImageAdoptionMs;
		result.NumericalRenderingInvoked = commitResult.Timing.NumericalRenderingInvoked;
		result.TransparencyCheckInvoked = commitResult.Timing.TransparencyCheckInvoked;
		result.ImageAdoptionAttempted = commitResult.Timing.ImageAdoptionAttempted;
		result.NumericalPhases = commitResult.Timing.NumericalPhases;
#endif
		SectionCommitOutcome completionOutcome = SectionCommitOutcome::Failed;
		if (commitResult.CommitStatus == SectionScheduler::SectionImageCommitResult::Status::Committed)
		{
			completionOutcome = SectionCommitOutcome::ImageCommitted;
		}
		else if (commitResult.CommitStatus == SectionScheduler::SectionImageCommitResult::Status::CommittedEmpty)
		{
			completionOutcome = SectionCommitOutcome::EmptyCommitted;
		}
		HandleSectionCommitResult(
			buildingMapService,
			coordinate,
			publicationOwner,
			completionOutcome,
			commitResult.CompletedImageRemoved,
			commitResult.TerminalSplineOmissions);
		return result;
	}

	/** Applies shared service events only for the two successful terminal commit outcomes. */
	void SectionSchedulingCoordinator::HandleSectionCommitResult(
		::Services::Map::Buildings::BuildingMapService& buildingMapService,
		const SectionCoordinate& coordinate,
		const std::shared_ptr<const ::Services::Map::Buildings::BuildingCandidatePublication>& publicationOwner,
		SectionCommitOutcome outcome,
		bool completedImageRemoved,
		const ::Services::Map::Buildings::Visuals::BuildingVisualsTerminalOmissionSummary& omissions)
	{
		if (outcome == SectionCommitOutcome::Failed)
		{
			return;
		}
		buildingMapService.RecordSectionAppliedHash(
			coordinate.GridX,
			coordinate.GridY,
			publicationOwner->ContentHash());
		RecordSectionCompleted(coordinate);
		ReportTerminalSplineOmissions(coordinate, omissions);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		if (outcome == SectionCommitOutcome::EmptyCommitted && completedImageRemoved)
		{
			++diagnostics_.ExistingImagesRemoved;
		}
		std::size_t diagnosticNotifications = 0;
#endif
		if (outcome == SectionCommitOutcome::ImageCommitted || completedImageRemoved)
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
#endif
	}

	/** Emits one bounded warning after a Section with terminal spline omissions commits. */
	void SectionSchedulingCoordinator::ReportTerminalSplineOmissions(
		const SectionCoordinate& coordinate,
		const ::Services::Map::Buildings::Visuals::BuildingVisualsTerminalOmissionSummary& omissions) const noexcept
	{
		using Alternative = ::Services::Map::Buildings::Visuals::BuildingSplineAlternativeStatus;
		if (omissions.SplineContributorCount == 0)
		{
			return;
		}
		MiniMap::Services::ILog* log = MiniMap::Services::TryLog();
		if (log == nullptr)
		{
			return;
		}
		const char* failureReason = omissions.AlternativeStatus == Alternative::NoDrawableAlternative
			? "NoDrawableAlternative"
			: "Unknown";
		MiniMap::Services::Logf(
			*log,
			MiniMap::Services::Level::Warn,
			"MiniMap: BuildingMap Section grid=(%lld,%lld) committed with terminal omissions category=splines count=%llu firstSectionLocalContributor=%llu reason=%s",
			static_cast<long long>(coordinate.GridX),
			static_cast<long long>(coordinate.GridY),
			static_cast<unsigned long long>(omissions.SplineContributorCount),
			static_cast<unsigned long long>(omissions.FirstSectionLocalOrdinal),
			failureReason);
	}

	/** Starts gameplay scheduling through the existing SectionScheduler. */
	bool SectionSchedulingCoordinator::BeginCaptureGameWorldSession()
	{
		if (sectionScheduler_ == nullptr || !sectionScheduler_->BeginCaptureGameWorldSession())
		{
			return false;
		}
		ResetSchedulingBudgetState();
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
			sectionScheduler_->ClearSectionJobs();
			sectionScheduler_->StopCaptureGameWorldSession();
		}
		ResetSchedulingBudgetState();
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

	/** Aggregates one actual slice and terminal-only status evidence without treating waits as failures. */
	void SectionSchedulingCoordinator::RecordIncrementalDiagnostics(
		const SectionCoordinate& coordinate,
		double sliceElapsedMs,
		const IncrementalSectionAdvanceResult& result) noexcept
	{
		++diagnostics_.SectionSlicesAttempted;
		diagnostics_.SectionSlice.Add(sliceElapsedMs);
		diagnostics_.WorkUnitsConsumed += result.WorkUnitsConsumed;
		if (sliceElapsedMs >= diagnostics_.WorstSlice.ElapsedMs)
		{
			diagnostics_.WorstSlice.Present = true;
			diagnostics_.WorstSlice.GridX = coordinate.GridX;
			diagnostics_.WorstSlice.GridY = coordinate.GridY;
			diagnostics_.WorstSlice.ElapsedMs = sliceElapsedMs;
			diagnostics_.WorstSlice.WorkUnitsConsumed = result.WorkUnitsConsumed;
			diagnostics_.WorstSlice.AdvancementState = static_cast<std::uint8_t>(result.CurrentState);
		}
		if (result.CurrentState == IncrementalSectionAdvanceResult::State::Pending)
		{
			++diagnostics_.PendingResults;
		}
		else if (result.CurrentState == IncrementalSectionAdvanceResult::State::WaitingForResources)
		{
			++diagnostics_.WaitingForResourcesResults;
		}
		else if (result.CurrentState == IncrementalSectionAdvanceResult::State::RestartRequired)
		{
			++diagnostics_.RestartRequiredResults;
		}
		else
		{
			++diagnostics_.AttemptFinishedResults;
		}
		if (result.HasCommitStatus)
		{
			RecordTerminalAttemptDiagnostics(result);
		}
	}

	/** Aggregates actual terminal commit, readiness, numerical and completed-operation phase evidence. */
	void SectionSchedulingCoordinator::RecordTerminalAttemptDiagnostics(
		const IncrementalSectionAdvanceResult& result) noexcept
	{
		RecordTerminalStatusDiagnostics(result);
		RecordTerminalTimingDiagnostics(result);
	}

	/** Counts terminal result categories without treating intermediate job states as commits. */
	void SectionSchedulingCoordinator::RecordTerminalStatusDiagnostics(
		const IncrementalSectionAdvanceResult& result) noexcept
	{
		if (result.HasCommitStatus)
		{
			if (result.CommitStatus < diagnostics_.CommitStatusCounts.size())
			{
				++diagnostics_.CommitStatusCounts[result.CommitStatus];
			}
			if (result.CommitStatus == static_cast<std::uint8_t>(
				SectionScheduler::SectionImageCommitResult::Status::Committed))
			{
				++diagnostics_.SuccessfulImageCommits;
			}
			else if (result.CommitStatus == static_cast<std::uint8_t>(
				SectionScheduler::SectionImageCommitResult::Status::CommittedEmpty))
			{
				++diagnostics_.SuccessfulEmptyCommits;
			}
			else
			{
				++diagnostics_.FailedTerminalAttempts;
			}
		}
		if (result.HasSceneReadiness && result.SceneReadiness < diagnostics_.SceneReadinessCounts.size())
		{
			++diagnostics_.SceneReadinessCounts[result.SceneReadiness];
		}
		if (result.HasNumericalStatus)
		{
			if (result.NumericalStatus < diagnostics_.NumericalStatusCounts.size())
			{
				++diagnostics_.NumericalStatusCounts[result.NumericalStatus];
			}
		}
	}

	/** Adds cumulative active operation phases once, only when the terminal result carries them. */
	void SectionSchedulingCoordinator::RecordTerminalTimingDiagnostics(
		const IncrementalSectionAdvanceResult& result) noexcept
	{
		if (result.HasSceneReadiness)
		{
			diagnostics_.ScenePreparation.Add(result.ScenePreparationMs);
		}
		if (result.SceneBorrowMeasured)
		{
			diagnostics_.SceneBorrow.Add(result.SceneBorrowMs);
		}
		if (result.TransparencyCheckInvoked)
		{
			diagnostics_.TransparencyCheck.Add(result.TransparencyCheckMs);
		}
		if (result.ImageAdoptionAttempted)
		{
			diagnostics_.ImageAdoption.Add(result.ImageAdoptionMs);
		}
		if (!result.NumericalRenderingInvoked)
		{
			return;
		}
		diagnostics_.NumericalRendering.Add(result.NumericalRenderingMs);
		const auto& phases = result.NumericalPhases;
		AddPhaseDuration(phases.DestinationConstruction, diagnostics_.DestinationConstruction);
		AddPhaseDuration(phases.OrdinarySprites, diagnostics_.OrdinarySprites);
		AddPhaseDuration(phases.OcclusionIndexPreparation, diagnostics_.OcclusionIndexPreparation);
		AddPhaseDuration(phases.SplineDrawing, diagnostics_.SplineDrawing);
		AddPhaseDuration(phases.OverlaySprites, diagnostics_.OverlaySprites);
		AddPhaseDuration(phases.DestinationFinalization, diagnostics_.DestinationFinalization);
	}

	/** Adds one invoked phase's active CPU duration without counting suspension between slices. */
	void SectionSchedulingCoordinator::AddPhaseDuration(
		const Visuals::BuildingVisualsPhaseTimings::Phase& phase,
		SectionSchedulingDiagnosticsSnapshot::Duration& aggregate) noexcept
	{
		if (phase.Invoked)
		{
			aggregate.Add(phase.ElapsedMs);
		}
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
		if (sectionScheduler_ != nullptr)
		{
			sectionScheduler_->CancelSectionJob(coordinate.GridX, coordinate.GridY);
		}
	}

	/** Checks the union count without consulting BuildingMapService's Section ownership. */
	bool SectionSchedulingCoordinator::IsDemanded(const SectionCoordinate& coordinate) const noexcept
	{
		return demandCounts_.find(coordinate) != demandCounts_.end();
	}
}

#endif // MODLOADER_CLIENT_BUILD
