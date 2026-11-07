// Defines StarRupture's fixed numeric identifiers for neutral spline appearance policies.
#pragma once

#include <cstdint>

namespace MiniMap::BuildingAcquisition
{
	/** Maps positively identified StarRupture spline definitions to neutral service indexes.
	 * Unknown remains zero and is never a selectable Building Map policy.
	 */
	enum class SplinePolicyId : std::uint16_t
	{
		Unknown = 0,
		DroneRail = 1,
		Zipline = 2,
		Walkway = 3
	};
}
