// Defines neutral candidate evidence retained by one immutable acquisition publication.
#pragma once

#include "BuildingMapRegion.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace Services::Map::Buildings
{
	/** Describes square half-open cells in row-major order, indexed row * columnCount + column. */
	struct BuildingSectionGrid
	{
		/** Upper-left X coordinate of the first cell. */
		double xMapUnits = 0.0;
		/** Upper-left Y coordinate of the first cell. */
		double yMapUnits = 0.0;
		/** Positive width and height of each square cell. */
		double sectionSizeMapUnits = 0.0;
		/** Number of cells in each rightward row. */
		std::size_t columnCount = 0;
		/** Number of cells in each downward column. */
		std::size_t rowCount = 0;
	};

	/** Describes one potential contributor occurrence without collapsing repeated content.
	 * The ordinal is unique only within its retained publication and is not a persistent ID.
	 */
	struct BuildingCandidateOccurrence
	{
		/** Publication-scoped ordinal for correlating neutral records from this transaction. */
		std::uint64_t publicationOrdinal = 0;
		/** Whether X/Y identify a conservatively useful map position. */
		bool hasMapPosition = false;
		/** Map X coordinate; ignored when hasMapPosition is false. */
		double xMapUnits = 0.0;
		/** Map Y coordinate; ignored when hasMapPosition is false. */
		double yMapUnits = 0.0;
		/** Whether the occurrence's required native facts have been acquired. */
		bool nativeFactsComplete = false;
		/** Whether this occurrence is pending or otherwise unresolved. */
		bool isPendingOrUnresolved = false;
		/** Producer-supplied placement-content signature used only for multiset aggregation. */
		std::uint64_t contentSignature = 0;
	};

	/** Reports candidate accounting separately from native fact readiness. */
	struct BuildingCandidateEvidence
	{
		/** Whether an authoritative source enumeration completed successfully. */
		bool membershipEnumerationComplete = false;
		/** Whether candidate coverage for the requested area is complete. */
		bool candidateCoverageComplete = false;
		/** Whether all represented candidates have complete native acquired facts. */
		bool nativeAcquisitionComplete = false;
		/** Whether any pending or unresolved occurrences are present. */
		bool hasPendingOrUnresolvedOccurrences = false;
	};

	/** Borrows immutable candidate occurrences and acquired coverage from its owner.
	 * hasCoverage=false means unknown coverage and never means an empty certified area.
	 */
	struct BuildingCandidateData
	{
		/** Whether an exact accepted acquisition request is retained by this publication. */
		bool hasRequestedCoverage = false;
		/** Exact acquisition request, ignored when hasRequestedCoverage is false. */
		BuildingCollectionRegion requestedCoverage{};
		/** Whether coverage describes a finite positive acquired map-space region. */
		bool hasCoverage = false;
		/** Acquired coverage, ignored when hasCoverage is false. */
		BuildingCollectionRegion coverage{};
		/** Independent completeness and uncertainty facts for the retained source. */
		BuildingCandidateEvidence evidence{};
		/** All retained occurrences, including unresolved and repeated occurrences. */
		std::span<const BuildingCandidateOccurrence> occurrences;
	};

	/** Reports candidate evidence for one requested half-open section cell. */
	struct BuildingCandidateObservation
	{
		/** Exact half-open cell requested by the caller. */
		BuildingCollectionRegion requestedRegion{};
		/** Completeness and unresolved evidence for this cell's conservative candidates. */
		BuildingCandidateEvidence evidence{};
		/** Publication-owned occurrence ordinals relevant to this cell; multiplicity is preserved. */
		std::span<const std::uint64_t> candidateOrdinals;
		/** Deterministic candidate-set signature; zero is valid and not a final fingerprint. */
		std::uint64_t candidateSignature = 0;
	};
}
