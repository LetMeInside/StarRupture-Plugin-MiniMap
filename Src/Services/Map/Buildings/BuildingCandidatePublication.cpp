// Implements owned immutable Section candidate/source publications.
#if defined(MODLOADER_CLIENT_BUILD)
#include "BuildingCandidatePublication.h"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace Services::Map::Buildings
{
	/** Takes ownership of aligned Section records and retains the supplied hash verbatim. */
	BuildingCandidatePublication::BuildingCandidatePublication(
		BuildingCollectionRegion sectionRegion,
		std::vector<BuildingCandidateOccurrence> occurrences,
		std::vector<BuildingCandidateSourceRecord> sourceRecords,
		std::uint64_t contentHash)
		: sectionRegion_(sectionRegion)
		, occurrences_(std::move(occurrences))
		, sourceRecords_(std::move(sourceRecords))
		, contentHash_(contentHash)
	{
		if (!IsValidRegion(sectionRegion_))
		{
			throw std::invalid_argument("Section publications require a finite positive rectangle.");
		}
		if (sourceRecords_.size() != occurrences_.size())
		{
			throw std::invalid_argument(
				"Section candidate and source records must have matching cardinality.");
		}
		for (std::size_t index = 0; index < occurrences_.size(); ++index)
		{
			if (occurrences_[index].publicationOrdinal != static_cast<std::uint64_t>(index))
			{
				throw std::invalid_argument(
					"Section candidate ordinals must be contiguous and match source record order.");
			}
		}
	}

	/** Returns this publication's exact requested Section rectangle. */
	const BuildingCollectionRegion& BuildingCandidatePublication::SectionRegion() const noexcept
	{
		return sectionRegion_;
	}

	/** Returns the collector-supplied Section hash without recomputing it. */
	std::uint64_t BuildingCandidatePublication::ContentHash() const noexcept
	{
		return contentHash_;
	}

	/** Borrows Section-local occurrences while this publication remains alive. */
	BuildingCandidateData BuildingCandidatePublication::BorrowCandidateData() const noexcept
	{
		return {std::span<const BuildingCandidateOccurrence>(occurrences_)};
	}

	/** Borrows the index-aligned source records while this publication remains alive. */
	BuildingCandidateSourceData BuildingCandidatePublication::BorrowSourceData() const noexcept
	{
		return {std::span<const BuildingCandidateSourceRecord>(sourceRecords_)};
	}

	/** Resolves a Section-local ordinal directly into the aligned owned source vector. */
	bool BuildingCandidatePublication::TryGetSourceRecord(
		std::uint64_t sectionOrdinal,
		const BuildingCandidateSourceRecord*& output) const noexcept
	{
		output = nullptr;
		if (sectionOrdinal >= sourceRecords_.size())
		{
			return false;
		}

		output = &sourceRecords_[static_cast<std::size_t>(sectionOrdinal)];
		return true;
	}

	/** Accepts only finite positive rectangles whose computed edges remain finite and distinct. */
	bool BuildingCandidatePublication::IsValidRegion(const BuildingCollectionRegion& region) noexcept
	{
		const double right = region.xMapUnits + region.widthMapUnits;
		const double bottom = region.yMapUnits + region.heightMapUnits;
		return std::isfinite(region.xMapUnits) && std::isfinite(region.yMapUnits) &&
			std::isfinite(region.widthMapUnits) && std::isfinite(region.heightMapUnits) &&
			region.widthMapUnits > 0.0 && region.heightMapUnits > 0.0 &&
			std::isfinite(right) && std::isfinite(bottom) &&
			right > region.xMapUnits && bottom > region.yMapUnits;
	}
}

#endif // MODLOADER_CLIENT_BUILD
