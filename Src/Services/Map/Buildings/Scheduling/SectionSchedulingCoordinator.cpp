#if defined(MODLOADER_CLIENT_BUILD)

// Owns shared viewport demand, delegates per-Section work to SectionScheduler, and reports completed-image changes to the service.
#include "BuildingMapLoadingTrace.h"
#include "SectionSchedulingCoordinator.h"

#include "../BuildingMapSection.h"
#include "../BuildingMapService.h"
#include "SectionScheduler.h"

#include "Services/Log.h"

#include <algorithm>
#include <optional>
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

	/** Orders waiting entries for direct stage lookup and deterministic rotation. */
	bool SectionSchedulingCoordinator::WaitingSection::operator<(const WaitingSection& other) const noexcept
	{
		if (Kind != other.Kind)
		{
			return Kind < other.Kind;
		}
		if (EligibleTick != other.EligibleTick)
		{
			return EligibleTick < other.EligibleTick;
		}
		return RotationPosition < other.RotationPosition;
	}

	/** Creates one scheduler that borrows the service-owned source manager. */
	SectionSchedulingCoordinator::SectionSchedulingCoordinator(
		double sectionSizeMapUnits,
		std::uint32_t sectionResolutionPixels,
		double spriteCaptureDensityPixelsPerMapUnit,
		std::filesystem::path buildingsCacheDirectory,
		Cache::SpriteSourceManager* spriteSourceManager)
		: sectionScheduler_(new SectionScheduler(
			sectionSizeMapUnits,
			sectionResolutionPixels,
			spriteCaptureDensityPixelsPerMapUnit,
			std::move(buildingsCacheDirectory),
			spriteSourceManager))
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
		std::int64_t endGridY,
		bool renderSectionImages)
	{
		if (!IsValidDemandRange(firstGridX, firstGridY, endGridX, endGridY))
		{
			return false;
		}
		AddDemandRange(firstGridX, firstGridY, endGridX, endGridY, renderSectionImages);
		return true;
	}

	/** Applies only newly entered and departed cells from one accepted viewport move. */
	bool SectionSchedulingCoordinator::ChangeViewportDemand(
		::Services::Map::Buildings::BuildingMapService& buildingMapService,
		std::int64_t previousFirstGridX,
		std::int64_t previousFirstGridY,
		std::int64_t previousEndGridX,
		std::int64_t previousEndGridY,
		std::int64_t nextFirstGridX,
		std::int64_t nextFirstGridY,
		std::int64_t nextEndGridX,
		std::int64_t nextEndGridY,
		bool renderSectionImages)
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
					RemoveDemandCoordinate(buildingMapService, coordinate, renderSectionImages);
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
					AddDemandCoordinate(coordinate, renderSectionImages);
					RequestSectionWork(coordinate);
				}
			}
		}
		return true;
	}

	/** Withdraws one accepted viewport range without disturbing overlapping demand. */
	void SectionSchedulingCoordinator::WithdrawViewportDemand(
		::Services::Map::Buildings::BuildingMapService& buildingMapService,
		std::int64_t firstGridX,
		std::int64_t firstGridY,
		std::int64_t endGridX,
		std::int64_t endGridY,
		bool renderSectionImages)
	{
		if (IsValidDemandRange(firstGridX, firstGridY, endGridX, endGridY))
		{
			RemoveDemandRange(
				buildingMapService,
				firstGridX,
				firstGridY,
				endGridX,
				endGridY,
				renderSectionImages);
		}
	}

	/** Clears viewport demand and resets unfinished state in all existing Sections. */
	void SectionSchedulingCoordinator::ClearViewportDemand(
		::Services::Map::Buildings::BuildingMapService& buildingMapService) noexcept
	{
		demandCounts_.clear();
		waitingSections_.clear();
		roundRobinPosition_ = 0;
		lastGpuGeometryPublicationOpportunity_.reset();
		ResetSchedulingBudgetState();
		for (auto& sectionEntry : buildingMapService.sections_)
		{
			sectionEntry.second->ResetRenderingState();
		}
	}

	/** Registers demand for source selection without disturbing an existing waiting entry. */
	void SectionSchedulingCoordinator::RequestSectionWork(const SectionCoordinate& coordinate)
	{
		const auto demand = demandCounts_.find(coordinate);
		if (demand != demandCounts_.end() && !demand->second.Waiting.has_value())
		{
			SetWaitingSection(coordinate, WaitingKind::AwaitingPublication);
		}
	}

	/** Reclassifies a changed or newly available selection without replacing a Section job. */
	void SectionSchedulingCoordinator::RecordSelectedPublication(
		::Services::Map::Buildings::BuildingMapService& buildingMapService,
		const SectionCoordinate& coordinate,
		const ::Services::Map::Buildings::BuildingCandidatePublication* publication,
		bool selectionChanged)
	{
		const auto demand = demandCounts_.find(coordinate);
		const bool hasWaiter = demand != demandCounts_.end() && demand->second.Waiting.has_value();
		const auto kind = hasWaiter ? (*demand->second.Waiting)->Kind : WaitingKind::AwaitingPublication;
		if (!selectionChanged && (!hasWaiter || kind != WaitingKind::AwaitingPublication))
		{
			return;
		}
		const auto* section = buildingMapService.FindSectionAt(coordinate.GridX, coordinate.GridY);
		if (selectionChanged && section != nullptr)
		{
			::Services::Map::Buildings::BuildingMapSection::ScenePresentationView presentation;
			if (section->TryBorrowPresentationScene(presentation) && !presentation.IsComplete)
			{
				buildingMapService.MarkGpuPresentationSnapshotChanged(coordinate.GridX, coordinate.GridY);
			}
		}
		if (!hasWaiter)
		{
			return;
		}
		if (publication != nullptr)
		{
			SetWaitingSection(coordinate, SectionScheduler::NeedsInitialSpriteVisit(section, *publication)
				? WaitingKind::InitialSprites : WaitingKind::Continuation);
		}
		else if (kind != WaitingKind::AwaitingPublication)
		{
			SetWaitingSection(coordinate, WaitingKind::AwaitingPublication);
		}
	}

	/** Rotates a demanded coordinate within one ordered set, preserving its existing node. */
	void SectionSchedulingCoordinator::SetWaitingSection(
		const SectionCoordinate& coordinate,
		WaitingKind kind,
		std::uint64_t eligibleTick)
	{
		auto& waiting = demandCounts_.at(coordinate).Waiting;
		const WaitingSection updated{
			kind, (std::max)(eligibleTick, budgetTickSequence_), ++roundRobinPosition_, coordinate};
		if (!waiting.has_value())
		{
			waiting = waitingSections_.insert(updated).first;
			return;
		}
		auto node = waitingSections_.extract(*waiting);
		node.value() = updated;
		waiting = waitingSections_.insert(std::move(node)).position;
	}

	/** Removes a waiting entry through the existing coordinate index. */
	void SectionSchedulingCoordinator::RemoveWaitingSection(const SectionCoordinate& coordinate) noexcept
	{
		const auto demand = demandCounts_.find(coordinate);
		if (demand != demandCounts_.end() && demand->second.Waiting.has_value())
		{
			waitingSections_.erase(*demand->second.Waiting);
			demand->second.Waiting.reset();
		}
	}

	/** Releases a completed job while retaining aggregate viewport demand. */
	void SectionSchedulingCoordinator::RecordSectionCompleted(
		::Services::Map::Buildings::BuildingMapService& buildingMapService,
		const SectionCoordinate& coordinate)
	{
		RemoveWaitingSection(coordinate);
		CancelSectionJob(buildingMapService, coordinate);
	}

	/** Advances acquisition first, then alternates Section and GPU work precedence under one deadline. */
	void SectionSchedulingCoordinator::AdvanceWaitingSections(
		::Services::Map::Buildings::BuildingMapService& buildingMapService,
		const SectionPublicationBatch& publications)
	{
		const auto advancementStarted = std::chrono::steady_clock::now();
		InitializeTickBudget(buildingMapService, advancementStarted);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		++diagnostics_.AdvancementCalls;
		const bool budgetWasAlreadyExhausted = consumedSchedulingMs_ >= nominalTickBudgetMs_;
#endif
		AdvanceSpriteAcquisitionOnce();
		const auto now = std::chrono::steady_clock::now();
		const bool admitDirtyGeometry = !lastGpuGeometryPublicationOpportunity_.has_value() ||
			now - *lastGpuGeometryPublicationOpportunity_ >= std::chrono::seconds(1);
		const bool gpuGeometryFirst = budgetTickSequence_ % 2 == 0;
		bool admissionConsumed = false;
		if (gpuGeometryFirst)
		{
			admissionConsumed = buildingMapService.AdvanceGpuGeometryPublications(
				tickDeadline_, admitDirtyGeometry);
		}
		AdvanceWaitingSectionSlices(buildingMapService, publications);
		if (!gpuGeometryFirst)
		{
			admissionConsumed = buildingMapService.AdvanceGpuGeometryPublications(
				tickDeadline_, admitDirtyGeometry);
		}
		if (admissionConsumed)
		{
			lastGpuGeometryPublicationOpportunity_ = std::chrono::steady_clock::now();
		}
		FinalizeTickBudget(advancementStarted
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			, budgetWasAlreadyExhausted
#endif
			);
	}

	/** Reads a new controller allowance only when its monotonic tick sequence changes. */
	void SectionSchedulingCoordinator::InitializeTickBudget(
		const ::Services::Map::Buildings::BuildingMapService& buildingMapService,
		std::chrono::steady_clock::time_point allowanceStarted)
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
		tickDeadline_ = allowanceStarted +
			std::chrono::duration_cast<std::chrono::steady_clock::duration>(
				std::chrono::duration<double, std::milli>(nominalTickBudgetMs_));
	}

	/** Advances acquisition once per coordinator call while the shared tick deadline remains. */
	void SectionSchedulingCoordinator::AdvanceSpriteAcquisitionOnce()
	{
		if (sectionScheduler_ == nullptr || std::chrono::steady_clock::now() >= tickDeadline_)
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

	/** Advances one waiting set under the shared allowance, rotating after each visit. */
	void SectionSchedulingCoordinator::AdvanceWaitingSectionSlices(
		::Services::Map::Buildings::BuildingMapService& buildingMapService,
		const SectionPublicationBatch& publications)
	{
		while (sectionScheduler_ != nullptr && !waitingSections_.empty() && IsTickAllowanceAvailable())
		{
			const auto selected = SelectNextWaitingSection();
			if (selected == waitingSections_.end())
			{
				return;
			}
			const WaitingSection waiting = *selected;
			AdvanceOneWaitingSection(buildingMapService, waiting, publications);
		}
	}

	/** Visits the selected publication once and records the retained job's next scheduling state. */
	void SectionSchedulingCoordinator::AdvanceOneWaitingSection(
		::Services::Map::Buildings::BuildingMapService& buildingMapService,
		const WaitingSection& waiting,
		const SectionPublicationBatch& publications)
	{
		const auto& coordinate = waiting.Coordinate;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const auto sourceSelectionStarted = std::chrono::steady_clock::now();
#endif
		const auto publication = publications.find({coordinate.GridX, coordinate.GridY});
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		diagnostics_.SourceSelection.Add(std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - sourceSelectionStarted).count());
#endif
		if (publication == publications.end() || publication->second == nullptr)
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			++diagnostics_.SourceSelectionMisses;
#endif
			SetWaitingSection(coordinate, WaitingKind::AwaitingPublication);
			return;
		}
		IncrementalSectionAdvanceResult result;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const auto sliceStarted = std::chrono::steady_clock::now();
#endif
		BM124D_SECTION(coordinate.GridX, coordinate.GridY, result = AdvanceSelectedSectionJob(
			buildingMapService, coordinate, publication->second, waiting.Kind == WaitingKind::InitialSprites));
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const double sliceElapsedMs = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - sliceStarted).count();
		RecordIncrementalDiagnostics(coordinate, sliceElapsedMs, result);
#endif
		UpdateWaitingSectionAfterVisit(waiting, result);
	}

	/** Keeps resource polls behind runnable work and parks jobs that require new source facts. */
	void SectionSchedulingCoordinator::UpdateWaitingSectionAfterVisit(
		const WaitingSection& waiting,
		const IncrementalSectionAdvanceResult& result)
	{
		using State = IncrementalSectionAdvanceResult::State;
		const auto& coordinate = waiting.Coordinate;
		if (result.AwaitingImageDemand || result.CurrentState == State::TerminalFailure)
		{
			RemoveWaitingSection(coordinate);
			return;
		}
		const auto& current = demandCounts_.at(coordinate).Waiting;
		if (!current.has_value())
		{
			return; // Successful image commitment already removed the waiter.
		}
		if (result.CurrentState == State::RestartRequired && !result.HasUnattemptedSprites)
		{
			SetWaitingSection(coordinate, WaitingKind::AwaitingSourceChange);
			return;
		}
		if (result.CurrentState == State::WaitingForResources)
		{
			SetWaitingSection(coordinate, result.HasSpriteRetries
				? WaitingKind::Continuation : WaitingKind::AwaitingSourceChange, budgetTickSequence_ + 1);
			return;
		}
		const auto kind = result.HasUnattemptedSprites ? WaitingKind::InitialSprites : WaitingKind::Continuation;
		const bool madeProgress = result.CurrentState == State::Pending &&
			(result.WorkUnitsConsumed != 0 || waiting.Kind != kind);
		SetWaitingSection(coordinate, kind, madeProgress ? budgetTickSequence_ : budgetTickSequence_ + 1);
	}

	/** Reports whether elapsed work leaves room for another indivisible Section visit. */
	bool SectionSchedulingCoordinator::IsTickAllowanceAvailable() const noexcept
	{
		return std::chrono::steady_clock::now() < tickDeadline_;
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
	/** Gives one continuation first access on every 32nd competing controller tick. */
	SectionSchedulingCoordinator::WaitingSections::const_iterator
	SectionSchedulingCoordinator::SelectNextWaitingSection()
	{
		const auto end = waitingSections_.end();
		const auto first = waitingSections_.begin();
		if (first == end || first->Kind > WaitingKind::Continuation)
		{
			return end;
		}
		const bool spritesReady = first->Kind == WaitingKind::InitialSprites &&
			first->EligibleTick <= budgetTickSequence_;
		if (spritesReady && lastSpritePriorityTick_ == budgetTickSequence_)
		{
			return first;
		}
		const auto continuation = first->Kind == WaitingKind::Continuation
			? first : waitingSections_.lower_bound(WaitingSection{WaitingKind::Continuation});
		const bool continuationReady = continuation != end && continuation->Kind == WaitingKind::Continuation &&
			continuation->EligibleTick <= budgetTickSequence_;
		if (spritesReady)
		{
			lastSpritePriorityTick_ = budgetTickSequence_;
			if (continuationReady && ++spritePriorityTickCount_ >= ContinuationFairnessIntervalTicks)
			{
				spritePriorityTickCount_ = 0;
				return continuation;
			}
			return first;
		}
		if (continuationReady)
		{
			spritePriorityTickCount_ = 0;
			return continuation;
		}
		return end;
	}

	/** Clears fairness history without replenishing the current tick's allowance. */
	void SectionSchedulingCoordinator::ResetSchedulingBudgetState() noexcept
	{
		lastSpritePriorityTick_.reset();
		spritePriorityTickCount_ = 0;
	}

	/** Advances one selected publication through the active round-robin visit adapter. */
	SectionSchedulingCoordinator::IncrementalSectionAdvanceResult
	SectionSchedulingCoordinator::AdvanceSelectedSectionJob(
		::Services::Map::Buildings::BuildingMapService& buildingMapService,
		const SectionCoordinate& coordinate,
		std::shared_ptr<const ::Services::Map::Buildings::BuildingCandidatePublication> publicationOwner,
		bool initialSpritesOnly)
	{
		if (!IsTickAllowanceAvailable())
		{
			IncrementalSectionAdvanceResult deferred;
			deferred.HasUnattemptedSprites = initialSpritesOnly;
			return deferred;
		}
		::Services::Map::Buildings::BuildingMapSection& section =
			buildingMapService.GetOrCreateSectionAt(coordinate.GridX, coordinate.GridY);
		publicationOwner = sectionScheduler_->EnsureSectionJob(section, publicationOwner);
		const SectionJobContinuationMode continuationMode =
			sectionScheduler_->SelectSectionJobContinuationMode(
				section,
				publicationOwner);
		auto result = AdvanceRetainedSectionJob(
			buildingMapService,
			coordinate,
			publicationOwner,
			section,
			continuationMode,
			initialSpritesOnly,
			HasImageDemand(coordinate));
		result.HasUnattemptedSprites = SectionScheduler::NeedsInitialSpriteVisit(&section, *publicationOwner);
		PublishGpuPresentationChange(buildingMapService, section);
		return result;
	}

	/** Queues only Sections whose scheduler invalidated selected drawable geometry. */
	void SectionSchedulingCoordinator::PublishGpuPresentationChange(
		::Services::Map::Buildings::BuildingMapService& buildingMapService,
		::Services::Map::Buildings::BuildingMapSection& section)
	{
		if (section.ConsumeGpuGeometrySnapshotChanged())
		{
			buildingMapService.MarkGpuPresentationSnapshotChanged(section.GetGridX(), section.GetGridY());
		}
		if (section.ConsumeGpuPresentationChanged())
		{
			buildingMapService.MarkGpuSectionPresentationChanged(section.GetGridX(), section.GetGridY());
		}
	}

	/** Advances one Section-owned job and handles only its actual terminal commit result. */
	SectionSchedulingCoordinator::IncrementalSectionAdvanceResult
	SectionSchedulingCoordinator::AdvanceRetainedSectionJob(
		::Services::Map::Buildings::BuildingMapService& buildingMapService,
		const SectionCoordinate& coordinate,
		const std::shared_ptr<const ::Services::Map::Buildings::BuildingCandidatePublication>& publicationOwner,
		::Services::Map::Buildings::BuildingMapSection& section,
		SectionJobContinuationMode continuationMode,
		bool initialSpritesOnly,
		bool renderSectionImages)
	{
		IncrementalSectionAdvanceResult result;
		SectionScheduler::SectionJobAdvanceResult jobResult = sectionScheduler_->AdvanceSectionJob(
			section,
			publicationOwner,
			continuationMode,
			buildingMapService.splinePolicies_,
			SectionScheduler::SectionPreparationVisit{tickDeadline_, budgetTickSequence_, initialSpritesOnly},
			renderSectionImages);
		result.WorkUnitsConsumed = jobResult.WorkUnitsConsumed;
		result.AwaitingImageDemand = jobResult.AwaitingImageDemand;
		result.HasSpriteRetries = jobResult.HasSpriteRetries;
		ReportTerminalSplineOmissions(coordinate, jobResult.TerminalSplineOmissions);
		if (jobResult.CurrentState == SectionScheduler::SectionJobAdvanceResult::State::WaitingForResources)
		{
			result.CurrentState = IncrementalSectionAdvanceResult::State::WaitingForResources;
		}
		else if (jobResult.CurrentState == SectionScheduler::SectionJobAdvanceResult::State::TerminalFailure)
		{
			result.CurrentState = IncrementalSectionAdvanceResult::State::TerminalFailure;
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
			commitResult.CompletedImageRemoved);
		return result;
	}

	/** Applies shared service events only for the two successful terminal commit outcomes. */
	void SectionSchedulingCoordinator::HandleSectionCommitResult(
		::Services::Map::Buildings::BuildingMapService& buildingMapService,
		const SectionCoordinate& coordinate,
		const std::shared_ptr<const ::Services::Map::Buildings::BuildingCandidatePublication>& publicationOwner,
		SectionCommitOutcome outcome,
		bool completedImageRemoved)
	{
		if (outcome == SectionCommitOutcome::Failed)
		{
			return;
		}
		buildingMapService.RecordSectionAppliedHash(
			coordinate.GridX,
			coordinate.GridY,
			publicationOwner->ContentHash());
		RecordSectionCompleted(buildingMapService, coordinate);
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

	/** Emits one bounded warning when a Section first promotes terminal spline omissions. */
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
		const char* failureReason = omissions.Reason ==
			::Services::Map::Buildings::Visuals::BuildingSplineOmissionReason::NumericalRejection
			? "NumericalRejection"
			: omissions.AlternativeStatus == Alternative::NoDrawableAlternative
				? "NoDrawableAlternative"
				: "Unknown";
		MiniMap::Services::Logf(
			*log,
			MiniMap::Services::Level::Warn,
			"MiniMap: BuildingMap Section grid=(%lld,%lld) completed with terminal omissions category=splines count=%llu firstSectionLocalContributor=%llu reason=%s",
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

	/** Resets Section state before stopping gameplay scheduling and capture resources. */
	void SectionSchedulingCoordinator::StopCaptureGameWorldSession(
		::Services::Map::Buildings::BuildingMapService& buildingMapService)
	{
		for (auto& sectionEntry : buildingMapService.sections_)
		{
			sectionEntry.second->ResetRenderingState();
		}
		if (sectionScheduler_ != nullptr)
		{
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
		else if (result.CurrentState == IncrementalSectionAdvanceResult::State::TerminalFailure)
		{
			++diagnostics_.FailedTerminalAttempts;
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
		std::int64_t endGridY,
		bool renderSectionImages)
	{
		for (std::int64_t gridY = firstGridY; gridY < endGridY; ++gridY)
		{
			for (std::int64_t gridX = firstGridX; gridX < endGridX; ++gridX)
			{
				AddDemandCoordinate(SectionCoordinate{gridX, gridY}, renderSectionImages);
			}
		}
	}

	/** Decrements aggregate demand once for every coordinate in one range. */
	void SectionSchedulingCoordinator::RemoveDemandRange(
		::Services::Map::Buildings::BuildingMapService& buildingMapService,
		std::int64_t firstGridX,
		std::int64_t firstGridY,
		std::int64_t endGridX,
		std::int64_t endGridY,
		bool renderSectionImages)
	{
		for (std::int64_t gridY = firstGridY; gridY < endGridY; ++gridY)
		{
			for (std::int64_t gridX = firstGridX; gridX < endGridX; ++gridX)
			{
				RemoveDemandCoordinate(
					buildingMapService,
					SectionCoordinate{gridX, gridY},
					renderSectionImages);
			}
		}
	}

	/** Adds one viewport reference and requeues shared coordinates for source reselection. */
	void SectionSchedulingCoordinator::AddDemandCoordinate(
		const SectionCoordinate& coordinate,
		bool renderSectionImages)
	{
		auto demandCount = demandCounts_.find(coordinate);
		if (demandCount != demandCounts_.end())
		{
			++demandCount->second.ViewportCount;
			if (renderSectionImages)
			{
				++demandCount->second.ImageViewportCount;
			}
			RequestSectionWork(coordinate);
			return;
		}
		AggregateSectionDemand demand;
		demand.ViewportCount = 1;
		demand.ImageViewportCount = renderSectionImages ? 1 : 0;
		demandCounts_.emplace(coordinate, demand);
		SetWaitingSection(coordinate, WaitingKind::AwaitingPublication);
	}

	/** Drops one reference, requeues shared demand, and withdraws work after the final departure. */
	void SectionSchedulingCoordinator::RemoveDemandCoordinate(
		::Services::Map::Buildings::BuildingMapService& buildingMapService,
		const SectionCoordinate& coordinate,
		bool renderSectionImages)
	{
		const auto demandCount = demandCounts_.find(coordinate);
		if (demandCount == demandCounts_.end())
		{
			return;
		}
		if (renderSectionImages && demandCount->second.ImageViewportCount > 0)
		{
			--demandCount->second.ImageViewportCount;
		}
		if (demandCount->second.ViewportCount > 1)
		{
			--demandCount->second.ViewportCount;
			RequestSectionWork(coordinate);
			return;
		}
		RemoveWaitingSection(coordinate);
		demandCounts_.erase(demandCount);
		if (sectionScheduler_ != nullptr)
		{
			const auto section = buildingMapService.FindSectionAt(coordinate.GridX, coordinate.GridY);
			const bool retainIncompleteJob =
				buildingMapService.IsSectionWithinAnyViewportResidencyRange(coordinate.GridX, coordinate.GridY) &&
				sectionScheduler_->CanRetainIncompleteJobOnDemandWithdrawal(section);
			if (!retainIncompleteJob)
			{
				CancelSectionJob(buildingMapService, coordinate);
			}
		}
	}

	/** Resets an existing Section's job state without creating absent sparse-grid entries. */
	void SectionSchedulingCoordinator::CancelSectionJob(
		::Services::Map::Buildings::BuildingMapService& buildingMapService,
		const SectionCoordinate& coordinate) noexcept
	{
		const auto section = buildingMapService.sections_.find(
			std::make_pair(coordinate.GridX, coordinate.GridY));
		if (section != buildingMapService.sections_.end())
		{
			section->second->ResetRenderingState();
		}
	}

	/** Checks the union count without consulting BuildingMapService's Section ownership. */
	bool SectionSchedulingCoordinator::IsDemanded(const SectionCoordinate& coordinate) const noexcept
	{
		const auto demandCount = demandCounts_.find(coordinate);
		return demandCount != demandCounts_.end() && demandCount->second.ViewportCount > 0;
	}

	/** Checks aggregate image demand independently from total Section demand. */
	bool SectionSchedulingCoordinator::HasImageDemand(const SectionCoordinate& coordinate) const noexcept
	{
		const auto demandCount = demandCounts_.find(coordinate);
		return demandCount != demandCounts_.end() && demandCount->second.ImageViewportCount > 0;
	}
}

#endif // MODLOADER_CLIENT_BUILD
