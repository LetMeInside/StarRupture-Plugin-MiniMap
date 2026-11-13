#if defined(MODLOADER_CLIENT_BUILD)

// Implements production building-mutation publication and bounded typed lifecycle detours.
#include "MassLifecycle.h"

#include "../Services/NativeHooks.h"
#include "../Services/Runtime.h"

#include <SDK/Chimera_classes.hpp>

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
	std::atomic<BuildingFunction> CreateOriginal{nullptr};
	std::atomic<BuildingFunction> DestroyOriginal{nullptr};

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
	constexpr auto HookStatusInterval = std::chrono::seconds(10);
	Clock::time_point LastHookStatusReport{};
#endif
}

namespace MiniMapMassLifecycle
{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
	/** Names a bounded hook installation state for ten-second operational status. */
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
#endif

	/** Publishes the typed trampoline exactly as returned by the native hook backend. */
	template<typename Function>
	void MassLifecycle::PublishOriginal(void* owner, void* original) noexcept
	{
		static_cast<std::atomic<Function>*>(owner)->store(
			reinterpret_cast<Function>(original), std::memory_order_release);
	}

	/** Installs one verified native target through the existing hook adapter. */
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

	/** Installs the two creation/destruction observers after native fingerprint resolution. */
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

	/** Resolves and publishes only the current world's scalar stability-subsystem identity. */
	void MassLifecycle::BeginWorld(SDK::UWorld* world)
	{
		SetGameplayAdmitted(false);
		CurrentStabilitySubsystem.store(0, std::memory_order_release);
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

	/** Opens or closes production callback admission for the established gameplay session. */
	void MassLifecycle::SetGameplayAdmitted(bool admitted) noexcept
	{
		GameplayAdmitted.store(admitted, std::memory_order_release);
	}

	/** Reads the atomic process-lifetime revision and reports whether collectors may schedule work. */
	bool MassLifecycle::TryGetAcceptedMutationRevision(std::uint64_t& revision) noexcept
	{
		revision = AcceptedMutationRevision.load(std::memory_order_acquire);
		return GameplayAdmitted.load(std::memory_order_acquire);
	}

	/** Ends callback admission and forgets the old subsystem address without dereferencing it. */
	void MassLifecycle::EndWorld() noexcept
	{
		SetGameplayAdmitted(false);
		CurrentStabilitySubsystem.store(0, std::memory_order_release);
	}

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
	/** Emits hook install/removal receipts at the existing ten-second reporting cadence. */
	void MassLifecycle::ReportHookStatusIfDue() noexcept
	{
		const auto now = Clock::now();
		if (LastHookStatusReport != Clock::time_point{} && now - LastHookStatusReport < HookStatusInterval)
		{
			return;
		}
		LastHookStatusReport = now;
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
	}
#endif

	/** Publishes a revision only for an admitted callback matching the sampled current subsystem. */
	void MassLifecycle::RecordAcceptedNotification(
		SDK::UCrBuildingStabilitySubsystem* subsystem) noexcept
	{
		if (!GameplayAdmitted.load(std::memory_order_acquire))
		{
			return;
		}
		const std::uintptr_t expectedSubsystem = CurrentStabilitySubsystem.load(std::memory_order_acquire);
		if (expectedSubsystem == 0 || reinterpret_cast<std::uintptr_t>(subsystem) != expectedSubsystem)
		{
			return;
		}
		if (!GameplayAdmitted.load(std::memory_order_acquire))
		{
			return;
		}
		AcceptedMutationRevision.fetch_add(1, std::memory_order_release);
	}

	/** Records creation activity before forwarding its untouched native arguments exactly once. */
	void __fastcall MassLifecycle::CreateBuildingDetour(
		SDK::UCrBuildingStabilitySubsystem* subsystem,
		N::PersistentEntityId persistentEntityId) noexcept
	{
		RecordAcceptedNotification(subsystem);
		CreateOriginal.load(std::memory_order_acquire)(subsystem, persistentEntityId);
	}

	/** Records destruction activity before forwarding its untouched native arguments exactly once. */
	void __fastcall MassLifecycle::DestroyBuildingDetour(
		SDK::UCrBuildingStabilitySubsystem* subsystem,
		N::PersistentEntityId persistentEntityId) noexcept
	{
		RecordAcceptedNotification(subsystem);
		DestroyOriginal.load(std::memory_order_acquire)(subsystem, persistentEntityId);
	}
}

#endif
