// Owns production building-mutation publication and typed observational detours for client gameplay sessions.
#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include "NativeApi.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace MiniMapMassLifecycle
{
	/** Installs building Add/Remove sinks and publishes accepted activity for collector-owned scheduling. */
	class MassLifecycle final
	{
	public:
		/** Installs the two fingerprint-resolved building callbacks through the existing loader hook service. */
		static bool Initialize();

		/** Closes callback admission and requests removal of installed native detours. */
		static void Shutdown() noexcept;

		/** Resolves and publishes the current world's stability-subsystem identity on the game thread. */
		static void BeginWorld(SDK::UWorld* world);

		/** Opens or closes production callback admission for the established gameplay session. */
		static void SetGameplayAdmitted(bool admitted) noexcept;

		/** Reads the process-lifetime accepted mutation revision and reports whether gameplay is admitted. */
		static bool TryGetAcceptedMutationRevision(std::uint64_t& revision) noexcept;

		/** Closes admission and clears the current world identity without resetting the revision. */
		static void EndWorld() noexcept;

	#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		/** Emits bounded hook installation and removal status on the existing service tick. */
		static void ReportHookStatusIfDue() noexcept;
	#endif

	private:
	#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		/** Names one bounded hook installation state for operational reporting. */
		static const char* HookStateName(int state) noexcept;
	#endif

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

		/** Publishes an admitted current-world callback without touching native entity data. */
		static void RecordAcceptedNotification(
			SDK::UCrBuildingStabilitySubsystem* subsystem) noexcept;

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
