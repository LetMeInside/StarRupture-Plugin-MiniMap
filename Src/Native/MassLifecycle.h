// Owns observational building lifecycle detours and bounded callback diagnostics for client gameplay sessions.
#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include "NativeApi.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace MiniMapMassLifecycle
{
	/** Installs building Add/Remove sinks and owns bounded gameplay-session diagnostics. */
	class MassLifecycle final
	{
	public:
		/** Installs the two fingerprint-resolved building callbacks through the existing loader hook service. */
		static bool Initialize();

		/** Closes diagnostic admission and requests removal of installed building hooks. */
		static void Shutdown() noexcept;

		/** Resolves the current world's stability subsystem identity once on the game thread. */
		static void BeginWorld(SDK::UWorld* world);

		/** Enables or disables callback classification for the admitted gameplay session. */
		static void SetGameplayAdmitted(bool admitted) noexcept;

		/** Clears the current world identity without resetting monotonic diagnostic counters. */
		static void EndWorld() noexcept;

		/** Samples the update thread and advances the tick sequence at the existing renderer callback. */
		static void ObserveUpdateThread() noexcept;

		/** Records the start boundary for one existing BuildingLayer update attempt. */
		static void BeginCollectorUpdate() noexcept;

		/** Emits one bounded development summary when its existing ten-second interval is due. */
		static void ReportDiagnosticsIfDue() noexcept;

	private:
		/** Reads the monotonic diagnostic clock without touching native state. */
		static std::uint64_t CurrentNanoseconds() noexcept;

		/** Retains a maximum scalar with lock-free compare/exchange where supported. */
		static void StoreMaximum(std::atomic<std::uint64_t>& destination, std::uint64_t value) noexcept;

		/** Names one bounded hook installation status for development reporting. */
		static const char* HookStateName(int state) noexcept;

		/** Reports installation and removal receipts on the existing update thread. */
		static void ReportHookDiagnostics() noexcept;

		/** Reports notification counts, timestamps and collector-attempt correlation. */
		static void ReportNotificationDiagnostics(std::uint64_t now) noexcept;

		/** Reports completed and active burst aggregates without retaining event history. */
		static void ReportBurstDiagnostics() noexcept;

		/** Adds one accepted callback to the bounded burst aggregates when uncontended. */
		static void RecordBurstSample(
			SDK::UCrBuildingStabilitySubsystem* subsystem,
			std::uint64_t timestamp,
			std::uint64_t tickSequence,
			bool creation) noexcept;

		/** Adds callback counts and tick samples to a new or existing active burst. */
		static void RecordBurstCallback(
			std::uint64_t timestamp,
			std::uint64_t tickSequence,
			bool creation) noexcept;

		/** Completes the active burst and updates lifetime maximum aggregates. */
		static void CompleteActiveBurst() noexcept;

		/** Closes an active diagnostic burst at the existing world boundary. */
		static void CloseActiveBurst() noexcept;

		/** Publishes the backend trampoline into the typed callback owner. */
		template<typename Function>
		static void PublishOriginal(void* owner, void* original) noexcept;

		/** Installs one typed member-function detour and retains its loader token. */
		template<typename Function>
		static bool InstallBuildingHook(
			std::size_t slot,
			Function target,
			void* detour,
			std::atomic<Function>& original,
			std::atomic<int>& state);

		/** Records one callback without accessing native entity data or service state. */
		static void RecordNotification(
			SDK::UCrBuildingStabilitySubsystem* subsystem,
			bool creation) noexcept;

		/** Forwards one creation notification to its original native method exactly once. */
		static void __fastcall CreateBuildingDetour(
			SDK::UCrBuildingStabilitySubsystem* subsystem,
			MiniMapNative::PersistentEntityId persistentEntityId) noexcept;

		/** Forwards one destruction notification to its original native method exactly once. */
		static void __fastcall DestroyBuildingDetour(
			SDK::UCrBuildingStabilitySubsystem* subsystem,
			MiniMapNative::PersistentEntityId persistentEntityId) noexcept;
	};
}

#endif
