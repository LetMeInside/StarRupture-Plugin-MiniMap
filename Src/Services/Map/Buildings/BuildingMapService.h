// Owns the shared Section grid, viewport registrations and image residency, and forwards demand to its coordinator.
#pragma once

#include "IBuildingCollector.h"
#include "Coordination/FrameBudgetController.h"
#include "SplinePolicy.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <utility>
#include <vector>

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
#include "Scheduling/BuildingScheduler.h"
#endif

namespace Services::Map::Buildings
{
	namespace Capture
	{
		class BuildingSpriteRenderer;
	}

	class BuildingMapSection;
	class BuildingMapViewport;
	class BuildingMapGpuViewport;
	namespace Visuals
	{
		class PreparedBuildingGeometry;
	}
	namespace Cache
	{
		class SpriteSourceManager;
	}
	namespace Scheduling
	{
		class SectionSchedulingCoordinator;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		struct SectionSchedulingDiagnosticsSnapshot;
#endif
	}

	/** Owns map configuration, shared sprite manager, Sections and image residency, viewport registrations, source selection, image-change routing, tick budget and scheduling coordinator.
	 * Service and viewport calls must be made on one calling thread; this
	 * foundation does not provide concurrent access or synchronization. The
	 * coordinator owns the SectionScheduler, which borrows the shared sprite manager.
	 */
	class BuildingMapService final
	{
	public:
		/** Creates a service whose settings define the shared grid and whose cache root contains its sprite files.
		 * Throws std::invalid_argument unless section size and sprite capture density are finite and positive,
		 * resolution is positive, and the MiniMap cache root is nonempty and absolute.
		 * The root is normalized and the service derives its Buildings subdirectory.
		 * Construction does not require either directory to exist and performs no cache
		 * file operations. The spline policy span is copied; its index-zero entry is
		 * reserved and cannot be resolved by TryGetSplinePolicy.
		 *
		 * @param sectionSizeMapUnits Width and height of each square cell in map units.
		 * @param sectionResolutionPixels Width and height of each future square image in pixels.
		 * @param spriteCaptureDensityPixelsPerMapUnit Sprite capture pixels per map unit, independent of Section image resolution.
		 * @param cacheRoot Absolute application-wide MiniMap/Cache directory.
		 * @param splinePolicies Direct-indexed policies with reserved entry zero.
		 */
		BuildingMapService(
			double sectionSizeMapUnits,
			std::uint32_t sectionResolutionPixels,
			double spriteCaptureDensityPixelsPerMapUnit,
			std::filesystem::path cacheRoot,
			std::span<const SplinePolicy> splinePolicies);
		/** Prevents copies from creating ambiguous ownership of sections and sessions. */
		BuildingMapService(const BuildingMapService&) = delete;
		/** Prevents copies from creating ambiguous ownership of sections and sessions. */
		BuildingMapService& operator=(const BuildingMapService&) = delete;
		/** Prevents moves from invalidating non-owning viewport service references. */
		BuildingMapService(BuildingMapService&&) = delete;
		/** Prevents moves from invalidating non-owning viewport service references. */
		BuildingMapService& operator=(BuildingMapService&&) = delete;
		/** Invalidates outstanding viewports when the plugin-lifetime owner is destroyed. */
		~BuildingMapService();
		/** Clears only this service's configured Buildings cache subtree.
		 * CacheIdentity remains responsible for deciding when this operation is requested.
		 * @return True when the cache subtree was cleared without a filesystem error.
		 */
		bool ClearCache();
		/** Returns read-only access to the shared, currently published sprite atlas. */
		const Cache::SpriteSourceManager* PublishedSpriteAtlas() const noexcept;
		/** Starts gameplay scheduling against the persistent renderer and stores map dimensions.
		 * @param mapWidthMapUnits Active gameplay map width in map units.
		 * @param mapHeightMapUnits Active gameplay map height in map units.
		 * @return True when gameplay scheduling starts and the map dimensions become active.
		 */
		bool StartGameSession(double mapWidthMapUnits, double mapHeightMapUnits);
		/** Ends one gameplay session, requests active-capture cancellation, and clears gameplay state.
		 * Returns without waiting for renderer-owned native retirement.
		 */
		/** Stops ownership and reports whether every atlas texture receipt was retired. */
		bool StopGameSession();
		/** Forwards one game-thread tick delta to the service-owned timing controller.
		 * @param engineDeltaSeconds Engine-provided delta retained for Debug diagnostics only.
		 */
		void OnGameTick(float engineDeltaSeconds) noexcept;
		/** Returns the persistent renderer for direct plugin engine-callback execution. */
		Capture::BuildingSpriteRenderer* GetCapturerLifecycleTarget() noexcept;
		/** Returns the active gameplay map width, or zero outside a gameplay session. */
		double GetMapWidthMapUnits() const noexcept;
		/** Returns the active gameplay map height, or zero outside a gameplay session. */
		double GetMapHeightMapUnits() const noexcept;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		/** Forwards one completed numeric scheduling interval without exposing its coordinator. */
		bool TakeSectionSchedulingDiagnosticsSnapshot(
			Scheduling::SectionSchedulingDiagnosticsSnapshot& output) noexcept;
		/** Forwards the independent scheduler-owned sprite acquisition interval.
		 * @param output Receives the completed numeric snapshot when its interval is due.
		 * @return True when the coordinator hands off a completed interval.
		 */
		bool TakeSpriteAcquisitionDiagnosticsSnapshot(
			Scheduling::BuildingScheduler::DiagnosticsSnapshot& output) noexcept;
#endif

		/** Returns the configured square grid-cell width and height in map units. */
		double GetSectionSizeMapUnits() const noexcept;

		/** Returns the configured square image dimension for future composition in pixels. */
		std::uint32_t GetSectionResolutionPixels() const noexcept;

		/** Returns the derived number of image pixels per map unit. */
		double GetPixelsPerMapUnit() const noexcept;
		/** Returns an immutable policy pointer, or null for reserved/unknown/out-of-range indexes.
		 * @param splinePolicyIndex Direct numeric index supplied by a neutral source record.
		 */
		const SplinePolicy* TryGetSplinePolicy(std::uint16_t splinePolicyIndex) const noexcept;

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
		/** Opens a GPU-specialized viewport through the same service registration path.
		 * The caller retains collector ownership and must keep it alive until viewport closure or invalidation.
		 * Throws for a null collector, duplicate collector association or invalid rectangle; returns null when
		 * no gameplay session is active or the collector rejects its initial acquisition region.
		 * @param buildingCollector Caller-owned collector used only by this viewport.
		 * @param xMapUnits Upper-left map X coordinate in this service's map units.
		 * @param yMapUnits Upper-left map Y coordinate in this service's map units.
		 * @param widthMapUnits Positive viewport width in map units.
		 * @param heightMapUnits Positive viewport height in map units.
		 */
		std::unique_ptr<BuildingMapGpuViewport> OpenGpuViewport(
			IBuildingCollector* buildingCollector,
			double xMapUnits,
			double yMapUnits,
			double widthMapUnits,
			double heightMapUnits);

	private:
		friend class BuildingMapViewport;
		friend class BuildingMapGpuViewport;
		friend class Scheduling::SectionSchedulingCoordinator;
		using SectionCoordinate = std::pair<std::int64_t, std::int64_t>;
		using SectionPublicationMap = std::map<
			SectionCoordinate,
			std::shared_ptr<const BuildingCandidatePublication>>;

		/** Finds a service-owned section; columns increase rightward and rows downward. */
		BuildingMapSection* FindSectionAt(std::int64_t gridX, std::int64_t gridY);
		/** Finds a borrowed service-owned Section for const geometry enumeration. */
		const BuildingMapSection* FindSectionAt(
			std::int64_t gridX,
			std::int64_t gridY) const;
		/** Enumerates available geometry in this GPU viewport's current range using borrowed Section storage. */
		void EnumerateDrawableSections(
			const BuildingMapGpuViewport& viewport,
			const std::function<void(
				std::int64_t,
				std::int64_t,
				const Visuals::PreparedBuildingGeometry&,
				bool)>& handler) const;
		/** Queues one Section presentation whose selected prepared scene changed. */
		void MarkGpuSectionPresentationChanged(std::int64_t gridX, std::int64_t gridY);
		/** Queues one GPU viewport after its registered Section range changes. */
		void MarkGpuViewportRangeChanged(std::uint64_t viewportHandle);
		/** Refreshes only GPU viewports affected by Section, range or atlas changes. */
		void RefreshGpuPresentationSnapshots(bool atlasBindingsChanged);

		/** Releases a live viewport handle without affecting shared section state. */
		void CloseViewport(std::uint64_t viewportHandle);
		/** Validates session and collector eligibility before allocating a registration handle.
		 * @param buildingCollector Caller-owned collector to associate with the new viewport.
		 * @param viewportHandle Receives the newly allocated handle on success.
		 * @return False when no gameplay session is active; validation errors throw.
		 */
		bool PrepareViewportRegistration(
			IBuildingCollector* buildingCollector,
			std::uint64_t& viewportHandle);
		/** Registers and initializes one caller-owned viewport, rolling back on rejection. */
		bool RegisterViewport(BuildingMapViewport& viewport);

		/** Routes one completed-image availability change to registered subscribed viewports that contain it.
		 * Delivery follows handle registration order and carries no Section or image ownership.
		 * @param gridX Signed global Section column whose completed-image state changed.
		 * @param gridY Signed global Section row whose completed-image state changed.
		 */
		void NotifySectionImageChangedToViewports(
			std::int64_t gridX,
			std::int64_t gridY
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			, std::size_t* diagnosticNotifications
#endif
		);

		/**
		 * Sends the exact map-space rectangle covered by a half-open Section grid range.
		 * No game-specific acquisition padding is added; the collector owns discovery policy.
		 * @param viewportHandle Registered viewport whose collector receives the request.
		 * @param firstGridX Inclusive first grid column.
		 * @param firstGridY Inclusive first grid row.
		 * @param endGridX Exclusive end grid column.
		 * @param endGridY Exclusive end grid row.
		 * @return True when the exact rectangle is representable and the collector accepts it.
		 */
		bool UpdateViewportGridRange(
			std::uint64_t viewportHandle,
			std::int64_t firstGridX,
			std::int64_t firstGridY,
			std::int64_t endGridX,
			std::int64_t endGridY);

		/** Observes one live viewport's acquisition hash and advances shared Section scheduling once. */
		bool AdvanceViewportScheduling(std::uint64_t viewportHandle);
		/** Queues a one-shot Section hash comparison after a collector policy change. */
		void RequestViewportSectionHashRecheck(std::uint64_t viewportHandle);

		/** Creates a service-owned section only when an internal producer needs it. */
		BuildingMapSection& GetOrCreateSectionAt(std::int64_t gridX, std::int64_t gridY);
		/** Begins gameplay scheduling against the coordinator's persistent renderer. */
		bool BeginCaptureGameWorldSession();
		/** Ends gameplay scheduling and requests cancellation of active capture work. */
		void StopCaptureGameWorldSession();
		/** Adds every cell in a live viewport's current half-open grid range. */
		void AddViewportGridCoordinates(
			const BuildingMapViewport& viewport,
			std::set<SectionCoordinate>& coordinates) const;
		/** Requests applicable sources in order and preserves equivalent pending publication owners. */
		void AcquireSectionPublicationBatches(
			const std::set<SectionCoordinate>& coordinates,
			std::uint64_t recheckedViewportHandle,
			bool clearRecheckWhenCovered);
		/** Acquires and validates one viewport's ordered Section publication batch. */
		void AcquireViewportSectionPublicationBatch(
			std::uint64_t viewportHandle,
			BuildingMapViewport& viewport,
			const std::set<SectionCoordinate>& coordinates,
			std::set<SectionCoordinate>& selectedCoordinates,
			bool clearRecheckWhenCovered);
		/** Builds one viewport's exact region and coordinate arrays from the requested cells. */
		bool BuildViewportSectionRequest(
			const BuildingMapViewport& viewport,
			const std::set<SectionCoordinate>& coordinates,
			const std::set<SectionCoordinate>& selectedCoordinates,
			std::vector<SectionCoordinate>& requestedCoordinates,
			std::vector<BuildingCollectionRegion>& requestedRegions) const;
		/** Tests whether the current acquisition pass selected a publication for every viewport cell. */
		bool AreAllViewportSectionsSelected(
			const BuildingMapViewport& viewport,
			const std::set<SectionCoordinate>& selectedCoordinates) const;
		/** Observes acquisition content and reports whether demanded Section hashes need comparison. */
		bool ObserveViewportAcquisitionHash(
			std::uint64_t viewportHandle,
			const IBuildingCollector& buildingCollector);
		/** Chooses affected demand or waiting work that lacks a retained publication. */
		std::set<SectionCoordinate> GetSectionRequestCoordinates(
			const BuildingMapViewport& viewport,
			bool compareViewportDemand);
		/** Catches only collector request failures that mean its committed Section data is unavailable. */
		bool TryAcquireSectionPublicationBatch(
			BuildingMapViewport& viewport,
			const std::vector<BuildingCollectionRegion>& requestedRegions,
			std::vector<std::shared_ptr<const BuildingCandidatePublication>>& publications);
		/** Validates ordered results and lets the first registered valid source win. */
		void RetainSectionPublicationBatch(
			std::uint64_t viewportHandle,
			const std::vector<SectionCoordinate>& requestedCoordinates,
			const std::vector<BuildingCollectionRegion>& requestedRegions,
			const std::vector<std::shared_ptr<const BuildingCandidatePublication>>& publications,
			std::set<SectionCoordinate>& selectedCoordinates,
			bool completeRequestBatch,
			bool clearRecheckWhenCovered);
		/** Removes retained publications whose content is already successfully applied. */
		void ReleasePendingPublicationsAlreadyApplied();
		/** Invalidates selected publications for explicit source invalidation or viewport closure. */
		void InvalidatePendingPublicationsInRange(
			std::int64_t firstGridX,
			std::int64_t firstGridY,
			std::int64_t endGridX,
			std::int64_t endGridY);
		/** Invalidates prior-source publications only for cells that departed an updated viewport. */
		void InvalidateDepartedViewportPublications(
			std::int64_t previousFirstGridX,
			std::int64_t previousFirstGridY,
			std::int64_t previousEndGridX,
			std::int64_t previousEndGridY,
			std::int64_t nextFirstGridX,
			std::int64_t nextFirstGridY,
			std::int64_t nextEndGridX,
			std::int64_t nextEndGridY);
		/** Converts a signed grid cell to its exact finite map-space rectangle. */
		bool TryCreateSectionRegion(
			std::int64_t gridX,
			std::int64_t gridY,
			BuildingCollectionRegion& output) const noexcept;
		/** Retains an applied hash only after an image or confirmed-empty commit. */
		void RecordSectionAppliedHash(
			std::int64_t gridX,
			std::int64_t gridY,
			std::uint64_t contentHash);

		/** Checks whether any valid active viewport's one-cell-expanded range contains a Section coordinate.
		 * @param gridX Signed global Section column to check.
		 * @param gridY Signed global Section row to check.
		 * @return True when at least one current viewport keeps the coordinate resident.
		 */
		bool IsSectionWithinAnyViewportResidencyRange(std::int64_t gridX, std::int64_t gridY) const;

		/** Releases completed images outside all active viewport residency ranges without erasing Sections. */
		void ReconcileSectionImageResidency();

		const double sectionSizeMapUnits_;
		const std::uint32_t sectionResolutionPixels_;
		const double spriteCaptureDensityPixelsPerMapUnit_;
		Coordination::FrameBudgetController frameBudgetController_;
		const std::vector<SplinePolicy> splinePolicies_;
		double mapWidthMapUnits_ = 0.0;
		double mapHeightMapUnits_ = 0.0;
		std::map<std::pair<std::int64_t, std::int64_t>, std::unique_ptr<BuildingMapSection>> sections_;
		std::map<std::uint64_t, BuildingMapViewport*> viewportsByHandle_;
		std::map<std::uint64_t, std::uint64_t> observedAcquisitionHashes_;
		std::map<SectionCoordinate, std::uint64_t> appliedSectionHashes_;
		std::set<SectionCoordinate> gpuPresentationChangedSections_;
		std::set<std::uint64_t> gpuViewportRangesChanged_;
		SectionPublicationMap pendingSectionPublications_;
		std::set<std::uint64_t> sectionHashRechecksRequested_;
		std::uint64_t nextViewportHandle_ = 1;
		bool sectionImageResidencyReconciliationPending_ = false;
		std::unique_ptr<Cache::SpriteSourceManager> spriteSourceManager_;
		std::unique_ptr<Scheduling::SectionSchedulingCoordinator> sectionSchedulingCoordinator_;
	};
}
