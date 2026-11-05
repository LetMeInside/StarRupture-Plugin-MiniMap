// Owns Building Map sections, sprite caches, scheduling and capture sessions, and creates scoped viewports.
#pragma once

#include "IBuildingCollector.h"
#include "Coordination/IBuildingMapService.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>

namespace Services::Map::Buildings
{
	class BuildingMapSection;
	class BuildingMapViewport;
	namespace Cache
	{
		class CpuSpriteSourceManager;
		class SpriteDiskCache;
	}
	namespace Scheduling
	{
		class BuildingScheduler;
	}

	/** Owns map configuration, sections, temporary acquisition resources, and viewport registrations.
	 * Service and viewport calls must be made on one calling thread; this
	 * foundation does not provide concurrent access or synchronization. The service
	 * keeps cache/source/scheduler owners only until future SectionScheduler ownership exists.
	 */
	class BuildingMapService final : private Coordination::IBuildingMapService
	{
	public:
		/** Creates a service whose settings define the shared grid and whose absolute cache directory serves sprite files.
		 * Throws std::invalid_argument unless section size is finite and positive,
		 * resolution is positive, collection margin is finite and nonnegative, and
		 * the Buildings cache directory is nonempty and absolute. Construction does
		 * not require that directory to exist and performs no cache file operations.
		 *
		 * @param sectionSizeMapUnits Width and height of each square cell in map units.
		 * @param sectionResolutionPixels Width and height of each future square image in pixels.
		 * @param collectionMarginMapUnits Acquisition expansion around covered cells in map units.
		 * @param buildingsCacheDirectory Already-resolved absolute Buildings sprite-cache directory.
		 */
		BuildingMapService(
			double sectionSizeMapUnits,
			std::uint32_t sectionResolutionPixels,
			double collectionMarginMapUnits,
			std::filesystem::path buildingsCacheDirectory);
		/** Prevents copies from creating ambiguous ownership of sections and sessions. */
		BuildingMapService(const BuildingMapService&) = delete;
		/** Prevents copies from creating ambiguous ownership of sections and sessions. */
		BuildingMapService& operator=(const BuildingMapService&) = delete;
		/** Prevents moves from invalidating non-owning viewport service references. */
		BuildingMapService(BuildingMapService&&) = delete;
		/** Prevents moves from invalidating non-owning viewport service references. */
		BuildingMapService& operator=(BuildingMapService&&) = delete;
		/** Invalidates viewports; the owner must finish active game-world-session shutdown before destruction. */
		~BuildingMapService();

		/** Returns the configured square grid-cell width and height in map units. */
		double GetSectionSizeMapUnits() const noexcept override;

		/** Returns the configured square image dimension for future composition in pixels. */
		std::uint32_t GetSectionResolutionPixels() const noexcept;

		/** Returns the configured acquisition expansion in map units. */
		double GetCollectionMarginMapUnits() const noexcept;

		/** Returns the derived number of image pixels per map unit. */
		double GetPixelsPerMapUnit() const noexcept;

		/**
		 * Opens a viewport with its own non-owning collector association. The caller
		 * must supply a collector not already associated with another open viewport,
		 * retain ownership, and keep it alive until the viewport closes or is invalidated.
		 * Throws for a null collector or invalid rectangle; returns null if the
		 * collector rejects the initial acquisition region.
		 *
		 * @param buildingCollector Caller-owned collector used only by this viewport.
		 * @param xMapUnits Upper-left map X coordinate in this service's map units.
		 * @param yMapUnits Upper-left map Y coordinate in this service's map units.
		 * @param widthMapUnits Positive viewport width in map units.
		 * @param heightMapUnits Positive viewport height in map units.
		 */
		std::unique_ptr<BuildingMapViewport> OpenViewport(
			IBuildingCollector* buildingCollector,
			double xMapUnits,
			double yMapUnits,
			double widthMapUnits,
			double heightMapUnits);

	private:
		friend class BuildingMapViewport;

		/** Finds a service-owned section; columns increase rightward and rows downward. */
		const BuildingMapSection* FindSectionAt(
			std::int64_t gridX,
			std::int64_t gridY) const override;

		/** Releases a live viewport handle without affecting shared section state. */
		void CloseViewport(std::uint64_t viewportHandle) override;

		/** Calculates and applies a map rectangle aligned to the supplied exclusive column and row range. */
		bool UpdateViewportGridRange(
			std::uint64_t viewportHandle,
			std::int64_t firstGridX,
			std::int64_t firstGridY,
			std::int64_t endGridX,
			std::int64_t endGridY) override;

		/**
		 * Requests one batch observation for the current cell grid and updates each
		 * corresponding shared section state.
		 *
		 * @param viewportHandle Opaque registration handle owned by the viewport.
		 * @return True if any complete fingerprint was first observed or differs from its prior one.
		 */
		bool RefreshViewportSections(std::uint64_t viewportHandle) override;

		/** Creates a service-owned section only when an internal producer needs it. */
		BuildingMapSection& GetOrCreateSectionAt(std::int64_t gridX, std::int64_t gridY);
		/** Begins one scheduler-owned renderer lifetime for the current game-world session. */
		bool BeginCaptureGameWorldSession(std::uint64_t sessionGeneration);
		/** Advances scheduler-owned shutdown and releases native state only after retirement completes. */
		bool AdvanceCaptureGameWorldSessionShutdown();

		/**
		 * Applies one cell result from a batch without creating a section for incomplete coverage.
		 *
		 * @param gridX World-grid X coordinate of the observed section.
		 * @param gridY World-grid Y coordinate of the observed section.
		 * @param observation Complete or incomplete collector result for that cell.
		 * @return True when a complete fingerprint is new or differs from its prior value.
		 */
		bool ApplySectionObservation(
			std::int64_t gridX,
			std::int64_t gridY,
			const BuildingSectionObservation& observation);

		const double sectionSizeMapUnits_;
		const std::uint32_t sectionResolutionPixels_;
		const double collectionMarginMapUnits_;
		std::map<std::pair<std::int64_t, std::int64_t>, std::unique_ptr<BuildingMapSection>> sections_;
		std::map<std::uint64_t, BuildingMapViewport*> viewportsByHandle_;
		std::uint64_t nextViewportHandle_ = 1;
		std::uint64_t lastCaptureSessionGeneration_ = 0;
		std::unique_ptr<Cache::SpriteDiskCache> spriteDiskCache_;
		std::unique_ptr<Cache::CpuSpriteSourceManager> cpuSpriteSourceManager_;
		// Reverse destruction releases the scheduler and its renderer before borrowed resources.
		std::unique_ptr<Scheduling::BuildingScheduler> buildingScheduler_;
	};
}
