// Defines the narrow service boundary used by BuildingMapViewport for grid-size
// access, section lookup and viewport registration operations.
#pragma once

#include <cstdint>

namespace Services::Map::Buildings
{
	class BuildingMapSection;
}

namespace Services::Map::Buildings::Coordination
{
	/** Provides only the service operations required by a live viewport. */
	class IBuildingMapService
	{
	public:
		/** Allows viewport references to be destroyed through the internal boundary. */
		virtual ~IBuildingMapService() = default;

		/** Returns the immutable grid-cell size used to convert viewport bounds to cells. */
		virtual double GetSectionSizeMapUnits() const noexcept = 0;

		/** Finds a cell by column and row; columns increase rightward and rows downward. */
		virtual const BuildingMapSection* FindSectionAt(
			std::int64_t gridX,
			std::int64_t gridY) const = 0;

		/**
		 * Applies the expanded collection region for a changed viewport grid range.
		 * End coordinates are exclusive; false leaves viewport and collector state unchanged.
		 */
		virtual bool UpdateViewportGridRange(
			std::uint64_t viewportHandle,
			std::int64_t firstGridX,
			std::int64_t firstGridY,
			std::int64_t endGridX,
			std::int64_t endGridY) = 0;

		/**
		 * Refreshes each cell in the registered viewport's current range.
		 *
		 * @param viewportHandle Opaque registration handle owned by the viewport.
		 * @return True if at least one complete membership fingerprint was first observed or changed.
		 */
		virtual bool RefreshViewportSections(std::uint64_t viewportHandle) = 0;

		/** Releases the registration represented by an opaque viewport handle. */
		virtual void CloseViewport(std::uint64_t viewportHandle) = 0;
	};
}
