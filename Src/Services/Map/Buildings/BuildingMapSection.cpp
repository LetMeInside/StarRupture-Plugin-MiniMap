#if defined(MODLOADER_CLIENT_BUILD)

// Implements Section identity, membership observation tracking, and lazy
// ownership of private numerical rendering context.
#include "BuildingMapSection.h"

#include "Visuals/BuildingVisualsContext.h"

#include <memory>

namespace Services::Map::Buildings
{
	/** Stores the fixed integer coordinate assigned by the owning service. */
	BuildingMapSection::BuildingMapSection(std::int64_t gridX, std::int64_t gridY)
		: gridX_(gridX)
		, gridY_(gridY)
	{
	}

	/** Destroys the Section-owned numerical context with its service-owned Section. */
	BuildingMapSection::~BuildingMapSection() = default;

	/** Returns this section's world-grid X coordinate. */
	std::int64_t BuildingMapSection::GetGridX() const
	{
		return gridX_;
	}

	/** Returns this section's world-grid Y coordinate. */
	std::int64_t BuildingMapSection::GetGridY() const
	{
		return gridY_;
	}

	/** Reports whether the most recent observation certifies current membership. */
	bool BuildingMapSection::HasCurrentMembershipObservation() const
	{
		return hasCurrentMembershipObservation_;
	}

	/** Returns the current fingerprint only while a complete observation is certified. */
	bool BuildingMapSection::TryGetCurrentMembershipFingerprint(std::uint64_t& fingerprint) const
	{
		if (!hasCurrentMembershipObservation_)
		{
			return false;
		}
		fingerprint = membershipFingerprint_;
		return true;
	}

	/** Tracks complete observations and reports first certification or fingerprint changes. */
	bool BuildingMapSection::RecordMembershipObservation(
		bool isComplete,
		std::uint64_t membershipFingerprint)
	{
		if (!isComplete)
		{
			hasCurrentMembershipObservation_ = false;
			return false;
		}
		const bool fingerprintChanged = !hasPreviousMembershipFingerprint_ ||
			membershipFingerprint_ != membershipFingerprint;
		membershipFingerprint_ = membershipFingerprint;
		hasPreviousMembershipFingerprint_ = true;
		hasCurrentMembershipObservation_ = true;
		return fingerprintChanged;
	}

	/** Lazily creates and returns this Section's exclusively owned numerical context. */
	Visuals::BuildingVisualsContext& BuildingMapSection::GetOrCreateVisualsContext()
	{
		if (!visualsContext_)
		{
			visualsContext_ = std::make_unique<Visuals::BuildingVisualsContext>();
		}
		return *visualsContext_;
	}
}

#endif // MODLOADER_CLIENT_BUILD
