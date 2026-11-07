// Owns the shared Section grid, viewport registrations and image residency, and forwards demand to its coordinator.
#pragma once

#include "IBuildingCollector.h"
#include "Coordination/IBuildingMapService.h"
#include "SplinePolicy.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <vector>

namespace Services::Map::Buildings
{
	class BuildingMapSection;
	class BuildingMapViewport;
	namespace Scheduling
	{
		class SectionSchedulingCoordinator;
	}

	/** Owns map configuration, shared Sections and their image residency, viewport registrations, source selection, image-change routing and the scheduling coordinator.
	 * Service and viewport calls must be made on one calling thread; this
	 * foundation does not provide concurrent access or synchronization. The
	 * coordinator owns the SectionScheduler and cache-aware acquisition resources.
	 */
	class BuildingMapService final : private Coordination::IBuildingMapService
	{
	public:
		/** Creates a service whose settings define the shared grid and whose absolute cache directory serves sprite files.
		 * Throws std::invalid_argument unless section size is finite and positive,
		 * resolution is positive, and the Buildings cache directory is nonempty and
		 * absolute. Construction does not require that directory to exist and performs
		 * no cache file operations. The spline policy span is copied; its index-zero entry
		 * is reserved and cannot be resolved by TryGetSplinePolicy.
		 *
		 * @param sectionSizeMapUnits Width and height of each square cell in map units.
		 * @param sectionResolutionPixels Width and height of each future square image in pixels.
		 * @param buildingsCacheDirectory Already-resolved absolute Buildings sprite-cache directory.
		 * @param splinePolicies Direct-indexed policies with reserved entry zero.
		 */
		BuildingMapService(
			double sectionSizeMapUnits,
			std::uint32_t sectionResolutionPixels,
			std::filesystem::path buildingsCacheDirectory,
			std::span<const SplinePolicy> splinePolicies);
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

	private:
		friend class BuildingMapViewport;
		friend class Scheduling::SectionSchedulingCoordinator;

		/** Retains one publication with an observation borrowing its ordinal storage.
		 * Observation spans remain valid only while PublicationOwner remains retained.
		 */
		struct SectionCandidateSource
		{
			/** Owner of the publication that stores the observation's borrowed ordinals. */
			std::shared_ptr<const BuildingCandidatePublication> PublicationOwner;
			/** Exact Section observation borrowing only from PublicationOwner. */
			BuildingCandidateObservation Observation{};

			/** Clears the owner and all borrowed observation views. */
			void Reset() noexcept;
		};

		/** Finds a service-owned section; columns increase rightward and rows downward. */
		const BuildingMapSection* FindSectionAt(
			std::int64_t gridX,
			std::int64_t gridY) const override;

		/** Releases a live viewport handle without affecting shared section state. */
		void CloseViewport(std::uint64_t viewportHandle) override;

		/** Routes one completed-image availability change to registered subscribed viewports that contain it.
		 * Delivery follows handle registration order and carries no Section or image ownership.
		 * @param gridX Signed global Section column whose completed-image state changed.
		 * @param gridY Signed global Section row whose completed-image state changed.
		 */
		void NotifySectionImageChangedToViewports(std::int64_t gridX, std::int64_t gridY);

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
			std::int64_t endGridY) override;

		/**
		 * Observes candidate evidence for the current grid without certifying Sections.
		 *
		 * @param viewportHandle Opaque registration handle owned by the viewport.
		 * @return True only if scheduler-certified final membership changed.
		 */
		bool RefreshViewportSections(std::uint64_t viewportHandle) override;

		/** Creates a service-owned section only when an internal producer needs it. */
		BuildingMapSection& GetOrCreateSectionAt(std::int64_t gridX, std::int64_t gridY);
		/** Begins one coordinator-owned SectionScheduler session for the current game-world session. */
		bool BeginCaptureGameWorldSession(std::uint64_t sessionGeneration);
		/** Advances coordinator-delegated shutdown only until native retirement completes. */
		bool AdvanceCaptureGameWorldSessionShutdown();
		/** Advances one internal coordinator Section-processing tick.
		 * A future runtime caller must invoke this only during an admitted active capture session;
		 * no host runtime caller is connected yet.
		 */
		void AdvanceSectionScheduling();

		/** Detects viewport publication owner changes and requeues each affected current range.
		 * The temporary publication owner is released before this operation returns.
		 */
		void ObserveViewportCandidatePublicationChanges();

		/** Checks whether any valid active viewport's one-cell-expanded range contains a Section coordinate.
		 * @param gridX Signed global Section column to check.
		 * @param gridY Signed global Section row to check.
		 * @return True when at least one current viewport keeps the coordinate resident.
		 */
		bool IsSectionWithinAnyViewportResidencyRange(std::int64_t gridX, std::int64_t gridY) const;

		/** Releases completed images outside all active viewport residency ranges without erasing Sections. */
		void ReconcileSectionImageResidency();

		/** Obtains one candidate batch from one retained publication without final certification.
		 * @param buildingCollector Associated collector for this viewport.
		 * @param observationGrid Requested row-major map-space grid.
		 * @param columnCount Number of rightward cells in each row.
		 * @param rowCount Number of downward rows.
		 */
		void ObserveCandidateBatch(
			IBuildingCollector& buildingCollector,
			const BuildingSectionGrid& observationGrid,
			std::size_t columnCount,
			std::size_t rowCount);

		/** Selects one live viewport publication for the requested signed Section coordinate.
		 * Prefers complete source evidence and keeps first registration order for ties. Incomplete
		 * evidence remains visible in the borrowed observation for later readiness decisions.
		 * @param gridX Signed Section column to select.
		 * @param gridY Signed Section row to select.
		 * @param output Receives a retained publication and its same-publication observation.
		 * @return True when a current eligible publication covers the exact target cell.
		 */
		bool TrySelectCandidatePublicationForSection(
			std::int64_t gridX,
			std::int64_t gridY,
			SectionCandidateSource& output) const;

		/** Considers one registered viewport without combining its publication with another.
		 * @param viewportHandle Registration key used as the deterministic tie order.
		 * @param viewport Live service registration to inspect.
		 * @param gridX Signed target Section column.
		 * @param gridY Signed target Section row.
		 * @param sectionRegion Exact target map-space cell.
		 * @param observationGrid One-cell grid requested from the publication.
		 * @param selectedCandidate Retains the best same-publication owner and observation so far.
		 * @param selectedEvidenceRank Quality tier of the retained candidate.
		 */
		void ConsiderViewportCandidateForSection(
			std::uint64_t viewportHandle,
			BuildingMapViewport* viewport,
			std::int64_t gridX,
			std::int64_t gridY,
			const BuildingCollectionRegion& sectionRegion,
			const BuildingSectionGrid& observationGrid,
			SectionCandidateSource& selectedCandidate,
			int& selectedEvidenceRank) const;

		/** Checks that accepted requested coverage contains one complete half-open Section cell. */
		static bool HasRequestedCoverageForSection(
			const BuildingCandidateData& candidateData,
			const BuildingCollectionRegion& sectionRegion) noexcept;

		/** Confirms that a generated observation describes the exact selected Section cell. */
		static bool IsObservationForSection(
			const BuildingCandidateObservation& observation,
			const BuildingCollectionRegion& sectionRegion) noexcept;

		const double sectionSizeMapUnits_;
		const std::uint32_t sectionResolutionPixels_;
		const std::vector<SplinePolicy> splinePolicies_;
		std::map<std::pair<std::int64_t, std::int64_t>, std::unique_ptr<BuildingMapSection>> sections_;
		std::map<std::uint64_t, BuildingMapViewport*> viewportsByHandle_;
		std::uint64_t nextViewportHandle_ = 1;
		std::uint64_t lastCaptureSessionGeneration_ = 0;
		bool sectionImageResidencyReconciliationPending_ = false;
		std::unique_ptr<Scheduling::SectionSchedulingCoordinator> sectionSchedulingCoordinator_;
	};
}
