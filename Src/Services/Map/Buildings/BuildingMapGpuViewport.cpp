#if defined(MODLOADER_CLIENT_BUILD)

// Implements the minimal service-created GPU viewport specialization.
#include "BuildingMapGpuViewport.h"

namespace Services::Map::Buildings
{
	/** Delegates all registration state and behavior to the existing base viewport. */
	BuildingMapGpuViewport::BuildingMapGpuViewport(
		BuildingMapService& service,
		IBuildingCollector& buildingCollector,
		std::uint64_t viewportHandle,
		double xMapUnits,
		double yMapUnits,
		double widthMapUnits,
		double heightMapUnits)
		: BuildingMapViewport(
			service,
			buildingCollector,
			viewportHandle,
			xMapUnits,
			yMapUnits,
			widthMapUnits,
			heightMapUnits)
	{
	}
}

#endif // MODLOADER_CLIENT_BUILD
