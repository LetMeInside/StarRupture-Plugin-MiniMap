// Owns Building Map acquisition resources and coordinates their game-world session lifecycle.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

namespace Services::Map::Buildings
{
	class BuildingMapService;
}

namespace Services::Map::Buildings::Cache
{
	class CpuSpriteSourceManager;
	class SpriteDiskCache;
}

namespace Services::Map::Buildings::Scheduling
{
	class BuildingScheduler;

	/** Owns sibling acquisition resources; the service controls session lifetime and shutdown progression.
	 * SpriteDiskCache outlives the CPU manager, which outlives BuildingScheduler and its renderer.
	 * Construction is inert; session shutdown must finish before this owner is destroyed.
	 */
	class SectionScheduler final
	{
	public:
		/** Prevents copies from duplicating acquisition resources or borrowed dependencies. */
		SectionScheduler(const SectionScheduler&) = delete;
		/** Prevents copies from duplicating acquisition resources or borrowed dependencies. */
		SectionScheduler& operator=(const SectionScheduler&) = delete;
		/** Prevents moves from invalidating subordinate borrowed resource pointers. */
		SectionScheduler(SectionScheduler&&) = delete;
		/** Prevents moves from invalidating subordinate borrowed resource pointers. */
		SectionScheduler& operator=(SectionScheduler&&) = delete;
		/** Releases owned objects without initiating native cleanup. */
		~SectionScheduler();

	private:
		friend class Services::Map::Buildings::BuildingMapService;

		/** Stores immutable service configuration and creates acquisition resources without I/O.
		 * @param sectionSizeMapUnits Width and height of the service's square cell in map units.
		 * @param sectionResolutionPixels Width and height of the future square image in pixels.
		 * @param collectionMarginMapUnits Acquisition expansion in map units.
		 * @param buildingsCacheDirectory Service-validated absolute Buildings cache directory.
		 */
		SectionScheduler(
			double sectionSizeMapUnits,
			std::uint32_t sectionResolutionPixels,
			double collectionMarginMapUnits,
			std::filesystem::path buildingsCacheDirectory);

		/** Begins the existing BuildingScheduler session using the stored service image scale.
		 * @param sessionGeneration Game-world session identity admitted by the service.
		 * @return True when BuildingScheduler establishes this renderer session.
		 */
		bool BeginCaptureGameWorldSession(std::uint64_t sessionGeneration);

		/** Advances existing ordered renderer retirement without performing destructor cleanup.
		 * @return True when native retirement is complete and session state is released.
		 */
		bool AdvanceCaptureGameWorldSessionShutdown();

		const double sectionSizeMapUnits_;
		const std::uint32_t sectionResolutionPixels_;
		const double collectionMarginMapUnits_;
		std::unique_ptr<Cache::SpriteDiskCache> spriteDiskCache_;
		std::unique_ptr<Cache::CpuSpriteSourceManager> cpuSpriteSourceManager_;
		std::unique_ptr<BuildingScheduler> buildingScheduler_;
	};
}
