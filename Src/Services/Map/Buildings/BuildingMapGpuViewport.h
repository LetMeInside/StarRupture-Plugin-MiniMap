// Defines the service-created GPU viewport specialization for future output work.
#pragma once

#include "BuildingMapViewport.h"

#include <cstdint>

namespace Services::Map::Buildings
{
	namespace Cache { class SpriteSourceManager; }
	/** Specializes viewport identity for GPU output while retaining the base viewport's registration and lifetime. */
	class BuildingMapGpuViewport final : public BuildingMapViewport
	{
	public:
		/** Allows owned GPU viewports to be destroyed safely through their base pointer. */
		~BuildingMapGpuViewport() override = default;
		/** Returns read-only access to the service's published shared sprite atlas. */
		const Cache::SpriteSourceManager* GetPublishedSpriteAtlas() const noexcept;

	private:
		friend class BuildingMapService;

		/** Creates the specialization using the existing service-owned base registration inputs. */
		BuildingMapGpuViewport(
			BuildingMapService& service,
			IBuildingCollector& buildingCollector,
			std::uint64_t viewportHandle,
			double xMapUnits,
			double yMapUnits,
			double widthMapUnits,
			double heightMapUnits);
	};
}
