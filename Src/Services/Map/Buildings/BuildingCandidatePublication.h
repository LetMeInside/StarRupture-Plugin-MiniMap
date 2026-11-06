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
	/** Retains immutable neutral candidates and independently revocable publication eligibility.
	 * The object owns all occurrence and ordinal storage borrowed by its output views. Historical
	 * data remains readable after the producer revokes the weak eligibility token.
	 */
	class BuildingCandidatePublication final
	{
	public:
		/** Takes ownership of complete producer records and explicit coverage evidence.
		 * Ordinals are reassigned contiguously in input order. The weak token is observed for
		 * currentness only; the producer retains control of its lifetime and revocation.
		 * @param occurrences Neutral candidate records, preserving one entry per occurrence.
		 * @param evidence Producer-certified enumeration, coverage and acquisition evidence.
		 * @param hasRequestedCoverage Whether an accepted request rectangle is supplied.
		 * @param requestedCoverage The exact accepted request rectangle.
		 * @param hasCoverage Whether the producer certifies acquired coverage for the rectangle.
		 * @param coverage The producer-certified acquired rectangle.
		 * @param eligibility Producer-owned independently revocable eligibility token.
		 */
		BuildingCandidatePublication(
			std::vector<BuildingCandidateOccurrence> occurrences,
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
