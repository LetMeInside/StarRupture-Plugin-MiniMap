#if defined(MODLOADER_CLIENT_BUILD)

// Owns shared viewport demand, delegates per-Section work to SectionScheduler, and reports completed-image changes to the service.
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
		const std::vector<SectionCoordinate> waitingSnapshot(waitingSections_.begin(), waitingSections_.end());
		sectionScheduler_->AdvanceSpriteAcquisition();
		for (const SectionCoordinate& coordinate : waitingSnapshot)
		{
			ProcessWaitingSection(buildingMapService, coordinate);
		}
	}

	/** Processes one still-demanded waiting coordinate from the current tick snapshot. */
	void SectionSchedulingCoordinator::ProcessWaitingSection(
		::Services::Map::Buildings::BuildingMapService& buildingMapService,
		const SectionCoordinate& coordinate)
	{
		if (!IsDemanded(coordinate) || waitingSections_.find(coordinate) == waitingSections_.end())
		{
			return;
		}
		::Services::Map::Buildings::BuildingMapService::SectionCandidateSource candidateSource;
		if (!buildingMapService.TrySelectCandidatePublicationForSection(
			coordinate.GridX,
			coordinate.GridY,
			candidateSource))
		{
			return;
		}
		::Services::Map::Buildings::BuildingMapSection& section =
			buildingMapService.GetOrCreateSectionAt(coordinate.GridX, coordinate.GridY);
		SectionScheduler::KnownSpritePreparationResult spritePreparation;
		sectionScheduler_->PrepareKnownSpriteRequirements(candidateSource.PublicationOwner, spritePreparation);
		SectionScheduler::SectionImageCommitResult commitResult;
		sectionScheduler_->RenderAndCommitSectionImage(
			section,
			candidateSource.PublicationOwner,
			candidateSource.Observation,
			spritePreparation,
			buildingMapService.splinePolicies_,
			commitResult);
		const bool imageCommitted =
			commitResult.CommitStatus == SectionScheduler::SectionImageCommitResult::Status::Committed;
		const bool emptyCommitted =
			commitResult.CommitStatus == SectionScheduler::SectionImageCommitResult::Status::CommittedEmpty;
		if (imageCommitted || emptyCommitted)
		{
			RecordSectionCompleted(coordinate);
		}
		if (imageCommitted || (emptyCommitted && commitResult.CompletedImageRemoved))
		{
			buildingMapService.NotifySectionImageChangedToViewports(
				coordinate.GridX,
				coordinate.GridY);
		}
	}

	/** Delegates acquisition-session creation to the owned SectionScheduler. */
	bool SectionSchedulingCoordinator::BeginCaptureGameWorldSession()
	{
		return sectionScheduler_ != nullptr &&
			sectionScheduler_->BeginCaptureGameWorldSession();
	}

	/** Delegates ordered acquisition-session retirement to the owned SectionScheduler. */
	bool SectionSchedulingCoordinator::AdvanceCaptureGameWorldSessionShutdown()
	{
		return sectionScheduler_ == nullptr || sectionScheduler_->AdvanceCaptureGameWorldSessionShutdown();
	}

	/** Delegates explicit building-cache clearing to the owned SectionScheduler. */
	bool SectionSchedulingCoordinator::ClearCache()
	{
		return sectionScheduler_ != nullptr && sectionScheduler_->ClearCache();
	}

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

	/** Adds one viewport reference and queues work on the first active demand. */
	void SectionSchedulingCoordinator::AddDemandCoordinate(const SectionCoordinate& coordinate)
	{
		auto demandCount = demandCounts_.find(coordinate);
		if (demandCount != demandCounts_.end())
		{
			++demandCount->second;
			return;
		}
		demandCounts_.emplace(coordinate, 1);
		waitingSections_.insert(coordinate);
	}

	/** Drops one viewport reference and withdraws waiting work after the final demand leaves. */
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
