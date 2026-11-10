// Owns multi-viewport Section demand and the single SectionScheduler resource owner.
#pragma once

#include <chrono>
#include <cstdint>
#include <cstddef>
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
#include <array>
#endif
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <utility>

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
#include "BuildingScheduler.h"
#include "../Rendering/SectionRendering.h"
#endif

namespace Services::Map::Buildings
{
	class BuildingMapService;
	class BuildingMapSection;
	class BuildingCandidatePublication;
}

namespace Services::Map::Buildings::Capture
{
	class BuildingSpriteRenderer;
}

namespace Services::Map::Buildings::Visuals
{
	struct BuildingVisualsTerminalOmissionSummary;
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

		/** Numeric evidence for the slowest measured incremental Section slice. */
		struct WorstSectionSlice
		{
			bool Present = false;
			std::int64_t GridX = 0;
			std::int64_t GridY = 0;
			double ElapsedMs = 0.0;
			std::size_t WorkUnitsConsumed = 0;
			std::uint8_t AdvancementState = 0;
		};

		double IntervalMs = 0.0;
		Duration CoordinatorAdvancement;
		Duration SpriteAcquisitionAdvancement;
		Duration SourceSelection;
		Duration KnownSpritePreparation;
		Duration SectionSlice;
		Duration ScenePreparation;
		Duration SceneBorrow;
		Duration NumericalRendering;
		Duration DestinationConstruction;
		Duration OrdinarySprites;
		Duration OcclusionIndexPreparation;
		Duration SplineDrawing;
		Duration OverlaySprites;
		Duration DestinationFinalization;
		Duration TransparencyCheck;
		Duration ImageAdoption;
		std::uint64_t AdvancementCalls = 0;
		std::uint64_t SectionSlicesAttempted = 0;
		std::uint64_t WorkUnitsConsumed = 0;
		std::uint64_t PendingResults = 0;
		std::uint64_t WaitingForResourcesResults = 0;
		std::uint64_t RestartRequiredResults = 0;
		std::uint64_t AttemptFinishedResults = 0;
		std::uint64_t SuccessfulImageCommits = 0;
		std::uint64_t SuccessfulEmptyCommits = 0;
		std::uint64_t FailedTerminalAttempts = 0;
		std::uint64_t BudgetExhaustionEvents = 0;
		std::uint64_t BudgetOverrunEvents = 0;
		double BudgetOverrunTotalMs = 0.0;
		double MaximumBudgetOverrunMs = 0.0;
		std::uint64_t NoLongerEligibleCoordinates = 0;
		std::uint64_t SourceSelectionMisses = 0;
		std::array<std::uint64_t, 5> CommitStatusCounts{};
		std::array<std::uint64_t, 8> SceneReadinessCounts{};
		std::array<std::uint64_t, 7> NumericalStatusCounts{};
		std::uint64_t ExistingImagesRemoved = 0;
		std::uint64_t ImageChangeNotificationsSent = 0;
		WorstSectionSlice WorstSlice;
	};
#endif

	/** Tracks deduplicated Section work required by active viewports.
	 * It owns one SectionScheduler but neither Section objects nor per-Section render resources.
	 * Waiting coordinates persist until demand is withdrawn or an explicit completed-work report
	 * removes them. Its serialized Section advancement incrementally processes waiting Sections.
	 * The renderer's lifecycle callback advances native request retirement independently of Section work.
	 * BuildingMapService invokes bounded Section advancement on the serialized game-thread path.
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

		/** Service-owned pending publications keyed by their exact grid cell. */
		using SectionPublicationBatch = std::map<
			std::pair<std::int64_t, std::int64_t>,
			std::shared_ptr<const ::Services::Map::Buildings::BuildingCandidatePublication>>;

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
		/** Clears demand counts, waiting coordinates and retained Section jobs while retaining scheduler resources. */
		void ClearViewportDemand() noexcept;

		/** Requeues demanded work after a Section dependency changes.
		 * @param coordinate Signed service-grid coordinate requiring regeneration.
		 */
		void RequestSectionWork(const SectionCoordinate& coordinate);

		/** Delegates the existing acquisition-session start to the owned scheduler.
		 * @return True when the owned BuildingScheduler establishes that session.
		 */
		bool BeginCaptureGameWorldSession();

		/** Clears retained Section jobs, then stops scheduling and requests active capture cancellation. */
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
		/** Forwards the independent BuildingScheduler acquisition interval without combining it.
		 * @param output Receives the completed sprite-acquisition interval when due.
		 * @return True when the SectionScheduler forwards a completed interval.
		 */
		bool TakeSpriteAcquisitionDiagnosticsSnapshot(
			BuildingScheduler::DiagnosticsSnapshot& output) noexcept;
#endif

	private:
		static constexpr std::size_t MaximumSectionVisitWorkUnits = 1024;
		friend class ::Services::Map::Buildings::BuildingMapService;
		/** Reports one Section-job state and the exact deterministic quota consumed. */
		struct IncrementalSectionAdvanceResult
		{
			enum class State
			{
				Pending,
				WaitingForResources,
				RestartRequired,
				AttemptFinished
			};

			State CurrentState = State::Pending;
			std::size_t WorkUnitsConsumed = 0;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			bool HasSceneReadiness = false;
			bool SceneBorrowMeasured = false;
			std::uint8_t SceneReadiness = 0;
			bool HasCommitStatus = false;
			std::uint8_t CommitStatus = 0;
			bool HasNumericalStatus = false;
			std::uint8_t NumericalStatus = 0;
			double ScenePreparationMs = 0.0;
			double SceneBorrowMs = 0.0;
			double NumericalRenderingMs = 0.0;
			double TransparencyCheckMs = 0.0;
			double ImageAdoptionMs = 0.0;
			bool NumericalRenderingInvoked = false;
			bool TransparencyCheckInvoked = false;
			bool ImageAdoptionAttempted = false;
			Visuals::BuildingVisualsPhaseTimings NumericalPhases;
#endif
		};
		/** Collapses existing terminal commit statuses for shared successful-completion handling. */
		enum class SectionCommitOutcome
		{
			Failed,
			ImageCommitted,
			EmptyCommitted
		};

		/** Advances sprite acquisition once and shares the service's current tick allowance across waiting Sections.
		 * @param buildingMapService Service-owned Section/source and frame-budget provider.
		 * @param publications Service-owned selected publications, copied into local owners before job advancement.
		 */
		void AdvanceWaitingSections(
			::Services::Map::Buildings::BuildingMapService& buildingMapService,
			const SectionPublicationBatch& publications);
		/** Starts a fresh allowance only when the service-owned tick identity advances. */
		void InitializeTickBudget(
			const ::Services::Map::Buildings::BuildingMapService& buildingMapService);
		/** Advances the single existing sprite-acquisition path once for this coordinator call. */
		void AdvanceSpriteAcquisitionOnce();

		/** Copies the next waiting coordinate after the value cursor, skipping current pass and per-tick deferred work. */
		bool TrySelectNextWaitingSection(
			SectionCoordinate& output,
			const std::set<SectionCoordinate>& visitedThisPass) const;
		/** Advances eligible jobs in rotated order until time or per-tick deferral stops useful work. */
		void AdvanceWaitingSectionSlices(
			::Services::Map::Buildings::BuildingMapService& buildingMapService,
			const SectionPublicationBatch& publications,
			std::chrono::steady_clock::time_point advancementStarted);
		/** Advances one selected coordinate once and reports whether it must wait until the next tick. */
		bool AdvanceOneWaitingSection(
			::Services::Map::Buildings::BuildingMapService& buildingMapService,
			const SectionCoordinate& coordinate,
			const SectionPublicationBatch& publications);
		/** Tests the shared allowance using prior same-tick consumption and this call's elapsed time. */
		bool IsTickAllowanceAvailable(
			std::chrono::steady_clock::time_point advancementStarted) const noexcept;
		/** Adds this advancement's wall duration to per-tick consumption before returning. */
		void FinalizeTickBudget(
			std::chrono::steady_clock::time_point advancementStarted
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			, bool budgetWasAlreadyExhausted
#endif
			);
		/** Clears value-only round-robin and shared-budget state at full demand/session reset boundaries. */
		void ResetSchedulingBudgetState() noexcept;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		/** Records one actual incremental state and terminal commit evidence in Debug aggregates. */
		void RecordIncrementalDiagnostics(
			const SectionCoordinate& coordinate,
			double sliceElapsedMs,
			const IncrementalSectionAdvanceResult& result) noexcept;
		/** Adds actual terminal commit and cumulative active-operation phase evidence once. */
		void RecordTerminalAttemptDiagnostics(const IncrementalSectionAdvanceResult& result) noexcept;
		/** Counts only terminal commit and numerical status values. */
		void RecordTerminalStatusDiagnostics(const IncrementalSectionAdvanceResult& result) noexcept;
		/** Adds only durations actually carried by a terminal incremental attempt. */
		void RecordTerminalTimingDiagnostics(const IncrementalSectionAdvanceResult& result) noexcept;
		/** Adds one invoked numerical phase's cumulative active duration to its interval aggregate. */
		static void AddPhaseDuration(
			const Visuals::BuildingVisualsPhaseTimings::Phase& phase,
			SectionSchedulingDiagnosticsSnapshot::Duration& aggregate) noexcept;
#endif
		/** Advances one selected publication job once from the active round-robin waiting loop.
		 * @param buildingMapService Service-owned Section and immutable spline-policy provider.
		 * @param coordinate Exact signed Section coordinate selected by the future scheduler.
		 * @param publicationOwner Selected shared publication retained through this advancement.
		 * @param maximumWorkUnits Positive deterministic quota passed unchanged to SectionScheduler.
		 * @return Actual job state and work units consumed; zero quota returns Pending without side effects.
		 */
		IncrementalSectionAdvanceResult AdvanceSelectedSectionJob(
			::Services::Map::Buildings::BuildingMapService& buildingMapService,
			const SectionCoordinate& coordinate,
			std::shared_ptr<const ::Services::Map::Buildings::BuildingCandidatePublication> publicationOwner,
			std::size_t maximumWorkUnits);
		/** Prepares fresh sprite views, advances an existing job once, and applies terminal completion events.
		 * @param buildingMapService Service providing the immutable spline policies.
		 * @param coordinate Exact coordinate of the retained Section job.
		 * @param publicationOwner Selected publication retained through this advancement.
		 * @param section Service-owned Section whose numerical context remains stable across calls.
		 * @param maximumWorkUnits Deterministic quota passed unchanged to SectionScheduler.
		 * @return The actual incremental state and work count reported by SectionScheduler.
		 */
		IncrementalSectionAdvanceResult AdvanceRetainedSectionJob(
			::Services::Map::Buildings::BuildingMapService& buildingMapService,
			const SectionCoordinate& coordinate,
			const std::shared_ptr<const ::Services::Map::Buildings::BuildingCandidatePublication>& publicationOwner,
			::Services::Map::Buildings::BuildingMapSection& section,
			std::size_t maximumWorkUnits);
		/** Applies existing service, waiting, omission and viewport events for one terminal commit result.
		 * @param buildingMapService Service owning the selected publication and viewport notifications.
		 * @param coordinate Coordinate of the terminal Section attempt.
		 * @param publicationOwner Selected immutable publication whose hash becomes applied on success.
		 * @param outcome Existing terminal status classified as failed, image committed, or empty committed.
		 * @param completedImageRemoved Whether a confirmed-empty commit removed a prior image.
		 * @param omissions Fixed-size terminal omission evidence from this attempt.
		 */
		void HandleSectionCommitResult(
			::Services::Map::Buildings::BuildingMapService& buildingMapService,
			const SectionCoordinate& coordinate,
			const std::shared_ptr<const ::Services::Map::Buildings::BuildingCandidatePublication>& publicationOwner,
			SectionCommitOutcome outcome,
			bool completedImageRemoved,
			const ::Services::Map::Buildings::Visuals::BuildingVisualsTerminalOmissionSummary& omissions);
		/** Logs one bounded warning after successful commitment of terminal spline omissions. */
		void ReportTerminalSplineOmissions(
			const SectionCoordinate& coordinate,
			const ::Services::Map::Buildings::Visuals::BuildingVisualsTerminalOmissionSummary& omissions) const noexcept;

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

		/** Removes one viewport reference and cancels work after the final demand leaves. */
		void RemoveDemandCoordinate(const SectionCoordinate& coordinate);

		/** Reports whether any active viewport currently requires this coordinate. */
		bool IsDemanded(const SectionCoordinate& coordinate) const noexcept;

		/** Removes completed waiting work and its retained job after a successful commit.
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
		std::set<SectionCoordinate> deferredSectionsThisTick_;
		SectionCoordinate roundRobinCursor_{};
		bool roundRobinCursorAvailable_ = false;
		std::uint64_t budgetTickSequence_ = 0;
		double nominalTickBudgetMs_ = 0.0;
		double consumedSchedulingMs_ = 0.0;
		bool budgetTickAvailable_ = false;
		std::unique_ptr<SectionScheduler> sectionScheduler_;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		SectionSchedulingDiagnosticsSnapshot diagnostics_{};
		std::chrono::steady_clock::time_point diagnosticsIntervalStart_{};
#endif
	};
}
