// Owns neutral candidate occurrences and provides conservative borrowed observations.
#pragma once

#include "BuildingCandidateData.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Services::Map::Buildings
{
	/** Retains immutable neutral candidates and matching source records under one eligibility token.
	 * The object owns all occurrence, source and ordinal storage borrowed by its output views.
	 * Historical data remains readable after the producer revokes the weak eligibility token.
	 */
	class BuildingCandidatePublication final
	{
	public:
		/** Takes ownership of candidate/source records and explicit coverage evidence.
		 * Ordinals are reassigned contiguously in input order. The weak token is observed for
		 * currentness only; the producer retains control of its lifetime and revocation. Source
		 * records must have exactly one entry per occurrence and remain in the same input order.
		 * A cardinality mismatch throws std::invalid_argument before the object can be published.
		 * @param occurrences Neutral candidate records, preserving one entry per occurrence.
		 * @param sourceRecords Matching neutral source records, preserving occurrence order.
		 * @param evidence Producer-certified enumeration, coverage and acquisition evidence.
		 * @param hasRequestedCoverage Whether an accepted request rectangle is supplied.
		 * @param requestedCoverage The exact accepted request rectangle.
		 * @param hasCoverage Whether the producer certifies acquired coverage for the rectangle.
		 * @param coverage The producer-certified acquired rectangle.
		 * @param eligibility Producer-owned independently revocable eligibility token.
		 */
		BuildingCandidatePublication(
			std::vector<BuildingCandidateOccurrence> occurrences,
			std::vector<BuildingCandidateSourceRecord> sourceRecords,
			BuildingCandidateEvidence evidence,
			bool hasRequestedCoverage,
			BuildingCollectionRegion requestedCoverage,
			bool hasCoverage,
			BuildingCollectionRegion coverage,
			std::weak_ptr<const std::atomic<bool>> eligibility);

		/** Reports current eligibility without changing historical readability. */
		bool IsCurrent() const noexcept;

		/** Borrows immutable occurrence and coverage data while this owner is retained. */
		BuildingCandidateData BorrowCandidateData() const noexcept;

		/** Borrows immutable source records aligned with candidate occurrence order. */
		BuildingCandidateSourceData BorrowSourceData() const noexcept;

		/** Resolves one publication-local ordinal to its borrowed immutable source record.
		 * @param publicationOrdinal Ordinal assigned to the matching candidate occurrence.
		 * @param output Receives a publication-owned record pointer on success, otherwise null.
		 * @return True when the ordinal addresses a source record in this publication.
		 */
		bool TryGetSourceRecord(
			std::uint64_t publicationOrdinal,
			const BuildingCandidateSourceRecord*& output) const noexcept;

		/** Fills valid row-major cells with every candidate ordinal, without spatial filtering. */
		void ObserveCandidates(
			const BuildingSectionGrid& grid,
			std::span<BuildingCandidateObservation> observations) const;

	private:
		/** Assigns publication ordinals and builds the existing order-independent signature. */
		void InitializeOccurrences();

		/** Finalizes wrapping Sum/Xor/Count aggregation of producer-supplied signatures. */
		static std::uint64_t ComputeCandidateSignature(
			std::span<const BuildingCandidateOccurrence> occurrences) noexcept;

		/** Applies the existing SplitMix64 finalization to the aggregate value. */
		static std::uint64_t FinalizeAggregate(std::uint64_t value) noexcept;

		/** Validates grid geometry and exact output cardinality before observation. */
		static bool IsSupportedGrid(
			const BuildingSectionGrid& grid,
			std::size_t outputCount) noexcept;

		/** Computes one exact half-open cell and rejects collapsed floating geometry. */
		static bool TryCreateCellRegion(
			const BuildingSectionGrid& grid,
			std::size_t column,
			std::size_t row,
			BuildingCollectionRegion& region) noexcept;

		/** Checks whether the complete requested cell lies within producer-certified coverage. */
		bool IsCellCovered(const BuildingCollectionRegion& region) const noexcept;

		/** Validates finite positive rectangle geometry before retaining producer coverage. */
		static bool IsValidRegion(const BuildingCollectionRegion& region) noexcept;

		/** Fills valid cells in row-major order with owned ordinals and conservative evidence. */
		void FillObservations(
			const BuildingSectionGrid& grid,
			std::span<BuildingCandidateObservation> observations) const;

		std::vector<BuildingCandidateOccurrence> occurrences_;
		std::vector<BuildingCandidateSourceRecord> sourceRecords_;
		std::vector<std::uint64_t> ordinals_;
		BuildingCandidateEvidence evidence_{};
		std::weak_ptr<const std::atomic<bool>> eligibility_;
		BuildingCollectionRegion requestedCoverage_{};
		BuildingCollectionRegion coverage_{};
		bool hasRequestedCoverage_ = false;
		bool hasCoverage_ = false;
		bool hasValidRequestedCoverage_ = false;
		std::uint64_t candidateSignature_ = 0;
	};
}
