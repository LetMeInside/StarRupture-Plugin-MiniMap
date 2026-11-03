// Represents one service-owned cell in the shared sparse Building Map grid and
// stores the latest certified source-membership observation independently of
// any future completed image state.
#pragma once

#include <cstdint>

namespace Services::Map::Buildings
{
	/** Identifies one fixed service-owned map cell; X columns and Y rows increase rightward and downward. */
	class BuildingMapSection
	{
	public:
		/** Returns the integer world-grid coordinate on the X axis. */
		std::int64_t GetGridX() const;

		/** Returns the integer world-grid coordinate on the Y axis. */
		std::int64_t GetGridY() const;

		/** Returns true when the section has a currently certified membership observation. */
		bool HasCurrentMembershipObservation() const;

		/**
		 * Copies the current certified fingerprint to the output when available.
		 * Returns false without changing the output when membership is uncertified.
		 *
		 * @param fingerprint Receives the valid fingerprint, including a possible zero value.
		 * @return True only while a complete observation is currently certified.
		 */
		bool TryGetCurrentMembershipFingerprint(std::uint64_t& fingerprint) const;

	private:
		friend class BuildingMapService;

		/** Creates a section for the service-owned grid coordinate. */
		BuildingMapSection(std::int64_t gridX, std::int64_t gridY);

		/**
		 * Records a complete fingerprint or revokes current certification when incomplete.
		 *
		 * @param isComplete Whether the collector certified complete source coverage.
		 * @param membershipFingerprint Fingerprint to retain when isComplete is true.
		 * @return True for the first complete fingerprint or a change from the previous one.
		 */
		bool RecordMembershipObservation(bool isComplete, std::uint64_t membershipFingerprint);

		std::int64_t gridX_;
		std::int64_t gridY_;
		bool hasCurrentMembershipObservation_ = false;
		bool hasPreviousMembershipFingerprint_ = false;
		std::uint64_t membershipFingerprint_ = 0;
	};
}
