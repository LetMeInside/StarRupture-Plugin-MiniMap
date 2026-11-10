// Owns one Section's immutable neutral candidate occurrences and aligned source records.
#pragma once

#include "BuildingCandidateData.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Services::Map::Buildings
{
	/** Owns the immutable candidate/source pairs selected for one exact Section.
	 * All vectors are owned by this publication. Borrowed spans and source pointers remain valid
	 * only while this publication owner remains alive. Nested source CPU values retain their own
	 * immutable value/shared-value ownership and no collector dataset or other publication is kept.
	 */
	class BuildingCandidatePublication final
	{
	public:
		/** Takes owned Section coordinates, aligned candidate/source vectors and collector hash.
		 * Construction rejects invalid rectangle geometry, mismatched vector sizes, or any
		 * candidate whose publicationOrdinal differs from its Section-local vector index.
		 * @param sectionRegion Exact half-open Section rectangle associated with this publication.
		 * @param occurrences Owned candidate occurrences in Section-local order.
		 * @param sourceRecords Owned source records, with record N corresponding to occurrence N.
		 * @param contentHash Collector-supplied Section content hash; every 64-bit value is valid.
		 * @throws std::invalid_argument when the rectangle or candidate/source correspondence is invalid.
		 */
		BuildingCandidatePublication(
			BuildingCollectionRegion sectionRegion,
			std::vector<BuildingCandidateOccurrence> occurrences,
			std::vector<BuildingCandidateSourceRecord> sourceRecords,
			std::uint64_t contentHash);

		/** Returns the exact Section rectangle associated with this immutable publication. */
		const BuildingCollectionRegion& SectionRegion() const noexcept;

		/** Returns the collector-supplied Section content hash unchanged; zero is valid. */
		std::uint64_t ContentHash() const noexcept;

		/** Borrows Section-local candidate occurrences while this publication owner is retained. */
		BuildingCandidateData BorrowCandidateData() const noexcept;

		/** Borrows source records aligned index-for-index with the candidate occurrences. */
		BuildingCandidateSourceData BorrowSourceData() const noexcept;

		/** Resolves one publication-local ordinal to its borrowed immutable source record.
		 * @param sectionOrdinal Section-local ordinal of the matching candidate occurrence.
		 * @param output Receives a publication-owned record pointer on success, otherwise null.
		 * @return True when the ordinal addresses a source record in this Section.
		 */
		bool TryGetSourceRecord(
			std::uint64_t sectionOrdinal,
			const BuildingCandidateSourceRecord*& output) const noexcept;

	private:
		/** Validates finite, positive geometry with representable non-collapsed edges. */
		static bool IsValidRegion(const BuildingCollectionRegion& region) noexcept;

		BuildingCollectionRegion sectionRegion_{};
		std::vector<BuildingCandidateOccurrence> occurrences_;
		std::vector<BuildingCandidateSourceRecord> sourceRecords_;
		std::uint64_t contentHash_ = 0;
	};
}
