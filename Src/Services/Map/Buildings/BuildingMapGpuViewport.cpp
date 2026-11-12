#if defined(MODLOADER_CLIENT_BUILD)

// Implements the minimal service-created GPU viewport specialization.
#include "BuildingMapGpuViewport.h"
#include "BuildingMapService.h"

namespace Services::Map::Buildings
{
	/** Borrows the service-owned manager without transferring texture or registry ownership. */
	const Cache::SpriteSourceManager* BuildingMapGpuViewport::GetPublishedSpriteAtlas() const noexcept
	{
		return service_ != nullptr ? service_->PublishedSpriteAtlas() : nullptr;
	}

	/** Delegates deterministic borrowed reads to the service that owns every Section. */
	void BuildingMapGpuViewport::EnumerateDrawableSections(const DrawableSectionHandler& handler) const
	{
		if (service_ != nullptr && handler)
		{
			service_->EnumerateDrawableSections(*this, handler);
		}
	}

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
			heightMapUnits,
			false)
	{
	}
}

#endif // MODLOADER_CLIENT_BUILD
