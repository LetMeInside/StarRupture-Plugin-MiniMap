#if defined(MODLOADER_CLIENT_BUILD)

// Implements generic immutable candidate retention and conservative grid observations.
#include "BuildingCandidatePublication.h"

#include <cmath>
#include <limits>
#include <utility>

namespace Services::Map::Buildings
{
	/** Takes completed producer evidence and initializes owned contiguous occurrence storage. */
	BuildingCandidatePublication::BuildingCandidatePublication(
		std::vector<BuildingCandidateOccurrence> occurrences,
		BuildingCandidateEvidence evidence,
		bool hasRequestedCoverage,
		BuildingCollectionRegion requestedCoverage,
		bool hasCoverage,
		BuildingCollectionRegion coverage,
		std::weak_ptr<const std::atomic<bool>> eligibility)
		: occurrences_(std::move(occurrences))
		, evidence_(evidence)
		, eligibility_(std::move(eligibility))
		, requestedCoverage_(requestedCoverage)
		, coverage_(coverage)
		, hasRequestedCoverage_(hasRequestedCoverage)
		, hasCoverage_(hasCoverage)
		, hasValidRequestedCoverage_(hasRequestedCoverage && IsValidRegion(requestedCoverage))
	{
		if (!hasValidRequestedCoverage_)
		{
			hasRequestedCoverage_ = false;
		}
		if (!hasCoverage_ || !hasValidRequestedCoverage_ || !IsValidRegion(coverage_))
		{
			hasCoverage_ = false;
			coverage_ = {};
		}
		if (!hasRequestedCoverage_)
		{
			requestedCoverage_ = {};
		}
		InitializeOccurrences();
	}

	/** Reads only the producer token and immutable construction validity. */
	bool BuildingCandidatePublication::IsCurrent() const noexcept
	{
		const auto eligibility = eligibility_.lock();
		return hasValidRequestedCoverage_ && eligibility != nullptr &&
			eligibility->load(std::memory_order_acquire);
	}

	/** Returns borrowed spans into storage owned independently by this publication. */
	BuildingCandidateData BuildingCandidatePublication::BorrowCandidateData() const noexcept
	{
		BuildingCandidateData data;
		data.hasRequestedCoverage = hasRequestedCoverage_;
		data.requestedCoverage = requestedCoverage_;
		data.hasCoverage = hasCoverage_;
		data.coverage = coverage_;
		data.evidence = evidence_;
		data.occurrences = std::span<const BuildingCandidateOccurrence>(occurrences_);
		return data;
	}

	/** Clears outputs first and fills them only after complete grid validation. */
	void BuildingCandidatePublication::ObserveCandidates(
		const BuildingSectionGrid& grid,
		std::span<BuildingCandidateObservation> observations) const
	{
		for (BuildingCandidateObservation& observation : observations)
		{
			observation = {};
		}
		if (!IsSupportedGrid(grid, observations.size()))
		{
			return;
		}
		for (std::size_t row = 0; row < grid.rowCount; ++row)
		{
			for (std::size_t column = 0; column < grid.columnCount; ++column)
			{
				BuildingCollectionRegion cellRegion;
				if (!TryCreateCellRegion(grid, column, row, cellRegion))
				{
					return;
				}
			}
		}
		FillObservations(grid, observations);
	}

	/** Assigns each input occurrence its publication-local ordinal and computes aggregate evidence. */
	void BuildingCandidatePublication::InitializeOccurrences()
	{
		ordinals_.reserve(occurrences_.size());
		for (std::size_t index = 0; index < occurrences_.size(); ++index)
		{
			const std::uint64_t ordinal = static_cast<std::uint64_t>(index);
			occurrences_[index].publicationOrdinal = ordinal;
			ordinals_.push_back(ordinal);
		}
		candidateSignature_ = ComputeCandidateSignature(occurrences_);
	}

	/** Uses the established order-independent wrapping Sum/Xor/Count signature formula. */
	std::uint64_t BuildingCandidatePublication::ComputeCandidateSignature(
		std::span<const BuildingCandidateOccurrence> occurrences) noexcept
	{
		std::uint64_t sum = 0;
		std::uint64_t exclusiveOr = 0;
		for (const BuildingCandidateOccurrence& occurrence : occurrences)
		{
			sum += occurrence.contentSignature;
			exclusiveOr ^= occurrence.contentSignature;
		}
		const std::uint64_t count = static_cast<std::uint64_t>(occurrences.size());
		const std::uint64_t rotatedXor = (exclusiveOr << 23) | (exclusiveOr >> 41);
		return FinalizeAggregate(sum ^ rotatedXor ^ (count * 0x9e3779b97f4a7c15ULL));
	}

	/** Validates finite positive cell dimensions and overflow-safe grid cardinality. */
	bool BuildingCandidatePublication::IsSupportedGrid(
		const BuildingSectionGrid& grid,
		std::size_t outputCount) noexcept
	{
		if (!std::isfinite(grid.xMapUnits) || !std::isfinite(grid.yMapUnits) ||
			!std::isfinite(grid.sectionSizeMapUnits) || grid.sectionSizeMapUnits <= 0.0 ||
			grid.columnCount == 0 || grid.rowCount == 0 ||
			grid.columnCount > (std::numeric_limits<std::size_t>::max)() / grid.rowCount ||
			grid.columnCount * grid.rowCount != outputCount)
		{
			return false;
		}
		const double width = static_cast<double>(grid.columnCount) * grid.sectionSizeMapUnits;
		const double height = static_cast<double>(grid.rowCount) * grid.sectionSizeMapUnits;
		const double right = grid.xMapUnits + width;
		const double bottom = grid.yMapUnits + height;
		return std::isfinite(width) && std::isfinite(height) &&
			std::isfinite(right) && std::isfinite(bottom) &&
			width > 0.0 && height > 0.0 && right > grid.xMapUnits && bottom > grid.yMapUnits;
	}

	/** Builds one half-open cell and rejects non-finite or collapsed boundaries. */
	bool BuildingCandidatePublication::TryCreateCellRegion(
		const BuildingSectionGrid& grid,
		std::size_t column,
		std::size_t row,
		BuildingCollectionRegion& region) noexcept
	{
		const double x = grid.xMapUnits + static_cast<double>(column) * grid.sectionSizeMapUnits;
		const double y = grid.yMapUnits + static_cast<double>(row) * grid.sectionSizeMapUnits;
		const double right = x + grid.sectionSizeMapUnits;
		const double bottom = y + grid.sectionSizeMapUnits;
		if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(right) || !std::isfinite(bottom) ||
			right <= x || bottom <= y)
		{
			return false;
		}
		region = {x, y, grid.sectionSizeMapUnits, grid.sectionSizeMapUnits};
		return true;
	}

	/** Requires a complete cell to fit inside producer-supplied acquired coverage. */
	bool BuildingCandidatePublication::IsCellCovered(const BuildingCollectionRegion& region) const noexcept
	{
		if (!hasCoverage_)
		{
			return false;
		}
		const double coverageRight = coverage_.xMapUnits + coverage_.widthMapUnits;
		const double coverageBottom = coverage_.yMapUnits + coverage_.heightMapUnits;
		const double cellRight = region.xMapUnits + region.widthMapUnits;
		const double cellBottom = region.yMapUnits + region.heightMapUnits;
		return region.xMapUnits >= coverage_.xMapUnits && region.yMapUnits >= coverage_.yMapUnits &&
			cellRight <= coverageRight && cellBottom <= coverageBottom;
	}

	/** Gives every valid cell all candidate ordinals and preserves incomplete coverage evidence. */
	void BuildingCandidatePublication::FillObservations(
		const BuildingSectionGrid& grid,
		std::span<BuildingCandidateObservation> observations) const
	{
		for (std::size_t row = 0; row < grid.rowCount; ++row)
		{
			for (std::size_t column = 0; column < grid.columnCount; ++column)
			{
				BuildingCandidateObservation& observation = observations[row * grid.columnCount + column];
				TryCreateCellRegion(grid, column, row, observation.requestedRegion);
				observation.evidence = evidence_;
				if (!IsCellCovered(observation.requestedRegion))
				{
					observation.evidence.candidateCoverageComplete = false;
				}
				observation.candidateOrdinals = std::span<const std::uint64_t>(ordinals_);
				observation.candidateSignature = candidateSignature_;
			}
		}
	}

	/** Applies the existing SplitMix64 finalization to candidate multiset evidence. */
	std::uint64_t BuildingCandidatePublication::FinalizeAggregate(std::uint64_t value) noexcept
	{
		value ^= value >> 30;
		value *= 0xbf58476d1ce4e5b9ULL;
		value ^= value >> 27;
		value *= 0x94d049bb133111ebULL;
		value ^= value >> 31;
		return value;
	}

	/** Validates a finite positive upper-left-origin rectangle before exposing it as coverage. */
	bool BuildingCandidatePublication::IsValidRegion(const BuildingCollectionRegion& region) noexcept
	{
		const double right = region.xMapUnits + region.widthMapUnits;
		const double bottom = region.yMapUnits + region.heightMapUnits;
		return std::isfinite(region.xMapUnits) && std::isfinite(region.yMapUnits) &&
			std::isfinite(region.widthMapUnits) && std::isfinite(region.heightMapUnits) &&
			region.widthMapUnits > 0.0 && region.heightMapUnits > 0.0 &&
			std::isfinite(right) && std::isfinite(bottom) &&
			right > region.xMapUnits && bottom > region.yMapUnits &&
			std::isfinite(std::hypot(region.widthMapUnits, region.heightMapUnits));
	}
}

#endif // MODLOADER_CLIENT_BUILD
