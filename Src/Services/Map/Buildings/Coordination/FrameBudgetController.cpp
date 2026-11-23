// Implements game-independent steady-clock tick estimation and bounded Debug reporting data.
#include "FrameBudgetController.h"

#include <algorithm>
#include <cmath>

namespace Services::Map::Buildings::Coordination
{
	/** Reads raw and corrected monotonic intervals, then updates the adaptive allowance.
	 * @param engineDeltaSeconds Engine delta retained for Debug diagnostics only.
	 */
	void FrameBudgetController::OnTick(double engineDeltaSeconds) noexcept
	{
		const auto now = std::chrono::steady_clock::now();
		const double schedulingTimeRemovedMs = pendingSchedulingExecutionMs_;
		pendingSchedulingExecutionMs_ = 0.0;
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
		double correctedIntervalMs = 0.0;
		bool validCorrectedInterval = false;
		if (validInterval)
		{
			UpdateTimingEstimate(intervalMs);
			correctedIntervalMs = intervalMs - schedulingTimeRemovedMs;
			validCorrectedInterval = std::isfinite(correctedIntervalMs) &&
				correctedIntervalMs >= 1.0 && correctedIntervalMs <= 100.0;
			if (validCorrectedInterval)
			{
				UpdateCorrectedTimingEstimate(correctedIntervalMs);
			}
		}
		UpdateNominalBudget();
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		RecordTickDiagnostics(
			engineDeltaSeconds,
			intervalMs,
			hasInterval,
			validInterval,
			validCorrectedInterval ? correctedIntervalMs : 0.0,
			schedulingTimeRemovedMs);
#else
		(void)engineDeltaSeconds;
#endif
	}

	/** Counts a scheduling advancement without changing or replenishing the tick allowance. */
	void FrameBudgetController::RecordSchedulingAdvancement() noexcept
	{
		++currentTickAdvancements_;
	}

	/** Accumulates one outer coordinator duration for subtraction at the next tick sample. */
	void FrameBudgetController::RecordSchedulingExecutionTime(double executionMs) noexcept
	{
		if (std::isfinite(executionMs) && executionMs > 0.0)
		{
			pendingSchedulingExecutionMs_ += executionMs;
		}
	}

	/** Returns the adaptive allowance established at construction and updated once per tick. */
	double FrameBudgetController::NominalBudgetMs() const noexcept
	{
		return nominalBudgetMs_;
	}

	/** Applies the interactive fraction and cap to the unchanged corrected estimate. */
	double FrameBudgetController::InteractiveNominalBudgetMs() const noexcept
	{
		return (std::min)(15.0, 0.30 * smoothedCorrectedTickMs_);
	}

	/** Returns this controller owner's monotonically advancing notification count. */
	std::uint64_t FrameBudgetController::TickSequence() const noexcept
	{
		return tickSequence_;
	}

	/** Smooths valid raw tick intervals for observed-TPS diagnostics. */
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

	/** Smooths a valid corrected interval using the same exponential response. */
	void FrameBudgetController::UpdateCorrectedTimingEstimate(double intervalMs) noexcept
	{
		if (!validCorrectedTimingEstimateAvailable_)
		{
			smoothedCorrectedTickMs_ = intervalMs;
			validCorrectedTimingEstimateAvailable_ = true;
			return;
		}
		const double alpha = 1.0 - std::exp(-intervalMs / 500.0);
		smoothedCorrectedTickMs_ += alpha * (intervalMs - smoothedCorrectedTickMs_);
	}

	/** Applies the target fraction to corrected duration and enforces the maximum allowance. */
	void FrameBudgetController::UpdateNominalBudget() noexcept
	{
		nominalBudgetMs_ = (std::min)(
			MaximumSchedulingAllowanceMs,
			TargetSchedulingFraction * smoothedCorrectedTickMs_);
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

	/** Adds raw interval, accepted correction, removed work and allowance to Debug aggregates. */
	void FrameBudgetController::RecordTickDiagnostics(
		double engineDeltaSeconds,
		double intervalMs,
		bool hasInterval,
		bool validInterval,
		double correctedIntervalMs,
		double schedulingTimeRemovedMs) noexcept
	{
		const auto now = previousTickTimestamp_;
		if (!diagnosticsIntervalStarted_)
		{
			diagnosticsIntervalStart_ = now;
			diagnosticsIntervalStarted_ = true;
		}
		++diagnostics_.processingTicks;
		diagnostics_.engineDeltaTotalSeconds += engineDeltaSeconds;
		latestRawTickIntervalMs_ = hasInterval ? intervalMs : 0.0;
		latestCorrectedTickIntervalMs_ = correctedIntervalMs;
		latestSchedulingTimeRemovedMs_ = schedulingTimeRemovedMs;
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
		output.LatestRawTickIntervalMs = latestRawTickIntervalMs_;
		output.LatestCorrectedTickIntervalMs = latestCorrectedTickIntervalMs_;
		output.LatestCorrectedSmoothedTickIntervalMs = smoothedCorrectedTickMs_;
		output.LatestSchedulingTimeRemovedMs = latestSchedulingTimeRemovedMs_;
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

	/** Clears diagnostic counters while leaving operational tick timing untouched. */
	void FrameBudgetController::ResetDiagnosticsAggregates() noexcept
	{
		diagnostics_ = {};
	}
#endif
}
