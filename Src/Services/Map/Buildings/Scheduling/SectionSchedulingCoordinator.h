// Owns multi-viewport Section demand and the single SectionScheduler resource owner.
#pragma once

#include <cstdint>
#include <cstddef>
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
#include <array>
#include <chrono>
#endif
#include <filesystem>
#include <map>
#include <memory>
#include <set>

namespace Services::Map::Buildings
{
	class BuildingMapService;
}

namespace Services::Map::Buildings::Capture
{
	class BuildingSpriteRenderer;
}

namespace Services::Map::Buildings::Scheduling
{
	class SectionScheduler;

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
	/** Numeric interval report for coordinator-owned V2 Section scheduling work. */
	struct SectionSchedulingDiagnosticsSnapshot
	{
		/** Repeated elapsed CPU wall-time aggregate; MeanMs is derived by the reporter. */
		struct Duration
		{
			std::uint64_t Samples = 0;
			double TotalMs = 0.0;
			double MaxMs = 0.0;

			/** Adds one elapsed CPU wall-time sample to this numeric aggregate. */
			void Add(double elapsedMs) noexcept
			{
				++Samples;
				TotalMs += elapsedMs;
				if (elapsedMs > MaxMs)
				{
					MaxMs = elapsedMs;
				}
			}
		};

		/** Numeric evidence for the slowest actual Section attempt in the interval. */
		struct WorstSection
		{
			bool Present = false;
			std::int64_t GridX = 0;
			std::int64_t GridY = 0;
			double TotalMs = 0.0;
			double SourceSelectionMs = 0.0;
			double KnownSpritePreparationMs = 0.0;
			double RenderAndCommitMs = 0.0;
			double ScenePreparationMs = 0.0;
			double NumericalRenderingMs = 0.0;
			std::uint8_t CommitStatus = 0;
			bool HasSceneReadiness = false;
			std::uint8_t SceneReadiness = 0;
			bool HasNumericalStatus = false;
			std::uint8_t NumericalStatus = 0;
		};

		double IntervalMs = 0.0;
		Duration CoordinatorAdvancement;
		Duration SpriteAcquisitionAdvancement;
		Duration SourceSelection;
		Duration KnownSpritePreparation;
		Duration RenderAndCommit;
		Duration ScenePreparation;
		Duration SceneBorrow;
		Duration NumericalRendering;
		Duration TransparencyCheck;
		Duration ImageAdoption;
		Duration WholeSectionAttempt;
		std::uint64_t AdvancementCalls = 0;
		std::uint64_t InitialWaitingSections = 0;
		std::uint64_t WaitingAfterAdvancement = 0;
		std::uint64_t SnapshotCoordinatesExamined = 0;
		std::uint64_t ProcessingAttempts = 0;
		std::uint64_t NoLongerEligibleCoordinates = 0;
		std::uint64_t SourceSelectionMisses = 0;
		std::array<std::uint64_t, 6> CommitStatusCounts{};
		std::array<std::uint64_t, 9> SceneReadinessCounts{};
		std::array<std::uint64_t, 6> NumericalStatusCounts{};
		std::uint64_t ExistingImagesRemoved = 0;
		std::uint64_t ImageChangeNotificationsSent = 0;
		WorstSection Worst;
	};
#endif

	/** Tracks deduplicated Section work required by active viewports.
	 * It owns one SectionScheduler but neither Section objects nor per-Section render resources.
	 * Waiting coordinates persist until demand is withdrawn or an explicit completed-work report
	 * removes them. Its Section tick synchronously processes waiting Sections. The renderer's
	 * lifecycle callback advances native request retirement independently of Section work.
	 * Normal Section scheduling remains externally controlled.
	 */
	class SectionSchedulingCoordinator final
	{
	public:
		/** Identifies a signed 64-bit cell in the service-owned row-major Section grid. */
		struct SectionCoordinate
		{
			/** Signed grid column, increasing rightward. */
			std::int64_t GridX = 0;
			/** Signed grid row, increasing downward. */
			std::int64_t GridY = 0;

			/** Orders coordinate keys for set storage only; it does not assign processing priority. */
			bool operator<(const SectionCoordinate& other) const noexcept;
		};

		/** Creates the owned SectionScheduler from service-validated immutable map settings.
		 * @param sectionSizeMapUnits Width and height of each square Section in map units.
		 * @param sectionResolutionPixels Width and height of the future Section image in pixels.
		 * @param buildingsCacheDirectory Service-validated absolute sprite-cache directory.
		 */
		SectionSchedulingCoordinator(
			double sectionSizeMapUnits,
			std::uint32_t sectionResolutionPixels,
			std::filesystem::path buildingsCacheDirectory);
		SectionSchedulingCoordinator(const SectionSchedulingCoordinator&) = delete;
		SectionSchedulingCoordinator& operator=(const SectionSchedulingCoordinator&) = delete;
		SectionSchedulingCoordinator(SectionSchedulingCoordinator&&) = delete;
		SectionSchedulingCoordinator& operator=(SectionSchedulingCoordinator&&) = delete;
		/** Destroys the owned SectionScheduler before coordinator bookkeeping is destroyed. */
		~SectionSchedulingCoordinator();

		/** Replaces one viewport's half-open grid demand and updates the deduplicated waiting set.
		 * @param firstGridX Inclusive first signed grid column.
		 * @param firstGridY Inclusive first signed grid row.
		 * @param endGridX Exclusive end signed grid column.
		 * @param endGridY Exclusive end signed grid row.
		 * @return True when the nonempty half-open range is valid.
		 */
		bool AddViewportDemand(
			std::int64_t firstGridX,
			std::int64_t firstGridY,
			std::int64_t endGridX,
			std::int64_t endGridY);

		/** Applies only the difference between the viewport's previous and accepted new ranges.
		 * @param previousFirstGridX Previous inclusive first column.
		 * @param previousFirstGridY Previous inclusive first row.
		 * @param previousEndGridX Previous exclusive end column.
		 * @param previousEndGridY Previous exclusive end row.
		 * @param nextFirstGridX Accepted new inclusive first column.
		 * @param nextFirstGridY Accepted new inclusive first row.
		 * @param nextEndGridX Accepted new exclusive end column.
		 * @param nextEndGridY Accepted new exclusive end row.
		 * @return True when both ranges are valid nonempty half-open rectangles.
		 */
		bool ChangeViewportDemand(
			std::int64_t previousFirstGridX,
			std::int64_t previousFirstGridY,
			std::int64_t previousEndGridX,
			std::int64_t previousEndGridY,
			std::int64_t nextFirstGridX,
			std::int64_t nextFirstGridY,
			std::int64_t nextEndGridX,
			std::int64_t nextEndGridY);

		/** Withdraws one viewport's existing half-open range and preserves overlapping demand.
		 * @param firstGridX Inclusive first signed grid column.
		 * @param firstGridY Inclusive first signed grid row.
		 * @param endGridX Exclusive end signed grid column.
		 * @param endGridY Exclusive end signed grid row.
		 */
		void WithdrawViewportDemand(
			std::int64_t firstGridX,
			std::int64_t firstGridY,
			std::int64_t endGridX,
			std::int64_t endGridY);
		/** Clears all viewport demand counts and waiting coordinates, retaining the scheduler. */
		void ClearViewportDemand() noexcept;

		/** Requeues demanded work after a Section dependency changes.
		 * @param coordinate Signed service-grid coordinate requiring regeneration.
		 */
		void RequestSectionWork(const SectionCoordinate& coordinate);

		/** Delegates the existing acquisition-session start to the owned scheduler.
		 * @return True when the owned BuildingScheduler establishes that session.
		 */
		bool BeginCaptureGameWorldSession();

		/** Ends gameplay scheduling and requests cancellation of active capture work once. */
		void StopCaptureGameWorldSession();
		/** Returns the capturer target used by the plugin's direct engine callback. */
		Capture::BuildingSpriteRenderer* GetCapturerLifecycleTarget() noexcept;
		/** Delegates explicit building-cache clearing to the owned SectionScheduler.
		 * @return True when the configured Buildings subtree is cleared without error.
		 */
		bool ClearCache();
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		/** Hands off one completed nonempty interval and resets only diagnostic aggregates. */
		bool TakeDiagnosticsSnapshot(SectionSchedulingDiagnosticsSnapshot& output) noexcept;
#endif

	private:
		friend class ::Services::Map::Buildings::BuildingMapService;

		/** Advances acquisition once and processes each coordinate in the initial waiting snapshot.
		 * @param buildingMapService Service-owned Section/source provider for synchronous attempts.
		 */
		void AdvanceWaitingSections(::Services::Map::Buildings::BuildingMapService& buildingMapService);

		/** Processes one snapshot coordinate only while it remains demanded and waiting.
		 * @param buildingMapService Service that selects and owns the Section/source.
		 * @param coordinate Signed grid coordinate from the initial tick snapshot.
		 */
		void ProcessWaitingSection(
			::Services::Map::Buildings::BuildingMapService& buildingMapService,
			const SectionCoordinate& coordinate);

		/** Tests whether one signed coordinate belongs to a half-open rectangular range.
		 * @param firstGridX Inclusive first grid column.
		 * @param firstGridY Inclusive first grid row.
		 * @param endGridX Exclusive end grid column.
		 * @param endGridY Exclusive end grid row.
		 * @param coordinate Signed grid coordinate to test.
		 * @return True when coordinate lies within both half-open axes.
		 */
		static bool ContainsCoordinate(
			std::int64_t firstGridX,
			std::int64_t firstGridY,
			std::int64_t endGridX,
			std::int64_t endGridY,
			const SectionCoordinate& coordinate) noexcept;

		/** Validates a nonempty half-open range before applying coordinate deltas. */
		static bool IsValidDemandRange(
			std::int64_t firstGridX,
			std::int64_t firstGridY,
			std::int64_t endGridX,
			std::int64_t endGridY) noexcept;

		/** Adds each coordinate in a half-open range to aggregate viewport demand. */
		void AddDemandRange(
			std::int64_t firstGridX,
			std::int64_t firstGridY,
			std::int64_t endGridX,
			std::int64_t endGridY);

		/** Removes each coordinate in a half-open range from aggregate viewport demand. */
		void RemoveDemandRange(
			std::int64_t firstGridX,
			std::int64_t firstGridY,
			std::int64_t endGridX,
			std::int64_t endGridY);

		/** Adds one viewport reference and queues newly demanded incomplete work. */
		void AddDemandCoordinate(const SectionCoordinate& coordinate);

		/** Removes one viewport reference and withdraws work after the final demand leaves. */
		void RemoveDemandCoordinate(const SectionCoordinate& coordinate);

		/** Reports whether any active viewport currently requires this coordinate. */
		bool IsDemanded(const SectionCoordinate& coordinate) const noexcept;

		/** Removes completed work after a SectionScheduler commit reports success.
		 * Aggregate demand remains intact.
		 * @param coordinate Signed service-grid coordinate whose image was committed.
		 */
		void RecordSectionCompleted(const SectionCoordinate& coordinate);
		/** Resets interval counters without changing demand, waiting work, or scheduler state. */
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		void ResetDiagnostics() noexcept;
#endif

		std::map<SectionCoordinate, std::size_t> demandCounts_;
		std::set<SectionCoordinate> waitingSections_;
		std::unique_ptr<SectionScheduler> sectionScheduler_;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		SectionSchedulingDiagnosticsSnapshot diagnostics_{};
		std::chrono::steady_clock::time_point diagnosticsIntervalStart_{};
#endif
	};
}
