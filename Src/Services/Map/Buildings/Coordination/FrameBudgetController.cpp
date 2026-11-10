// Implements game-independent steady-clock tick estimation and bounded Debug reporting data.
#include "FrameBudgetController.h"

#include <algorithm>
#include <cmath>

namespace Services::Map::Buildings::Coordination
{
	/** Reads monotonic wall time, advances tick identity, and updates the provisional allowance.
	 * @param engineDeltaSeconds Engine delta retained for Debug diagnostics only.
	 */
	void FrameBudgetController::OnTick(double engineDeltaSeconds) noexcept
	{
		const auto now = std::chrono::steady_clock::now();
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		FinalizeCurrentTickDiagnostics();
#endif
		++tickSequence_;
		currentTickAdvancements_ = 0;
		double intervalMs = 0.0;
		const bool hasInterval = previousTickTimestampAvailable_;
		if (hasInterval)
		{
			intervalMs = std::chrono::duration<double, std::milli>(now - previousTickTimestamp_).count();
		}
		previousTickTimestamp_ = now;
		previousTickTimestampAvailable_ = true;
		const bool validInterval = hasInterval && intervalMs >= 1.0 && intervalMs <= 100.0;
		if (validInterval)
		{
			UpdateTimingEstimate(intervalMs);
		}
		UpdateNominalBudget();
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		RecordTickDiagnostics(engineDeltaSeconds, intervalMs, hasInterval, validInterval);
#else
		(void)engineDeltaSeconds;
#endif
	}

	/** Counts a scheduling advancement without changing or replenishing the tick allowance. */
	void FrameBudgetController::RecordSchedulingAdvancement() noexcept
	{
		++currentTickAdvancements_;
	}

	/** Returns the allowance calculated once for the most recently observed tick. */
	double FrameBudgetController::NominalBudgetMs() const noexcept
	{
		return nominalBudgetMs_;
	}

	/** Returns this controller owner's monotonically advancing notification count. */
	std::uint64_t FrameBudgetController::TickSequence() const noexcept
	{
		return tickSequence_;
	}

	/** Applies the provisional exponential smoothing rule to a valid wall-clock interval. */
	void FrameBudgetController::UpdateTimingEstimate(double intervalMs) noexcept
	{
		if (!validTimingEstimateAvailable_)
		{
			smoothedTickMs_ = intervalMs;
			validTimingEstimateAvailable_ = true;
			return;
		}
		const double alpha = 1.0 - std::exp(-intervalMs / 500.0);
		smoothedTickMs_ += alpha * (intervalMs - smoothedTickMs_);
	}

	/** Caps effective tick duration at the provisional reference before applying twelve percent. */
	void FrameBudgetController::UpdateNominalBudget() noexcept
	{
		const double effectiveTickMs = (std::min)(smoothedTickMs_, ReferenceTickMs);
		nominalBudgetMs_ = BudgetFraction * effectiveTickMs;
	}

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
	/** Finalizes advancement totals for the previous tick before checking interval closure. */
	void FrameBudgetController::FinalizeCurrentTickDiagnostics() noexcept
	{
		if (currentTickAdvancements_ > diagnostics_.maximumAdvancementsInOneTick)
		{
			diagnostics_.maximumAdvancementsInOneTick = currentTickAdvancements_;
		}
		if (currentTickAdvancements_ > 1)
		{
			++diagnostics_.ticksWithMultipleAdvancements;
		}
	}

	/** Adds the current notification, valid interval and calculated allowance to Debug aggregates. */
	void FrameBudgetController::RecordTickDiagnostics(
		double engineDeltaSeconds,
		double intervalMs,
		bool hasInterval,
		bool validInterval) noexcept
	{
		const auto now = previousTickTimestamp_;
		if (!diagnosticsIntervalStarted_)
		{
			diagnosticsIntervalStart_ = now;
			diagnosticsIntervalStarted_ = true;
		}
		++diagnostics_.processingTicks;
		diagnostics_.engineDeltaTotalSeconds += engineDeltaSeconds;
		diagnostics_.nominalBudgetTotalMs += nominalBudgetMs_;
		if (nominalBudgetMs_ > diagnostics_.maximumNominalBudgetMs)
		{
			diagnostics_.maximumNominalBudgetMs = nominalBudgetMs_;
		}
		if (!hasInterval)
		{
			return;
		}
		if (validInterval)
		{
			++diagnostics_.validIntervals;
			diagnostics_.validIntervalTotalMs += intervalMs;
			if (intervalMs > diagnostics_.maximumValidIntervalMs)
			{
				diagnostics_.maximumValidIntervalMs = intervalMs;
			}
		}
		else
		{
			++diagnostics_.rejectedIntervals;
		}
	}

	/** Returns and resets only completed Debug interval aggregates after ten wall-clock seconds. */
	bool FrameBudgetController::TakeDiagnosticsSnapshot(DiagnosticsSnapshot& output) noexcept
	{
		if (!diagnosticsIntervalStarted_ || diagnostics_.processingTicks == 0)
		{
			return false;
		}
		const auto now = std::chrono::steady_clock::now();
		const double intervalDurationMs = std::chrono::duration<double, std::milli>(
			now - diagnosticsIntervalStart_).count();
		if (intervalDurationMs < 10000.0)
		{
			return false;
		}
		output.IntervalDurationMs = intervalDurationMs;
		output.ProcessingTickCount = diagnostics_.processingTicks;
		output.ValidObservedIntervalCount = diagnostics_.validIntervals;
		output.RejectedIntervalCount = diagnostics_.rejectedIntervals;
		output.MeanValidTickIntervalMs = diagnostics_.validIntervals == 0
			? 0.0
			: diagnostics_.validIntervalTotalMs / static_cast<double>(diagnostics_.validIntervals);
		output.MaximumValidTickIntervalMs = diagnostics_.maximumValidIntervalMs;
		output.MeanEngineDeltaSeconds = diagnostics_.processingTicks == 0
			? 0.0
			: diagnostics_.engineDeltaTotalSeconds / static_cast<double>(diagnostics_.processingTicks);
		output.LatestSmoothedTickIntervalMs = smoothedTickMs_;
		output.LatestObservedTPS = 1000.0 / smoothedTickMs_;
		output.MeanNominalBudgetMs = diagnostics_.processingTicks == 0
			? 0.0
			: diagnostics_.nominalBudgetTotalMs / static_cast<double>(diagnostics_.processingTicks);
		output.MaximumNominalBudgetMs = diagnostics_.maximumNominalBudgetMs;
		output.LatestNominalBudgetMs = nominalBudgetMs_;
		output.MaximumSchedulingAdvancementsInOneTick = diagnostics_.maximumAdvancementsInOneTick;
		output.TicksWithMultipleAdvancements = diagnostics_.ticksWithMultipleAdvancements;
		ResetDiagnosticsAggregates();
		diagnosticsIntervalStart_ = now;
		return true;
	}

	/** Clears diagnostic counters while leaving tick timing and current budget untouched. */
	void FrameBudgetController::ResetDiagnosticsAggregates() noexcept
	{
		diagnostics_ = {};
	}
#endif
}
