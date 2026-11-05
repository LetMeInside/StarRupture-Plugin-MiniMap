// Owns Building Map sections, sprite caches, scheduling and capture sessions, and creates scoped viewports.
#pragma once

#include "Cache/CpuSpriteSourceManager.h"
#include "IBuildingCollector.h"
#include "Coordination/IBuildingMapService.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string_view>
#include <utility>

namespace Services::Map::Buildings
{
	class BuildingMapSection;
	class BuildingMapViewport;
	namespace Cache
	{
		class SpriteDiskCache;
	}
	namespace Scheduling
	{
		class BuildingScheduler;
	}
	namespace Capture
	{
		class BuildingSpriteRenderer;
	}

	/** Owns map configuration, sections, sprite sources and caches, scheduling, capture, and viewport registrations.
	 * Service and viewport calls must be made on one calling thread; this
	 * foundation does not provide concurrent access or synchronization. The capture
	 * renderer pointer remains empty until future service-directed initialization.
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

		/** Returns the inert service-owned manager for immutable CPU sprite sources.
		 * It performs no startup work; callers publish sources only through an explicitly
		 * authorized producer path and retain lookup leases across source use.
		 */
		Cache::CpuSpriteSourceManager& GetCpuSpriteSourceManager() noexcept;

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

		/** Describes whether one Blueprint sprite is already usable or can be captured. */
		enum class SpriteRequirementStatus
		{
			Ready,
			CaptureScheduled,
			Busy,
			Unavailable
		};

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
		/** Begins one inert renderer lifetime for the current game-world session. */
		bool BeginCaptureGameWorldSession(std::uint64_t sessionGeneration);
		/** Advances session shutdown and releases the renderer only after native retirement completes. */
		bool AdvanceCaptureGameWorldSessionShutdown();
		/** Resolves one canonical Blueprint sprite from cache or admits its capture request. */
		SpriteRequirementStatus EnsureBlueprintSprite(std::string_view canonicalBlueprintName);
		/** Advances and consumes the one accepted capture request, preserving renderer retirement ownership. */
		void AdvanceAdmittedCapture();

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
		Cache::CpuSpriteSourceManager cpuSpriteSourceManager_;
		// Declaration order keeps scheduler references valid until the scheduler is destroyed.
		std::unique_ptr<Cache::SpriteDiskCache> spriteDiskCache_;
		std::unique_ptr<Scheduling::BuildingScheduler> buildingScheduler_;
		// Default-null inert owner; service teardown must retire request work, shared lights, and world first.
		std::unique_ptr<Capture::BuildingSpriteRenderer> captureRenderer_;
	};
}
