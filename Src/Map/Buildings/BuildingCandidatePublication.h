// Retains one immutable StarRupture fact owner behind neutral candidate observations.
#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include "BuildingAcquiredFacts.h"
#include "Services/Map/Buildings/IBuildingCandidatePublication.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace MiniMap::BuildingAcquisition
{
	/** Owns candidate descriptors and retains the exact facts owner from V2 publication.
	 * It has no collector/native dependency; eligibility is a weak independently revocable token.
	 */
	class BuildingCandidatePublication final :
		public ::Services::Map::Buildings::IBuildingCandidatePublication
	{
	public:
		/** Freezes neutral occurrences over one immutable facts owner and eligibility token.
		 * @param factsOwner Exact V2 fact publication associated with every ordinal.
		 * @param eligibility State-owned token revoked when these facts become historical.
		 */
		BuildingCandidatePublication(
			std::shared_ptr<const BuildingAcquiredFacts> factsOwner,
			std::weak_ptr<const std::atomic<bool>> eligibility);

		/** Reports fixed-publication eligibility without accessing V2 or native state. */
		bool IsCurrent() const noexcept override;

		/** Borrows immutable occurrence descriptors while the publication owner is retained. */
		::Services::Map::Buildings::BuildingCandidateView BorrowCandidateView() const noexcept override;

		/** Fills row-major observations without acquisition or temporary borrowed spans. */
		void ObserveCandidates(
			const ::Services::Map::Buildings::BuildingSectionGrid& grid,
			std::span<::Services::Map::Buildings::BuildingCandidateObservation> observations) const override;

		/** Returns the exact immutable fact owner associated with this candidate publication. */
		std::shared_ptr<const BuildingAcquiredFacts> GetAcquiredFactsOwner() const noexcept;

		/** Resolves a publication-scoped ordinal to its exact authoritative occurrence. */
		const BuildingAcquiredFacts::Building* FindBuildingFacts(
			std::uint64_t publicationOrdinal) const noexcept;

	private:
		/** Builds one neutral occurrence for every fact entry, preserving order and multiplicity. */
		void BuildOccurrences();

		/** Computes the approved placement-content signature as an order-independent multiset. */
		std::uint64_t ComputeCandidateSignature() const noexcept;

		/** Fills valid cells with the retained ordinal multiset and cell-specific coverage evidence. */
		void FillObservations(
			const ::Services::Map::Buildings::BuildingSectionGrid& grid,
			std::span<::Services::Map::Buildings::BuildingCandidateObservation> observations) const;

		/** Hashes available definition and placement content without using native handles. */
		static std::uint64_t ComputeOccurrenceSignature(
			const BuildingAcquiredFacts::Building& building) noexcept;

		/** Finalizes the order-independent aggregate with the approved SplitMix64 finalizer. */
		static std::uint64_t FinalizeAggregate(std::uint64_t value) noexcept;

		/** Appends canonical quaternion components so q and negative q have equal content hashes. */
		static void AppendCanonicalQuaternion(
			std::uint64_t& hash,
			const BuildingAcquiredFacts::Quaternion& rotation) noexcept;

		/** Appends one byte to the stable 64-bit FNV-1a value. */
		static void AppendHashByte(std::uint64_t& hash, std::uint8_t value) noexcept;

		/** Appends an unsigned integer in a fixed little-endian byte order. */
		static void AppendHashInteger(std::uint64_t& hash, std::uint64_t value) noexcept;

		/** Appends finite floating content while canonicalizing signed zero. */
		static void AppendHashDouble(std::uint64_t& hash, double value) noexcept;

		/** Validates a grid and output cardinality before any cell can be certified. */
		static bool IsSupportedGrid(
			const ::Services::Map::Buildings::BuildingSectionGrid& grid,
			std::size_t outputCount) noexcept;

		/** Calculates one exact half-open cell and rejects collapsed floating geometry. */
		static bool TryCreateCellRegion(
			const ::Services::Map::Buildings::BuildingSectionGrid& grid,
			std::size_t column,
			std::size_t row,
			::Services::Map::Buildings::BuildingCollectionRegion& region) noexcept;

		/** Requires a requested cell to lie fully inside successfully acquired coverage. */
		bool IsCellCovered(
			const ::Services::Map::Buildings::BuildingCollectionRegion& region) const noexcept;

		/** Validates the exact native-centimetre request copied into immutable facts. */
		static bool IsValidRegion(const BuildingAcquiredFacts::Region& region) noexcept;

		/** Converts native centimetres directly to the game-selected neutral map units. */
		static ::Services::Map::Buildings::BuildingCollectionRegion ToMapRegion(
			const BuildingAcquiredFacts::Region& region) noexcept;

		/** Requires transform-complete, finite position evidence before exposing map X/Y. */
		static bool HasValidMapPosition(const BuildingAcquiredFacts::Building& building) noexcept;

		std::shared_ptr<const BuildingAcquiredFacts> factsOwner_;
		std::weak_ptr<const std::atomic<bool>> eligibility_;
		std::vector<::Services::Map::Buildings::BuildingCandidateOccurrence> occurrences_;
		std::vector<std::uint64_t> ordinals_;
		::Services::Map::Buildings::BuildingCollectionRegion requestedCoverage_{};
		::Services::Map::Buildings::BuildingCandidateEvidence evidence_{};
		bool hasRequestedCoverage_ = false;
		bool hasAcquiredCoverage_ = false;
		std::uint64_t candidateSignature_ = 0;
	};
}

#endif // MODLOADER_CLIENT_BUILD
