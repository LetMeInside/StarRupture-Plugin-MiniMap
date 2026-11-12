#if defined(MODLOADER_CLIENT_BUILD)

// Implements read-only access to complete owned Building Map geometry.
#include "PreparedBuildingGeometry.h"

#include <limits>

namespace Services::Map::Buildings::Visuals
{
	/** Reports whether preparation completed for every required geometry input. */
	bool PreparedBuildingGeometry::IsComplete() const noexcept
	{
		return complete_;
	}

	/** Initializes a fresh scene's status slots to the exact publication occurrence count. */
	void PreparedBuildingGeometry::InitializeContributionStates(std::size_t contributionCount)
	{
		contributionStates_.assign(contributionCount, ContributionStatus::NotProcessed);
		unfinishedContributionCount_ = contributionCount;
		failedContributionCount_ = 0;
		terminalOmissionCount_ = 0;
		complete_ = false;
	}

	/** Rejects out-of-range ordinals and any change after an occurrence reaches a terminal state. */
	bool PreparedBuildingGeometry::TrySetContributionStatus(
		std::uint64_t ordinal,
		ContributionStatus status) noexcept
	{
		if (ordinal >= static_cast<std::uint64_t>(contributionStates_.size()))
		{
			return false;
		}
		ContributionStatus& current = contributionStates_[static_cast<std::size_t>(ordinal)];
		if (current == status)
		{
			return true;
		}
		const bool currentTerminal = current == ContributionStatus::Prepared ||
			current == ContributionStatus::TerminalOmission ||
			current == ContributionStatus::GeometricOmission ||
			current == ContributionStatus::Failed;
		if (currentTerminal || (current == ContributionStatus::NotProcessed && status == ContributionStatus::NotProcessed))
		{
			return false;
		}
		if (status == ContributionStatus::NotProcessed)
		{
			return false;
		}
		if (status == ContributionStatus::Prepared || status == ContributionStatus::TerminalOmission ||
			status == ContributionStatus::GeometricOmission ||
			status == ContributionStatus::Failed)
		{
			if (unfinishedContributionCount_ == 0)
			{
				return false;
			}
			--unfinishedContributionCount_;
			failedContributionCount_ += status == ContributionStatus::Failed ? 1 : 0;
			terminalOmissionCount_ += status == ContributionStatus::TerminalOmission ||
				status == ContributionStatus::GeometricOmission ? 1 : 0;
		}
		current = status;
		return true;
	}

	/** Allows completion only after every fixed publication occurrence has a successful terminal outcome. */
	bool PreparedBuildingGeometry::CanComplete() const noexcept
	{
		return unfinishedContributionCount_ == 0 && failedContributionCount_ == 0;
	}

	/** Returns the Section publication content hash associated with these contributions. */
	std::uint64_t PreparedBuildingGeometry::SourceContentHash() const noexcept
	{
		return sourceContentHash_;
	}

	/** Returns owned ordinary sprite values without transferring their storage. */
	const std::vector<PreparedBuildingGeometry::SpriteOccurrence>&
		PreparedBuildingGeometry::OrdinarySprites() const noexcept
	{
		return ordinarySprites_;
	}

	/** Returns owned clipped spline values without transferring their storage. */
	const std::vector<PreparedBuildingGeometry::SplineSurface>&
		PreparedBuildingGeometry::SplineSurfaces() const noexcept
	{
		return splineSurfaces_;
	}

	/** Returns source ordinals accepted as terminal no-drawable spline omissions. */
	const std::vector<std::uint64_t>& PreparedBuildingGeometry::TerminalSplineOmissionOrdinals() const noexcept
	{
		return terminalSplineOmissionOrdinals_;
	}

	/** Returns owned overlay-last sprite values without transferring their storage. */
	const std::vector<PreparedBuildingGeometry::SpriteOccurrence>&
		PreparedBuildingGeometry::OverlayLastSprites() const noexcept
	{
		return overlayLastSprites_;
	}

	/** Returns source-ordinal outcomes while preserving incomplete-scene visibility semantics. */
	const std::vector<PreparedBuildingGeometry::ContributionStatus>&
		PreparedBuildingGeometry::ContributionStates() const noexcept
	{
		return contributionStates_;
	}
}

#endif // MODLOADER_CLIENT_BUILD
