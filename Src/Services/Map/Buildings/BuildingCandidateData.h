// Defines neutral candidate evidence and per-occurrence source values retained by one publication.
#pragma once

#include "BuildingMapRegion.h"
#include "Visuals/SpriteGeometry.h"
#include "Visuals/SplineGeometry.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

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

	/** Identifies the neutral source shape known for one candidate occurrence. */
	enum class BuildingCandidateSourceKind : std::uint8_t
	{
		Unknown,
		Sprite,
		Spline
	};

	/** Records neutral source acquisition without implying resource or render readiness. */
	enum class BuildingCandidateSourceStatus : std::uint8_t
	{
		Unknown,
		Incomplete,
		Unavailable,
		Invalid,
		Unsupported,
		Complete
	};

	/** Describes the neutral sprite drawing pass without assigning game-specific policy meaning. */
	enum class BuildingCandidateSpritePass : std::uint8_t
	{
		/** Pass classification is unresolved and authorizes no default rendering pass. */
		Unknown,
		/** Ordinary sprites; eligible successful submissions may contribute spline occluders. */
		FirstPass,
		/** Sprites drawn after spline surfaces; never included in the spline occlusion index. */
		SecondPass
	};

	/** Identifies the immutable source used to produce one neutral keyed spline curve. */
	enum class BuildingCandidateSplineProvenance : std::uint8_t
	{
		Unknown,
		InitializedCurve,
		FragmentPoints,
		PayloadFallback
	};

	/** Describes the coordinate basis of the retained keyed spline values. */
	enum class BuildingCandidateSplineCoordinateSpace : std::uint8_t
	{
		Unknown,
		WorldSpace,
		LocalSpace
	};

	/** Retains fallback spline values when the source has no supported implicit key interval. */
	struct BuildingCandidateUnkeyedSplinePoint
	{
		/** XYZ source position in original point order and the enclosing curve's coordinate space. */
		std::array<double, 3> position{};
		/** Source derivative/tangent paired with this point in the same coordinate space. */
		std::array<double, 3> tangent{};
	};

	/** Owns one neutral keyed spline curve and preserves its source interpretation evidence. */
	struct BuildingCandidateSplineSource
	{
		/** Ordered neutral keys and derivatives copied from the acquired source. */
		std::vector<Visuals::KeyedSplinePoint> points;
		/** Ordered original fallback values when no valid two-point key mapping exists. */
		std::vector<BuildingCandidateUnkeyedSplinePoint> unkeyedPoints;
		/** Source provenance; fallback values use the established implicit [0, 1] interval. */
		BuildingCandidateSplineProvenance provenance = BuildingCandidateSplineProvenance::Unknown;
		/** Coordinate basis retained without applying an additional placement transform. */
		BuildingCandidateSplineCoordinateSpace coordinateSpace =
			BuildingCandidateSplineCoordinateSpace::Unknown;
		/** Preserves loop evidence for explicit rejection by later numerical conversion. */
		bool isLooped = false;
		/** Distinguishes an acquired keyed curve from unavailable or unsupported spline data. */
		bool hasKeyedCurve = false;
	};

	/** Owns immutable, game-independent source values for one candidate occurrence.
	 * Records are positionally aligned with the publication's candidate occurrences. Empty
	 * identity or absent placement remains distinguishable through Kind and Status; neither
	 * means that the occurrence is absent from a Section. Complete means source values were
	 * copied, not that resources, spline profiles, prepared geometry or rendering are ready.
	 */
	struct BuildingCandidateSourceRecord
	{
		/** Known neutral source shape, or Unknown while source classification is unresolved. */
		BuildingCandidateSourceKind kind = BuildingCandidateSourceKind::Unknown;
		/** Sprite pass supplied by the game-specific producer; Unknown authorizes no default pass. */
		BuildingCandidateSpritePass spritePass = BuildingCandidateSpritePass::Unknown;
		/** Neutral direct policy-table index for a positively classified spline; zero is unresolved. */
		std::uint16_t splinePolicyIndex = 0;
		/** Acquisition state of the retained neutral values, independent of resource readiness. */
		BuildingCandidateSourceStatus status = BuildingCandidateSourceStatus::Unknown;
		/** Owned canonical generated-class identity bytes when acquired. */
		std::string canonicalClassPath;
		/** Whether the full neutral world placement is available. */
		bool hasPlacement = false;
		/** World position, XYZW rotation and signed scale when hasPlacement is true. */
		Visuals::SpritePlacement placement{};
		/** Owned keyed values and source provenance for known spline occurrences. */
		BuildingCandidateSplineSource spline;
	};

	/** Borrows the publication-owned source records in candidate occurrence order. */
	struct BuildingCandidateSourceData
	{
		/** Record N corresponds to candidate occurrence N in the same retained publication. */
		std::span<const BuildingCandidateSourceRecord> records;
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
