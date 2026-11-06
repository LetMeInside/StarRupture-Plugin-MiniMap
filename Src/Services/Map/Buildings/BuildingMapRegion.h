// Defines the shared neutral rectangle used by acquisition requests and adapted source views.
#pragma once

namespace Services::Map::Buildings
{
	/** Describes a half-open rectangle with upper-left origin and downward-positive Y. */
	struct BuildingCollectionRegion
	{
		/** Upper-left X coordinate in the owning map units. */
		double xMapUnits = 0.0;
		/** Upper-left Y coordinate in the owning map units. */
		double yMapUnits = 0.0;
		/** Positive requested width in map units. */
		double widthMapUnits = 0.0;
		/** Positive requested height in map units. */
		double heightMapUnits = 0.0;
	};
}
