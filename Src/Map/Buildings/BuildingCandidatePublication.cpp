#if defined(MODLOADER_CLIENT_BUILD)

// Implements retained StarRupture candidate evidence without native or rendering work.
#include "BuildingCandidatePublication.h"

#include <bit>
#include <cmath>
#include <limits>
#include <string_view>
#include <utility>

namespace MiniMap::BuildingAcquisition
{
	using ::Services::Map::Buildings::BuildingCandidateOccurrence;
	using ::Services::Map::Buildings::BuildingCandidateObservation;
	using ::Services::Map::Buildings::BuildingCandidateData;
	using ::Services::Map::Buildings::BuildingCollectionRegion;
	using ::Services::Map::Buildings::BuildingSectionGrid;

	/** Builds occurrence descriptors and freezes completeness over one immutable facts owner. */
	BuildingCandidatePublication::BuildingCandidatePublication(
		std::shared_ptr<const BuildingAcquiredFacts> factsOwner,
		std::weak_ptr<const std::atomic<bool>> eligibility)
		: factsOwner_(std::move(factsOwner))
		, eligibility_(std::move(eligibility))
	{
		BuildOccurrences();
		candidateSignature_ = ComputeCandidateSignature();
	}

	/** Reads only the independent token, so the query remains safe after collector destruction. */
	bool BuildingCandidatePublication::IsCurrent() const noexcept
	{
		const auto eligibility = eligibility_.lock();
		return factsOwner_ != nullptr && hasRequestedCoverage_ && eligibility != nullptr &&
			eligibility->load(std::memory_order_acquire);
	}

	/** Borrows descriptor and coverage storage owned by this retained publication. */
	BuildingCandidateData BuildingCandidatePublication::BorrowCandidateData() const noexcept
	{
		BuildingCandidateData data;
		data.hasRequestedCoverage = hasRequestedCoverage_;
		data.requestedCoverage = requestedCoverage_;
		data.hasCoverage = hasAcquiredCoverage_;
		data.coverage = hasAcquiredCoverage_ ? requestedCoverage_ : BuildingCollectionRegion{};
		data.evidence = evidence_;
		data.occurrences = std::span<const BuildingCandidateOccurrence>(occurrences_);
		return data;
	}

	/** Initializes every output first, then fills only valid row-major cells. */
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

	/** Keeps future adaptation tied to this publication's exact immutable fact owner. */
	std::shared_ptr<const BuildingAcquiredFacts>
	BuildingCandidatePublication::GetAcquiredFactsOwner() const noexcept
	{
		return factsOwner_;
	}

	/** Resolves the contiguous publication ordinal without exposing native Index/Serial in neutral candidate data. */
	const BuildingAcquiredFacts::Building*
	BuildingCandidatePublication::FindBuildingFacts(
		std::uint64_t publicationOrdinal) const noexcept
	{
		if (factsOwner_ == nullptr || publicationOrdinal >= factsOwner_->Buildings.size())
		{
			return nullptr;
		}
		return &factsOwner_->Buildings[static_cast<std::size_t>(publicationOrdinal)];
	}

	/** Creates one ordinal per source occurrence and keeps independent completeness evidence. */
	void BuildingCandidatePublication::BuildOccurrences()
	{
		if (factsOwner_ == nullptr)
		{
			return;
		}
		const BuildingAcquiredFacts& facts = *factsOwner_;
		hasRequestedCoverage_ = facts.HasRequestedRegion && IsValidRegion(facts.RequestedRegion);
		if (hasRequestedCoverage_)
		{
			requestedCoverage_ = ToMapRegion(facts.RequestedRegion);
		}
		evidence_.membershipEnumerationComplete = facts.MembershipComplete;
		evidence_.candidateCoverageComplete = hasRequestedCoverage_ && facts.MembershipComplete;
		evidence_.nativeAcquisitionComplete = facts.AcquisitionComplete;
		hasAcquiredCoverage_ = evidence_.candidateCoverageComplete;
		occurrences_.reserve(facts.Buildings.size());
		ordinals_.reserve(facts.Buildings.size());
		for (std::size_t index = 0; index < facts.Buildings.size(); ++index)
		{
			const BuildingAcquiredFacts::Building& building = facts.Buildings[index];
			BuildingCandidateOccurrence occurrence;
			occurrence.publicationOrdinal = static_cast<std::uint64_t>(index);
			occurrence.hasMapPosition = HasValidMapPosition(building);
			if (occurrence.hasMapPosition)
			{
				occurrence.xMapUnits = building.Position[0];
				occurrence.yMapUnits = building.Position[1];
			}
			occurrence.nativeFactsComplete = building.Complete;
			occurrence.isPendingOrUnresolved = !building.Complete;
			evidence_.hasPendingOrUnresolvedOccurrences |= occurrence.isPendingOrUnresolved;
			occurrences_.push_back(occurrence);
			ordinals_.push_back(occurrence.publicationOrdinal);
		}
		evidence_.nativeAcquisitionComplete &= !evidence_.hasPendingOrUnresolvedOccurrences;
	}

	/** Uses order-independent wrapping Sum/Xor/Count over FNV-1a placement-content values. */
	std::uint64_t BuildingCandidatePublication::ComputeCandidateSignature() const noexcept
	{
		if (factsOwner_ == nullptr)
		{
			return FinalizeAggregate(0);
		}
		std::uint64_t sum = 0;
		std::uint64_t exclusiveOr = 0;
		for (const BuildingAcquiredFacts::Building& building : factsOwner_->Buildings)
		{
			const std::uint64_t occurrenceSignature = ComputeOccurrenceSignature(building);
			sum += occurrenceSignature;
			exclusiveOr ^= occurrenceSignature;
		}
		const std::uint64_t count = static_cast<std::uint64_t>(factsOwner_->Buildings.size());
		const std::uint64_t rotatedXor = (exclusiveOr << 23) | (exclusiveOr >> 41);
		return FinalizeAggregate(sum ^ rotatedXor ^ (count * 0x9e3779b97f4a7c15ULL));
	}

	/** Hashes canonical class path, world position and canonical quaternion, excluding handles and geometry. */
	std::uint64_t BuildingCandidatePublication::ComputeOccurrenceSignature(
		const BuildingAcquiredFacts::Building& building) noexcept
	{
		std::uint64_t hash = 14695981039346656037ULL;
		std::string_view classPath;
		if (building.DefinitionFacts != nullptr)
		{
			classPath = building.DefinitionFacts->CanonicalClassPath;
		}
		AppendHashInteger(hash, static_cast<std::uint64_t>(classPath.size()));
		for (unsigned char value : classPath)
		{
			AppendHashByte(hash, value);
		}
		if (building.TransformStatus == BuildingAcquiredFacts::Status::Ready)
		{
			for (double value : building.Position)
			{
				AppendHashDouble(hash, value);
			}
			AppendCanonicalQuaternion(hash, building.Rotation);
		}
		return hash;
	}

	/** Applies the FNV-1a byte step used by the approved placement-content policy. */
	void BuildingCandidatePublication::AppendHashByte(
		std::uint64_t& hash,
		std::uint8_t value) noexcept
	{
		hash = (hash ^ value) * 1099511628211ULL;
	}

	/** Hashes integer fields in a platform-independent byte order. */
	void BuildingCandidatePublication::AppendHashInteger(
		std::uint64_t& hash,
		std::uint64_t value) noexcept
	{
		for (std::size_t byteIndex = 0; byteIndex < sizeof(value); ++byteIndex)
		{
			AppendHashByte(hash, static_cast<std::uint8_t>(value & 0xff));
			value >>= 8;
		}
	}

	/** Canonicalizes signed zero before hashing floating-point placement content. */
	void BuildingCandidatePublication::AppendHashDouble(
		std::uint64_t& hash,
		double value) noexcept
	{
		if (value == 0.0)
		{
			value = 0.0;
		}
		AppendHashInteger(hash, std::bit_cast<std::uint64_t>(value));
	}

	/** Rejects invalid dimensions, overflowed cardinality and unsupported coordinate extents. */
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

	/** Computes one requested cell and rejects non-finite or collapsed half-open bounds. */
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

	/** Tests full half-open cell containment in the acquired request coverage. */
	bool BuildingCandidatePublication::IsCellCovered(
		const BuildingCollectionRegion& region) const noexcept
	{
		if (!hasAcquiredCoverage_)
		{
			return false;
		}
		const double requestedRight = requestedCoverage_.xMapUnits + requestedCoverage_.widthMapUnits;
		const double requestedBottom = requestedCoverage_.yMapUnits + requestedCoverage_.heightMapUnits;
		const double cellRight = region.xMapUnits + region.widthMapUnits;
		const double cellBottom = region.yMapUnits + region.heightMapUnits;
		return region.xMapUnits >= requestedCoverage_.xMapUnits &&
			region.yMapUnits >= requestedCoverage_.yMapUnits &&
			cellRight <= requestedRight && cellBottom <= requestedBottom;
	}

	/** Fills valid cells with the publication-owned multiset and conservative evidence. */
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

	/** Validates finite positive native-centimetre request geometry before exposing it neutrally. */
	bool BuildingCandidatePublication::IsValidRegion(
		const BuildingAcquiredFacts::Region& region) noexcept
	{
		const double right = region.X + region.Width;
		const double bottom = region.Y + region.Height;
		return std::isfinite(region.X) && std::isfinite(region.Y) &&
			std::isfinite(region.Width) && std::isfinite(region.Height) &&
			region.Width > 0.0 && region.Height > 0.0 &&
			std::isfinite(right) && std::isfinite(bottom) &&
			right > region.X && bottom > region.Y &&
			std::isfinite(std::hypot(region.Width, region.Height));
	}

	/** Copies the native-centimetre rectangle directly into the neutral map-unit fields. */
	BuildingCollectionRegion BuildingCandidatePublication::ToMapRegion(
		const BuildingAcquiredFacts::Region& region) noexcept
	{
		return {region.X, region.Y, region.Width, region.Height};
	}

	/** Confirms a transform-ready fact has finite native X/Y values before publishing its position. */
	bool BuildingCandidatePublication::HasValidMapPosition(
		const BuildingAcquiredFacts::Building& building) noexcept
	{
		return building.TransformStatus == BuildingAcquiredFacts::Status::Ready &&
			std::isfinite(building.Position[0]) && std::isfinite(building.Position[1]);
	}

	/** Canonicalizes quaternion sign so equivalent q and -q placements hash identically. */
	void BuildingCandidatePublication::AppendCanonicalQuaternion(
		std::uint64_t& hash,
		const BuildingAcquiredFacts::Quaternion& rotation) noexcept
	{
		bool negate = false;
		const std::size_t signCheckOrder[] = {3, 0, 1, 2};
		for (std::size_t componentIndex : signCheckOrder)
		{
			const double value = rotation[componentIndex];
			if (value != 0.0)
			{
				negate = std::signbit(value);
				break;
			}
		}
		for (double value : rotation)
		{
			AppendHashDouble(hash, negate ? -value : value);
		}
	}

	/** Applies the existing 64-bit avalanche finalization to aggregate multiset evidence. */
	std::uint64_t BuildingCandidatePublication::FinalizeAggregate(
		std::uint64_t value) noexcept
	{
		value ^= value >> 30;
		value *= 0xbf58476d1ce4e5b9ULL;
		value ^= value >> 27;
		value *= 0x94d049bb133111ebULL;
		value ^= value >> 31;
		return value;
	}
}

#endif // MODLOADER_CLIENT_BUILD
