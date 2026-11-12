#if defined(MODLOADER_CLIENT_BUILD)

// Implements typed building callbacks, production mutation publication, and bounded scalar diagnostics.
#include "MassLifecycle.h"

#include "../Services/NativeHooks.h"
#include "../Services/Runtime.h"

#include <SDK/Chimera_classes.hpp>
#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace
{
	namespace N = MiniMapNative;
	namespace S = MiniMap::Services;
	using BuildingFunction = N::MassLifecycleApi::BuildingNotificationFn;
	using Clock = std::chrono::steady_clock;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
	constexpr std::uint64_t BurstQuietGapNanoseconds = 500'000'000;
	constexpr auto DiagnosticInterval = std::chrono::seconds(10);
#endif
	std::array<S::Hook, 2> HookHandles{};
	std::array<std::atomic<bool>, 2> HookRemovalRequested{};
	std::array<std::atomic<int>, 2> HookRemovalRegistryState{
		static_cast<int>(S::RegistryRemoval::NotRequested),
		static_cast<int>(S::RegistryRemoval::NotRequested)};
	std::array<std::atomic<int>, 2> HookRemovalCoreState{
		static_cast<int>(S::CoreDrain::Unknown),
		static_cast<int>(S::CoreDrain::Unknown)};
	std::atomic<int> CreateInstallState{static_cast<int>(S::HookInstallState::Rejected)};
	std::atomic<int> DestroyInstallState{static_cast<int>(S::HookInstallState::Rejected)};
	std::atomic<bool> InstallationAttempted{false};
	std::atomic<bool> GameplayAdmitted{false};
	std::atomic<std::uintptr_t> CurrentStabilitySubsystem{0};
	std::atomic<std::uint64_t> AcceptedMutationRevision{0};
	std::atomic<std::uint64_t> LatestAcceptedMutationNanoseconds{0};
	std::atomic<BuildingFunction> CreateOriginal{nullptr};
	std::atomic<BuildingFunction> DestroyOriginal{nullptr};

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
	std::atomic<std::uint64_t> AcceptedCreationNotifications{0};
	std::atomic<std::uint64_t> AcceptedDestructionNotifications{0};
	std::atomic<std::uint64_t> WorldMismatches{0};
	std::atomic<std::uint64_t> MissingWorldIdentity{0};
	std::atomic<std::uint64_t> CrossThreadNotifications{0};
	std::atomic<std::uint64_t> NotificationSequence{0};
	std::atomic<std::uint64_t> LatestNotificationNanoseconds{0};
	std::atomic<std::uint64_t> UpdateThreadId{0};
	std::atomic<std::uint64_t> ObservedTickSequence{0};
	std::atomic<std::uint64_t> BurstSampleDrops{0};
	std::atomic<std::uint64_t> BurstTickUnavailableSamples{0};
	std::atomic_flag BurstStateLock = ATOMIC_FLAG_INIT;
	bool HasActiveBurst = false;
	std::uint64_t ActiveBurstCreationCount = 0;
	std::uint64_t ActiveBurstDestructionCount = 0;
	std::uint64_t ActiveBurstFirstTimestamp = 0;
	std::uint64_t ActiveBurstLastTimestamp = 0;
	std::uint64_t ActiveBurstMaximumGap = 0;
	std::uint64_t ActiveBurstFirstTickSequence = 0;
	std::uint64_t ActiveBurstLastTickSequence = 0;
	std::uint64_t ActiveBurstLastDistinctTickSequence = 0;
	std::uint64_t ActiveBurstDistinctTickCount = 0;
	std::uint64_t CompletedBurstCount = 0;
	std::uint64_t MaximumCallbacksPerBurst = 0;
	std::uint64_t MaximumBurstDurationNanoseconds = 0;
	std::uint64_t MaximumIntraBurstGapNanoseconds = 0;
	std::uint64_t MaximumTickSequenceSpan = 0;
	std::uint64_t MaximumDistinctCallbackTicks = 0;
	std::uint64_t LargestBurstCreationCount = 0;
	std::uint64_t LargestBurstDestructionCount = 0;
	std::atomic<std::uint64_t> CollectorUpdateAttemptSequence{0};
	std::atomic<std::uint64_t> LastNotificationSequenceObservedAtUpdate{0};
	std::atomic<std::uint64_t> LastNotificationObservedAtCollectorAttempt{0};
	std::atomic<std::uint64_t> NotificationSequenceAtLastCollectorAttempt{0};
	std::uint64_t lastObservedNotificationSequence = 0;
	std::uint64_t lastReportedAtNanoseconds = 0;
#endif

}

namespace MiniMapMassLifecycle
{
	/** Returns the current steady-clock timestamp as nanoseconds from its fixed epoch. */
	std::uint64_t MassLifecycle::CurrentNanoseconds() noexcept
	{
		return static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
	}

	/** Atomically retains the greater scalar value without taking a callback lock. */
	void MassLifecycle::StoreMaximum(
		std::atomic<std::uint64_t>& destination,
		std::uint64_t value) noexcept
	{
		std::uint64_t observed = destination.load(std::memory_order_acquire);
		while (observed < value && !destination.compare_exchange_weak(
			observed, value, std::memory_order_acq_rel, std::memory_order_acquire))
		{
		}
	}

	/** Names one bounded hook installation status for development reporting. */
	const char* MassLifecycle::HookStateName(int state) noexcept
	{
		if (state == static_cast<int>(S::HookInstallState::Active))
		{
			return "active";
		}
		if (state == static_cast<int>(S::HookInstallState::BackendUnknown))
		{
			return "backend-unknown";
		}
		return "rejected";
	}

	/** Publishes the typed trampoline exactly as returned by the native hook backend. */
	template<typename Function>
	void MassLifecycle::PublishOriginal(void* owner, void* original) noexcept
	{
		static_cast<std::atomic<Function>*>(owner)->store(
			reinterpret_cast<Function>(original), std::memory_order_release);
	}

	/** Installs one verified native target through the existing AlienX hook adapter. */
	template<typename Function>
	bool MassLifecycle::InstallBuildingHook(
		std::size_t slot,
		Function target,
		void* detour,
		std::atomic<Function>& original,
		std::atomic<int>& state)
	{
		auto* hooks = S::TryNativeHooks();
		if (hooks == nullptr || target == nullptr)
		{
			state.store(static_cast<int>(S::HookInstallState::Rejected), std::memory_order_release);
			return false;
		}
		const S::HookInstallation installation = hooks->Install({
			reinterpret_cast<std::uintptr_t>(target),
			detour,
			&MassLifecycle::PublishOriginal<Function>,
			&original});
		HookHandles[slot] = installation.handle;
		state.store(static_cast<int>(installation.state), std::memory_order_release);
		return installation.state == S::HookInstallState::Active;
	}

	/** Installs only the creation and destruction observers after native fingerprint resolution. */
	bool MassLifecycle::Initialize()
	{
		if (InstallationAttempted.exchange(true, std::memory_order_acq_rel))
		{
			return CreateInstallState.load(std::memory_order_acquire) ==
				static_cast<int>(S::HookInstallState::Active) &&
				DestroyInstallState.load(std::memory_order_acquire) ==
				static_cast<int>(S::HookInstallState::Active) &&
				!HookRemovalRequested[0].load(std::memory_order_acquire) &&
				!HookRemovalRequested[1].load(std::memory_order_acquire);
		}
		const auto* native = N::Get();
		if (native == nullptr)
		{
			return false;
		}
		bool createInstalled = false;
		bool destroyInstalled = false;
		try
		{
			createInstalled = InstallBuildingHook(0, native->massLifecycle.onCreate,
				reinterpret_cast<void*>(&MassLifecycle::CreateBuildingDetour), CreateOriginal, CreateInstallState);
			if (createInstalled)
			{
				destroyInstalled = InstallBuildingHook(1, native->massLifecycle.onDestroy,
					reinterpret_cast<void*>(&MassLifecycle::DestroyBuildingDetour), DestroyOriginal, DestroyInstallState);
			}
		}
		catch (...)
		{
			Shutdown();
			return false;
		}
		if (!createInstalled || !destroyInstalled)
		{
			Shutdown();
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			if (auto* log = S::TryLog())
			{
				S::Logf(*log, S::Level::Error,
					"MiniMap: building mutation hook install failed create=%s destroy=%s",
					HookStateName(CreateInstallState.load(std::memory_order_acquire)),
					HookStateName(DestroyInstallState.load(std::memory_order_acquire)));
			}
#endif
			return false;
		}
		return true;
	}

	/** Closes session classification before requesting removal of installed native detours. */
	void MassLifecycle::Shutdown() noexcept
	{
		EndWorld();
		auto* hooks = S::TryNativeHooks();
		if (hooks == nullptr)
		{
			return;
		}
		for (std::size_t slot = 0; slot < HookHandles.size(); ++slot)
		{
			if (HookHandles[slot].id == 0 || HookRemovalRequested[slot].exchange(true, std::memory_order_acq_rel))
			{
				continue;
			}
			try
			{
				const S::Removal removal = hooks->Remove(HookHandles[slot]);
				HookRemovalRegistryState[slot].store(static_cast<int>(removal.registry), std::memory_order_release);
				HookRemovalCoreState[slot].store(static_cast<int>(removal.core), std::memory_order_release);
			}
			catch (...)
			{
			}
		}
	}

	/** Stores only the active world subsystem address for later scalar callback comparison. */
	void MassLifecycle::BeginWorld(SDK::UWorld* world)
	{
		SetGameplayAdmitted(false);
		CurrentStabilitySubsystem.store(0, std::memory_order_release);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		CloseActiveBurst();
#endif
		if (world == nullptr)
		{
			return;
		}
		auto* observer = static_cast<SDK::UCrBuildingStabilitySubsystem*>(
			SDK::USubsystemBlueprintLibrary::GetWorldSubsystem(
				world, SDK::TSubclassOf<SDK::UWorldSubsystem>(
					SDK::UCrBuildingStabilitySubsystem::StaticClass())));
		CurrentStabilitySubsystem.store(reinterpret_cast<std::uintptr_t>(observer), std::memory_order_release);
	}

	/** Changes only the atomic admission bit used to classify later callback observations. */
	void MassLifecycle::SetGameplayAdmitted(bool admitted) noexcept
	{
		GameplayAdmitted.store(admitted, std::memory_order_release);
	}

	/** Reads the two scalar watermarks; the collector handles any concurrent revision/timestamp mismatch conservatively. */
	bool MassLifecycle::TryGetMutationSnapshot(MutationSnapshot& snapshot) noexcept
	{
		snapshot.Revision = AcceptedMutationRevision.load(std::memory_order_acquire);
		snapshot.TimestampNanoseconds = LatestAcceptedMutationNanoseconds.load(std::memory_order_acquire);
		return GameplayAdmitted.load(std::memory_order_acquire);
	}

	/** Ends callback admission and forgets the old subsystem address without dereferencing it. */
	void MassLifecycle::EndWorld() noexcept
	{
		SetGameplayAdmitted(false);
		CurrentStabilitySubsystem.store(0, std::memory_order_release);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		CloseActiveBurst();
#endif
	}

	/** Captures the OS thread identity at the existing renderer/update callback. */
	void MassLifecycle::ObserveUpdateThread() noexcept
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		UpdateThreadId.store(static_cast<std::uint64_t>(GetCurrentThreadId()), std::memory_order_release);
		ObservedTickSequence.fetch_add(1, std::memory_order_relaxed);
#endif
	}

	/** Records the notification watermark at an update-attempt start and advances its sequence. */
	void MassLifecycle::BeginCollectorUpdate() noexcept
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const std::uint64_t sequence = NotificationSequence.load(std::memory_order_acquire);
		const std::uint64_t attemptSequence = CollectorUpdateAttemptSequence.load(std::memory_order_relaxed);
		if (sequence != lastObservedNotificationSequence)
		{
			lastObservedNotificationSequence = sequence;
			LastNotificationSequenceObservedAtUpdate.store(sequence, std::memory_order_release);
			LastNotificationObservedAtCollectorAttempt.store(attemptSequence, std::memory_order_release);
		}
		CollectorUpdateAttemptSequence.fetch_add(1, std::memory_order_relaxed);
		NotificationSequenceAtLastCollectorAttempt.store(sequence, std::memory_order_release);
#endif
	}

	/** Emits cumulative counters and latest-event timing outside native notification callbacks. */
	void MassLifecycle::ReportDiagnosticsIfDue() noexcept
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const std::uint64_t now = CurrentNanoseconds();
		if (lastReportedAtNanoseconds != 0 && now - lastReportedAtNanoseconds <
			static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(DiagnosticInterval).count()))
		{
			return;
		}
		lastReportedAtNanoseconds = now;
		ReportHookDiagnostics();
		ReportNotificationDiagnostics(now);
		ReportBurstDiagnostics();
#endif
	}

	/** Completes the active burst and folds its required maxima into cumulative diagnostics. */
	void MassLifecycle::CompleteActiveBurst() noexcept
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		if (!HasActiveBurst)
		{
			return;
		}
		const std::uint64_t callbackCount = ActiveBurstCreationCount + ActiveBurstDestructionCount;
		const std::uint64_t duration = ActiveBurstLastTimestamp - ActiveBurstFirstTimestamp;
		++CompletedBurstCount;
		if (callbackCount > MaximumCallbacksPerBurst)
		{
			MaximumCallbacksPerBurst = callbackCount;
			LargestBurstCreationCount = ActiveBurstCreationCount;
			LargestBurstDestructionCount = ActiveBurstDestructionCount;
		}
		MaximumBurstDurationNanoseconds = (std::max)(MaximumBurstDurationNanoseconds, duration);
		MaximumIntraBurstGapNanoseconds = (std::max)(MaximumIntraBurstGapNanoseconds, ActiveBurstMaximumGap);
		if (ActiveBurstDistinctTickCount != 0)
		{
			const std::uint64_t tickSpan = ActiveBurstLastTickSequence - ActiveBurstFirstTickSequence + 1;
			MaximumTickSequenceSpan = (std::max)(MaximumTickSequenceSpan, tickSpan);
		}
		MaximumDistinctCallbackTicks = (std::max)(MaximumDistinctCallbackTicks, ActiveBurstDistinctTickCount);
		HasActiveBurst = false;
#endif
	}

	/** Completes an active burst at the existing gameplay world boundary. */
	void MassLifecycle::CloseActiveBurst() noexcept
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		while (BurstStateLock.test_and_set(std::memory_order_acquire))
		{
		}
		CompleteActiveBurst();
		BurstStateLock.clear(std::memory_order_release);
#endif
	}

	/** Records one accepted callback sample without waiting for concurrent diagnostic work. */
	void MassLifecycle::RecordBurstSample(
		SDK::UCrBuildingStabilitySubsystem* subsystem,
		std::uint64_t timestamp,
		std::uint64_t tickSequence,
		bool creation) noexcept
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		if (BurstStateLock.test_and_set(std::memory_order_acquire))
		{
			BurstSampleDrops.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		if (!GameplayAdmitted.load(std::memory_order_acquire) ||
			reinterpret_cast<std::uintptr_t>(subsystem) !=
				CurrentStabilitySubsystem.load(std::memory_order_acquire) ||
				(HasActiveBurst && timestamp < ActiveBurstLastTimestamp) ||
				(HasActiveBurst && tickSequence != 0 && ActiveBurstLastTickSequence != 0 &&
					tickSequence < ActiveBurstLastTickSequence))
		{
			BurstSampleDrops.fetch_add(1, std::memory_order_relaxed);
			BurstStateLock.clear(std::memory_order_release);
			return;
		}
		if (HasActiveBurst)
		{
			const std::uint64_t gap = timestamp - ActiveBurstLastTimestamp;
			if (gap > BurstQuietGapNanoseconds)
			{
				CompleteActiveBurst();
			}
			else
			{
				ActiveBurstMaximumGap = (std::max)(ActiveBurstMaximumGap, gap);
			}
		}
		RecordBurstCallback(timestamp, tickSequence, creation);
		BurstStateLock.clear(std::memory_order_release);
#else
		(void)subsystem;
		(void)timestamp;
		(void)tickSequence;
		(void)creation;
#endif
	}

	/** Updates active callback counts and distinct tick values for one accepted sample. */
	void MassLifecycle::RecordBurstCallback(
		std::uint64_t timestamp,
		std::uint64_t tickSequence,
		bool creation) noexcept
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		if (!HasActiveBurst)
		{
			HasActiveBurst = true;
			ActiveBurstCreationCount = creation ? 1 : 0;
			ActiveBurstDestructionCount = creation ? 0 : 1;
			ActiveBurstFirstTimestamp = timestamp;
			ActiveBurstMaximumGap = 0;
			ActiveBurstFirstTickSequence = tickSequence;
			ActiveBurstLastTickSequence = tickSequence;
			ActiveBurstLastDistinctTickSequence = tickSequence;
			ActiveBurstDistinctTickCount = tickSequence == 0 ? 0 : 1;
		}
		else if (creation)
		{
			++ActiveBurstCreationCount;
		}
		else
		{
			++ActiveBurstDestructionCount;
		}
		ActiveBurstLastTimestamp = timestamp;
		if (tickSequence == 0)
		{
			BurstTickUnavailableSamples.fetch_add(1, std::memory_order_relaxed);
		}
		else if (ActiveBurstFirstTickSequence == 0 && tickSequence != 0)
		{
			ActiveBurstFirstTickSequence = tickSequence;
			ActiveBurstLastTickSequence = tickSequence;
			ActiveBurstLastDistinctTickSequence = tickSequence;
			ActiveBurstDistinctTickCount = 1;
		}
		else if (tickSequence > ActiveBurstLastDistinctTickSequence)
		{
			ActiveBurstLastTickSequence = tickSequence;
			ActiveBurstLastDistinctTickSequence = tickSequence;
			++ActiveBurstDistinctTickCount;
		}
#else
		(void)timestamp;
		(void)tickSequence;
		(void)creation;
#endif
	}

	/** Expires quiet bursts and reports one compact aggregate outside native callbacks. */
	void MassLifecycle::ReportBurstDiagnostics() noexcept
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const std::uint64_t now = CurrentNanoseconds();
		while (BurstStateLock.test_and_set(std::memory_order_acquire))
		{
		}
		if (HasActiveBurst && now - ActiveBurstLastTimestamp >= BurstQuietGapNanoseconds)
		{
			CompleteActiveBurst();
		}
		const bool active = HasActiveBurst;
		const std::uint64_t activeCallbacks = active
			? ActiveBurstCreationCount + ActiveBurstDestructionCount
			: 0;
		const std::uint64_t activeSpan = active ? ActiveBurstLastTimestamp - ActiveBurstFirstTimestamp : 0;
		const std::uint64_t completed = CompletedBurstCount;
		const std::uint64_t maximumCallbacks = MaximumCallbacksPerBurst;
		const std::uint64_t maximumDuration = MaximumBurstDurationNanoseconds;
		const std::uint64_t maximumGap = MaximumIntraBurstGapNanoseconds;
		const std::uint64_t maximumTickSpan = MaximumTickSequenceSpan;
		const std::uint64_t maximumDistinctTicks = MaximumDistinctCallbackTicks;
		const std::uint64_t largestCreate = LargestBurstCreationCount;
		const std::uint64_t largestDestroy = LargestBurstDestructionCount;
		BurstStateLock.clear(std::memory_order_release);
		if (auto* log = S::TryLog())
		{
			S::Logf(*log, S::Level::Info,
				"MiniMap: building_mutation_bursts intervalMs=10000 thresholdMs=%llu "
				"completed=%llu active=%u activeCallbacks=%llu activeObservedSpanMs=%.3f "
				"maxCallbacksPerBurst=%llu maxDurationMs=%.3f maxIntraBurstGapMs=%.3f "
				"maxTickSpan=%llu maxDistinctCallbackTicks=%llu largestBurst={create:%llu destroy:%llu} "
				"sampleDrops=%llu tickSamplesUnavailable=%llu",
				static_cast<unsigned long long>(BurstQuietGapNanoseconds / 1'000'000),
				static_cast<unsigned long long>(completed),
				static_cast<unsigned>(active),
				static_cast<unsigned long long>(activeCallbacks),
				static_cast<double>(activeSpan) / 1'000'000.0,
				static_cast<unsigned long long>(maximumCallbacks),
				static_cast<double>(maximumDuration) / 1'000'000.0,
				static_cast<double>(maximumGap) / 1'000'000.0,
				static_cast<unsigned long long>(maximumTickSpan),
				static_cast<unsigned long long>(maximumDistinctTicks),
				static_cast<unsigned long long>(largestCreate),
				static_cast<unsigned long long>(largestDestroy),
				static_cast<unsigned long long>(BurstSampleDrops.load(std::memory_order_relaxed)),
				static_cast<unsigned long long>(BurstTickUnavailableSamples.load(std::memory_order_relaxed)));
		}
#endif
	}

	/** Reports per-hook install and removal evidence without touching callback state. */
	void MassLifecycle::ReportHookDiagnostics() noexcept
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		if (auto* log = S::TryLog())
		{
			S::Logf(*log, S::Level::Info,
				"MiniMap: building_mutation_hook_status intervalMs=10000 "
				"installed={create:%s destroy:%s} "
				"removal={create:{attempted:%u registry:%d core:%d} destroy:{attempted:%u registry:%d core:%d}}",
				HookStateName(CreateInstallState.load(std::memory_order_acquire)),
				HookStateName(DestroyInstallState.load(std::memory_order_acquire)),
				static_cast<unsigned>(HookRemovalRequested[0].load(std::memory_order_acquire)),
				HookRemovalRegistryState[0].load(std::memory_order_acquire),
				HookRemovalCoreState[0].load(std::memory_order_acquire),
				static_cast<unsigned>(HookRemovalRequested[1].load(std::memory_order_acquire)),
				HookRemovalRegistryState[1].load(std::memory_order_acquire),
				HookRemovalCoreState[1].load(std::memory_order_acquire));
		}
#endif
	}

	/** Reports monotonic notification totals and their latest timing correlation. */
	void MassLifecycle::ReportNotificationDiagnostics(std::uint64_t now) noexcept
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const std::uint64_t lastNotification = LatestNotificationNanoseconds.load(std::memory_order_acquire);
		const std::uint64_t latestSequence = NotificationSequence.load(std::memory_order_acquire);
		const std::uint64_t observedSequence = LastNotificationSequenceObservedAtUpdate.load(std::memory_order_acquire);
		const std::uint64_t lastUpdateAttempt = LastNotificationObservedAtCollectorAttempt.load(std::memory_order_acquire);
		const std::uint64_t updateAttempts = CollectorUpdateAttemptSequence.load(std::memory_order_acquire);
		const std::uint64_t latestSequenceAfterSnapshot = NotificationSequence.load(std::memory_order_acquire);
		const bool latestNotificationObservedAtUpdate = latestSequence != 0 &&
			latestSequence == observedSequence && latestSequence == latestSequenceAfterSnapshot;
		const std::uint64_t attemptsAfterLatestNotification = latestNotificationObservedAtUpdate &&
			updateAttempts >= lastUpdateAttempt
			? updateAttempts - lastUpdateAttempt
			: 0;
		const std::uint64_t ageMs = lastNotification == 0 ? 0 : (now - lastNotification) / 1'000'000;
		if (auto* log = S::TryLog())
		{
			S::Logf(*log, S::Level::Info,
				"MiniMap: building_mutation_hooks intervalMs=10000 "
				"accepted={create:%llu destroy:%llu sequence:%llu lastAgeMs:%llu "
				"threadMismatch:%llu subsystemMismatch:%llu subsystemIdentityUnavailable:%llu} "
				"activity={collectorUpdateAttempts:%llu "
				"notificationSequenceAtLastUpdateAttempt:%llu attemptsFromFirstObservationOfLatestNotification:%llu}",
				static_cast<unsigned long long>(AcceptedCreationNotifications.load(std::memory_order_relaxed)),
				static_cast<unsigned long long>(AcceptedDestructionNotifications.load(std::memory_order_relaxed)),
				static_cast<unsigned long long>(latestSequence),
				static_cast<unsigned long long>(ageMs),
				static_cast<unsigned long long>(CrossThreadNotifications.load(std::memory_order_relaxed)),
				static_cast<unsigned long long>(WorldMismatches.load(std::memory_order_relaxed)),
			static_cast<unsigned long long>(MissingWorldIdentity.load(std::memory_order_relaxed)),
			static_cast<unsigned long long>(updateAttempts),
			static_cast<unsigned long long>(NotificationSequenceAtLastCollectorAttempt.load(std::memory_order_relaxed)),
			static_cast<unsigned long long>(attemptsAfterLatestNotification));
		}
#endif
	}

	/** Counts one callback and compares only its scalar subsystem address and thread identity. */
	void MassLifecycle::RecordNotification(
		SDK::UCrBuildingStabilitySubsystem* subsystem,
		bool creation) noexcept
	{
		if (!GameplayAdmitted.load(std::memory_order_acquire))
		{
			return;
		}
		const std::uintptr_t expectedSubsystem = CurrentStabilitySubsystem.load(std::memory_order_acquire);
		if (!GameplayAdmitted.load(std::memory_order_acquire))
		{
			return;
		}
		if (expectedSubsystem == 0)
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			MissingWorldIdentity.fetch_add(1, std::memory_order_relaxed);
#endif
			return;
		}
		if (reinterpret_cast<std::uintptr_t>(subsystem) != expectedSubsystem)
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			WorldMismatches.fetch_add(1, std::memory_order_relaxed);
#endif
			return;
		}
		if (!GameplayAdmitted.load(std::memory_order_acquire))
		{
			return;
		}
		const std::uint64_t timestamp = CurrentNanoseconds();
		// Separate producer stores can interleave; the collector floors a newly seen revision to observation time.
		LatestAcceptedMutationNanoseconds.store(timestamp, std::memory_order_release);
		AcceptedMutationRevision.fetch_add(1, std::memory_order_release);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		if (creation)
		{
			AcceptedCreationNotifications.fetch_add(1, std::memory_order_relaxed);
		}
		else
		{
			AcceptedDestructionNotifications.fetch_add(1, std::memory_order_relaxed);
		}
		NotificationSequence.fetch_add(1, std::memory_order_relaxed);
		StoreMaximum(LatestNotificationNanoseconds, timestamp);
		const std::uint64_t tickSequence = ObservedTickSequence.load(std::memory_order_acquire);
		RecordBurstSample(subsystem, timestamp, tickSequence, creation);
		const std::uint64_t updateThread = UpdateThreadId.load(std::memory_order_acquire);
		const std::uint64_t callbackThread = static_cast<std::uint64_t>(GetCurrentThreadId());
		if (updateThread != 0 && updateThread != callbackThread)
		{
			CrossThreadNotifications.fetch_add(1, std::memory_order_relaxed);
		}
#endif
	}

	/** Records the creation notification before forwarding its untouched value exactly once. */
	void __fastcall MassLifecycle::CreateBuildingDetour(
		SDK::UCrBuildingStabilitySubsystem* subsystem,
		N::PersistentEntityId persistentEntityId) noexcept
	{
		RecordNotification(subsystem, true);
		CreateOriginal.load(std::memory_order_acquire)(subsystem, persistentEntityId);
	}

	/** Records the destruction notification before forwarding its untouched value exactly once. */
	void __fastcall MassLifecycle::DestroyBuildingDetour(
		SDK::UCrBuildingStabilitySubsystem* subsystem,
		N::PersistentEntityId persistentEntityId) noexcept
	{
		RecordNotification(subsystem, false);
		DestroyOriginal.load(std::memory_order_acquire)(subsystem, persistentEntityId);
	}
}

#endif
