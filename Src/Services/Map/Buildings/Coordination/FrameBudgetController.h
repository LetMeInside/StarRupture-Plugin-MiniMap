// Owns game-thread tick timing, a corrected adaptive budget and bounded Debug aggregates.
#pragma once

#include <chrono>
#include <cstdint>

namespace Services::Map::Buildings::Coordination
{
	/** Tracks one service instance's tick timing, corrected estimate and adaptive allowance.
	 * The owning BuildingMapService controls its lifetime; this game-independent value has no
	 * external resources, synchronization, or diagnostic history allocation.
	 */
	class FrameBudgetController final
	{
	public:
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		/** Summarizes one completed wall-clock diagnostic interval in Debug builds. */
		struct DiagnosticsSnapshot
		{
			double IntervalDurationMs = 0.0;
			std::uint64_t ProcessingTickCount = 0;
			std::uint64_t ValidObservedIntervalCount = 0;
			std::uint64_t RejectedIntervalCount = 0;
			double MeanValidTickIntervalMs = 0.0;
			double MaximumValidTickIntervalMs = 0.0;
			double MeanEngineDeltaSeconds = 0.0;
			double LatestSmoothedTickIntervalMs = 0.0;
			double LatestObservedTPS = 0.0;
			double LatestRawTickIntervalMs = 0.0;
			double LatestCorrectedTickIntervalMs = 0.0;
			double LatestCorrectedSmoothedTickIntervalMs = 0.0;
			double LatestSchedulingTimeRemovedMs = 0.0;
			double MeanNominalBudgetMs = 0.0;
			double MaximumNominalBudgetMs = 0.0;
			double LatestNominalBudgetMs = 0.0;
			std::uint64_t MaximumSchedulingAdvancementsInOneTick = 0;
			std::uint64_t TicksWithMultipleAdvancements = 0;
		};
#endif

		/** Creates a controller with its reference-rate adaptive allowance ready immediately. */
		FrameBudgetController() noexcept = default;
		/** Keeps tick identity and timing estimator local to one service lifetime. */
		FrameBudgetController(const FrameBudgetController&) = delete;
		/** Keeps tick identity and timing estimator local to one service lifetime. */
		FrameBudgetController& operator=(const FrameBudgetController&) = delete;
		/** Prevents moving an active clock and tick sequence into another owner. */
		FrameBudgetController(FrameBudgetController&&) = delete;
		/** Prevents moving an active clock and tick sequence into another owner. */
		FrameBudgetController& operator=(FrameBudgetController&&) = delete;

		/** Observes one game-thread tick; engine delta is retained only in Debug aggregates.
		 * @param engineDeltaSeconds Engine-provided delta used solely for diagnostics.
		 */
		void OnTick(double engineDeltaSeconds) noexcept;
		/** Counts one validated service scheduling advancement in the current tick. */
		void RecordSchedulingAdvancement() noexcept;
		/** Adds measured synchronous coordinator time to the current tick interval.
		 * @param executionMs Actual elapsed coordinator time, measured once around its outer call.
		 */
		void RecordSchedulingExecutionTime(double executionMs) noexcept;
		/** Returns the adaptive per-tick nominal allowance in milliseconds. */
		double NominalBudgetMs() const noexcept;
		/** Returns the number of game-thread tick notifications observed by this owner. */
		std::uint64_t TickSequence() const noexcept;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		/** Transfers and resets one completed ten-second Debug aggregate interval.
		 * @param output Receives the aggregate when at least ten seconds and one tick elapsed.
		 * @return True only when a completed interval is copied to output.
		 */
		bool TakeDiagnosticsSnapshot(DiagnosticsSnapshot& output) noexcept;
#endif

	private:
		static constexpr double TargetSchedulingFraction = 0.20;
		static constexpr double MaximumSchedulingAllowanceMs = 10.0;
		static constexpr double ReferenceTPS = 60.0;
		static constexpr double ReferenceTickMs = 1000.0 / ReferenceTPS;

		/** Updates the raw observed-TPS estimate from one valid interval.
		 * @param intervalMs Raw elapsed wall-clock interval in milliseconds.
		 */
		void UpdateTimingEstimate(double intervalMs) noexcept;
		/** Updates the corrected tick-duration estimate using the existing smoothing rule.
		 * @param intervalMs Valid tick duration after measured coordinator time is removed.
		 */
		void UpdateCorrectedTimingEstimate(double intervalMs) noexcept;
		/** Calculates the target-fraction allowance under its fixed maximum. */
		void UpdateNominalBudget() noexcept;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		/** Completes the prior tick's advancement counters before any interval handoff. */
		void FinalizeCurrentTickDiagnostics() noexcept;
		/** Adds one tick and timing observation to the bounded Debug aggregate.
		 * @param engineDeltaSeconds Engine delta retained as diagnostic input.
		 * @param intervalMs Raw elapsed wall-clock interval when one exists.
		 * @param hasInterval Whether a previous tick timestamp existed.
		 * @param validInterval Whether the raw interval is within the accepted range.
		 * @param correctedIntervalMs Corrected interval when it passed validation, otherwise zero.
		 * @param schedulingTimeRemovedMs Actual coordinator time attributed to the interval.
		 */
		void RecordTickDiagnostics(
			double engineDeltaSeconds,
			double intervalMs,
			bool hasInterval,
			bool validInterval,
			double correctedIntervalMs,
			double schedulingTimeRemovedMs) noexcept;
		/** Clears only interval aggregates and retains all operational timing state. */
		void ResetDiagnosticsAggregates() noexcept;

		struct DiagnosticsAggregates
		{
			std::uint64_t processingTicks = 0;
			std::uint64_t validIntervals = 0;
			std::uint64_t rejectedIntervals = 0;
			double validIntervalTotalMs = 0.0;
			double maximumValidIntervalMs = 0.0;
			double engineDeltaTotalSeconds = 0.0;
			double nominalBudgetTotalMs = 0.0;
			double maximumNominalBudgetMs = 0.0;
			std::uint64_t maximumAdvancementsInOneTick = 0;
			std::uint64_t ticksWithMultipleAdvancements = 0;
		};
		DiagnosticsAggregates diagnostics_{};
		std::chrono::steady_clock::time_point diagnosticsIntervalStart_{};
		double latestRawTickIntervalMs_ = 0.0;
		double latestCorrectedTickIntervalMs_ = 0.0;
		double latestSchedulingTimeRemovedMs_ = 0.0;
		bool diagnosticsIntervalStarted_ = false;
#endif
		std::chrono::steady_clock::time_point previousTickTimestamp_{};
		double smoothedTickMs_ = ReferenceTickMs;
		double smoothedCorrectedTickMs_ = ReferenceTickMs;
		double nominalBudgetMs_ = TargetSchedulingFraction * ReferenceTickMs;
		double pendingSchedulingExecutionMs_ = 0.0;
		std::uint64_t tickSequence_ = 0;
		std::uint64_t currentTickAdvancements_ = 0;
		bool previousTickTimestampAvailable_ = false;
		bool validTimingEstimateAvailable_ = false;
		bool validCorrectedTimingEstimateAvailable_ = false;
	};
}
