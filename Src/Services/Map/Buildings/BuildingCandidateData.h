// Defines neutral Section candidate occurrences and immutable per-occurrence source records.
#pragma once

#include "BuildingMapRegion.h"
#include "SplineProfile.h"
#include "Visuals/SpriteGeometry.h"
#include "Visuals/SplineGeometry.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Services::Map::Buildings
{
	/** Describes one potential contributor occurrence without collapsing repeated content.
	 * Its ordinal is local to one Section publication and is not a persistent ID.
	 */
	struct BuildingCandidateOccurrence
	{
		/** Section-local ordinal directly indexing the aligned source record. */
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

	/** Selects how a neutral spline source supplies its surface geometry. */
	enum class BuildingCandidateSplineGeometryMode : std::uint8_t
	{
		/** Uses only acquired authored profile alternatives. */
		AuthoredProfiles,
		/** Uses one explicitly requested synthetic thin ribbon. */
		SyntheticThinRibbon
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
		/** Geometry intent is explicit; profile NotApplicable alone does not imply a ribbon. */
		BuildingCandidateSplineGeometryMode splineGeometryMode =
			BuildingCandidateSplineGeometryMode::AuthoredProfiles;
		/** Synthetic ribbon half-width in map units; used only by SyntheticThinRibbon. */
		double syntheticThinRibbonHalfWidthMapUnits = 0.0;
		/** Acquired ordered geometry alternatives shared by occurrences of one definition. */
		std::shared_ptr<const SplineProfileSet> splineProfiles;
		/** Acquisition state remains separate from curve completeness and renderability. */
		SplineProfileAcquisitionStatus splineProfileStatus = SplineProfileAcquisitionStatus::Unknown;
	};

	/** Borrows Section-local candidate occurrences owned by a publication. */
	struct BuildingCandidateData
	{
		/** All Section contributors, including unresolved and repeated occurrences. */
		std::span<const BuildingCandidateOccurrence> occurrences;
	};

	/** Borrows the publication-owned source records in candidate occurrence order. */
	struct BuildingCandidateSourceData
	{
		/** Record N corresponds to candidate occurrence N in the same Section publication. */
		std::span<const BuildingCandidateSourceRecord> records;
	};

}
