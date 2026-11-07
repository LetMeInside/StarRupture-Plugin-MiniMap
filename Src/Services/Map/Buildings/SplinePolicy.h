// Defines game-independent solid spline appearance copied into a BuildingMapService.
#pragma once

#include <cstdint>

namespace Services::Map::Buildings
{
	/** Holds a copy-safe, game-independent solid appearance for a spline surface.
	 * Optional texture appearance remains a future extension after its CPU source and ownership are established.
	 */
	struct SplinePolicy
	{
		/** Packed sRGB representative color in 0xRRGGBB form. */
		std::uint32_t SolidRgb = 0xFFFFFF;
		/** Straight opacity in the inclusive normalized range [0, 1]. */
		double Opacity = 1.0;
	};
}
