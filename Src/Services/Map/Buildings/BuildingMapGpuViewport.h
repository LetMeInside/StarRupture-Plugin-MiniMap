// Defines the service-created GPU viewport specialization for future output work.
#pragma once

#include "BuildingMapViewport.h"
#include "Visuals/PreparedBuildingGeometry.h"

#include <cstdint>
#include <functional>

namespace Services::Map::Buildings
{
	namespace Cache { class SpriteSourceManager; }
	/** Specializes viewport identity for GPU output while retaining the base viewport's registration and lifetime. */
	class BuildingMapGpuViewport final : public BuildingMapViewport
	{
	public:
		/** Receives one demanded Section's borrowed drawable geometry in deterministic grid order.
		 * The geometry reference is valid only during this synchronous callback. The callback must
		 * not retain its address or spans, reenter scheduling, mutate service/session/viewport
		 * state, or destroy the service or viewport. IsComplete is false for first appearance.
		 */
		using DrawableSectionHandler = std::function<void(
			std::int64_t,
			std::int64_t,
			const Visuals::PreparedBuildingGeometry&,
			bool)>;

		/** Allows owned GPU viewports to be destroyed safely through their base pointer. */
		~BuildingMapGpuViewport() override = default;
		/** Returns read-only access to the service's published shared sprite atlas. */
		const Cache::SpriteSourceManager* GetPublishedSpriteAtlas() const noexcept;
		/** Visits existing demanded Sections without copying their geometry.
		 * Read OrdinarySprites(), then SplineSurfaces(), then OverlayLastSprites() from each
		 * borrowed scene; each category retains its established deterministic preparation order.
		 */
		void EnumerateDrawableSections(const DrawableSectionHandler& handler) const;

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
