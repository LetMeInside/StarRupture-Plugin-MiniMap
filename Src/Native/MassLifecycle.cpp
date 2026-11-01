#if defined(MODLOADER_CLIENT_BUILD)
#include "MassLifecycle.h"
#include "../Services/NativeHooks.h"
#include "../Services/Runtime.h"
#include <SDK/Chimera_classes.hpp>
#include <Windows.h>
#include <array>
#include <atomic>

namespace
{
    namespace B = MiniMapMassLifecycle;
    namespace N = MiniMapNative;
    namespace S = MiniMap::Services;
    using BuildingFn = N::MassLifecycleApi::BuildingNotificationFn;
    using RestoreFn = N::MassLifecycleApi::RestoreNotificationFn;
    constexpr size_t Capacity = 65536;
    SRWLOCK Mutex = SRWLOCK_INIT;
    struct Lock
    {
        Lock() noexcept { AcquireSRWLockExclusive(&Mutex); }
        ~Lock() { ReleaseSRWLockExclusive(&Mutex); }
    };
    std::array<B::Event, Capacity> Journal{};
    size_t Head = 0, Count = 0;
    B::LifecycleStatus State;
    B::ServiceFn Consumer = nullptr;
    SDK::UCrBuildingStabilitySubsystem* Observer = nullptr;
    SDK::UCrMassSaveSubsystem* Save = nullptr;
    std::atomic<uint32_t> InFlight{0};
    std::atomic<BuildingFn> CreateOriginal{nullptr}, DestroyOriginal{nullptr};
    std::atomic<RestoreFn> SaveOriginal{nullptr}, PostOriginal{nullptr};
    std::array<S::Hook, 4> Hooks{};
    bool EverInstalled = false;
    thread_local bool InService = false;

    bool GameThread() noexcept
    {
        const auto* n = N::Get();
        __try { return n && n->player.isGameThread && n->player.isGameThread(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void Lose() noexcept { State.ObservationLost = true; State.SeedRequired = true; ++State.LossRevision; }
    void Cancel() noexcept
    {
        ++State.Epoch;
        State.Cancelled = true; State.Admitted = false;
        State.RestoreObserved = State.MassInitialized = State.PostSaveCompleted = false;
        State.SeedRequired = true;
        Head = Count = 0;
    }
    void Publish(SDK::UCrBuildingStabilitySubsystem* observer, N::PersistentEntityId id, B::EventKind kind) noexcept
    {
        Lock lock;
        if (!State.Available || State.Cancelled || !Consumer || observer != Observer) return;
        const uint64_t sequence = ++State.Sequence;
        if (id.CachedHandle.Index < 0 || id.CachedHandle.SerialNumber <= 0 || Count == Capacity)
        { Lose(); return; }
        Journal[(Head + Count) % Capacity] = {State.Epoch, sequence, id.CachedHandle,
            reinterpret_cast<uintptr_t>(observer), id.Id, kind};
        ++Count;
    }
    void Fault() noexcept { Lock lock; Lose(); State.Cancelled = true; }
    // Original calls are leaf boundaries: identical by-value lowering, one
    // invocation. SEH/C++ faults close this facility, never cross its detour.
    bool CallBuilding(BuildingFn fn, SDK::UCrBuildingStabilitySubsystem* self, N::PersistentEntityId id)
    {
        __try { fn(self, id); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool CallRestore(RestoreFn fn, SDK::UCrMassSaveSubsystem* self)
    {
        __try { fn(self); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void BuildingCall(std::atomic<BuildingFn>& original, SDK::UCrBuildingStabilitySubsystem* self,
        N::PersistentEntityId id, B::EventKind kind) noexcept
    {
        InFlight.fetch_add(1, std::memory_order_acq_rel);
        // Own the entire cached handle before the original can mutate anything.
        Publish(self, id, kind);
        try { if (!CallBuilding(original.load(std::memory_order_acquire), self, id)) Fault(); }
        catch (...) { Fault(); }
        InFlight.fetch_sub(1, std::memory_order_release);
    }
    void __fastcall Create(SDK::UCrBuildingStabilitySubsystem* self, N::PersistentEntityId id) noexcept
    { BuildingCall(CreateOriginal, self, id, B::EventKind::Add); }
    void __fastcall Destroy(SDK::UCrBuildingStabilitySubsystem* self, N::PersistentEntityId id) noexcept
    { BuildingCall(DestroyOriginal, self, id, B::EventKind::Remove); }
    void RestoreCall(std::atomic<RestoreFn>& original, SDK::UCrMassSaveSubsystem* self, bool post) noexcept
    {
        InFlight.fetch_add(1, std::memory_order_acq_rel);
        uint64_t epoch = 0, token = 0;
        {
            Lock lock;
            if (State.Available && !State.Cancelled && self == Save)
            {
                if (!post)
                {
                    ++State.Epoch; ++State.RestoreToken;
                    Head = Count = 0;
                    State.RestoreObserved = true;
                    State.MassInitialized = State.PostSaveCompleted = false;
                    State.SeedRequired = true;
                }
                epoch = State.Epoch; token = State.RestoreToken;
            }
        }
        bool returned = false;
        try { returned = CallRestore(original.load(std::memory_order_acquire), self); }
        catch (...) {}
        {
            Lock lock;
            if (epoch && epoch == State.Epoch && token == State.RestoreToken && !State.Cancelled)
            {
                if (!returned) { Lose(); State.Cancelled = true; }
                else if (post) State.PostSaveCompleted = true;
                else State.MassInitialized = true;
            }
        }
        InFlight.fetch_sub(1, std::memory_order_release);
    }
    void __fastcall SaveLoaded(SDK::UCrMassSaveSubsystem* self) noexcept { RestoreCall(SaveOriginal, self, false); }
    void __fastcall PostLoaded(SDK::UCrMassSaveSubsystem* self) noexcept { RestoreCall(PostOriginal, self, true); }

    template<class Fn> bool Install(size_t slot, Fn target, void* entry, std::atomic<Fn>& original)
    {
        auto* hooks = S::TryNativeHooks();
        if (!hooks || !target) return false;
        const auto receipt = hooks->Install({reinterpret_cast<uintptr_t>(target), entry,
            [](void* owner, void* value) noexcept {
                static_cast<std::atomic<Fn>*>(owner)->store(reinterpret_cast<Fn>(value), std::memory_order_release);
            }, &original});
        Hooks[slot] = receipt.handle;
        return receipt.state == S::HookInstallState::Active;
    }
    bool Boundary() noexcept
    { return InService && State.Available && State.Admitted && !State.Cancelled && !InFlight.load(std::memory_order_acquire); }
}

namespace MiniMapMassLifecycle
{
    bool Initialize()
    {
        // Legacy hook removal has no physical drain certificate: never reinstall
        // into callback storage that may still be reachable from an old backend.
        if (EverInstalled) return false;
        const auto* n = N::Get();
        const auto* hooks = S::TryNativeHooks();
        if (!n || !hooks) return false;
        EverInstalled = true;
        const auto& api = n->massLifecycle;
        try
        {
            if (!Install(0, api.onCreate, reinterpret_cast<void*>(&Create), CreateOriginal) ||
                !Install(1, api.onDestroy, reinterpret_cast<void*>(&Destroy), DestroyOriginal) ||
                !Install(2, api.onSaveLoaded, reinterpret_cast<void*>(&SaveLoaded), SaveOriginal) ||
                !Install(3, api.onPostSaveLoaded, reinterpret_cast<void*>(&PostLoaded), PostOriginal))
            { Shutdown(); return false; }
        }
        catch (...) { Shutdown(); return false; }
        Lock lock; State.Available = true;
        return true;
    }
    void Shutdown() noexcept
    {
        { Lock lock; Cancel(); State.Available = false; Consumer = nullptr; Observer = nullptr; Save = nullptr; }
        // No lifecycle lock while invoking backend removal. Original pointers,
        // journal and callback storage survive pending/unknown removal evidence.
        try
        {
            if (auto* hooks = S::TryNativeHooks())
                for (auto handle : Hooks) if (handle.id) hooks->Remove(handle);
        }
        catch (...) {} // No retry of uncertain backend mutations.
    }
    void BeginWorld(SDK::UWorld* world, uint64_t generation)
    {
        if (!GameThread()) return;
        EndWorld();
        if (!world || !Status().Available) return;
        auto* observer = static_cast<SDK::UCrBuildingStabilitySubsystem*>(SDK::USubsystemBlueprintLibrary::GetWorldSubsystem(
            world, SDK::TSubclassOf<SDK::UWorldSubsystem>(SDK::UCrBuildingStabilitySubsystem::StaticClass())));
        auto* save = static_cast<SDK::UCrMassSaveSubsystem*>(SDK::USubsystemBlueprintLibrary::GetWorldSubsystem(
            world, SDK::TSubclassOf<SDK::UWorldSubsystem>(SDK::UCrMassSaveSubsystem::StaticClass())));
        Lock lock;
        Observer = observer; Save = save;
        State.WorldGeneration = generation;
        State.Cancelled = !State.Available || !observer || !save;
        State.ObservationLost = false;
        // No observed restore is a late/current-world attachment, not evidence
        // of historical handler completion. CanAcquire still requires !IsLoading.
    }
    void AdmitWorld(bool admitted) noexcept { Lock lock; State.Admitted = admitted && !State.Cancelled; }
    void EndWorld() noexcept { Lock lock; Cancel(); Observer = nullptr; Save = nullptr; }
    bool SetConsumer(ServiceFn consumer) noexcept
    {
        if (!GameThread() || InService) return false;
        Lock lock;
        if (!State.Available) return false;
        if (Consumer != consumer)
        {
            Consumer = consumer; Head = Count = 0;
            State.SeedRequired = true; // A fresh census must cover the dormant prefix.
        }
        return true;
    }
    void Service()
    {
        if (InService) return;
        ServiceFn callback = nullptr;
        { Lock lock; if (State.Available && State.Admitted && !State.Cancelled && !InFlight.load()) callback = Consumer; }
        if (!callback || !GameThread()) return;
        InService = true;
        try { callback(); }
        catch (...) { Fault(); }
        InService = false;
    }
    LifecycleStatus Status() noexcept
    {
        Lock lock; auto result = State;
        result.Queued = Count; result.InFlight = InFlight.load(std::memory_order_acquire);
        result.Consumer = Consumer != nullptr;
        return result;
    }
    size_t Drain(std::span<Event> output) noexcept
    {
        Lock lock;
        if (!Boundary()) return 0;
        size_t copied = 0;
        while (copied < output.size() && Count)
        { output[copied++] = Journal[Head]; Head = (Head + 1) % Capacity; --Count; }
        return copied;
    }
    bool InAdmittedService() noexcept { Lock lock; return Boundary(); }
    bool CanAcquire(bool engineSaveLoading) noexcept
    {
        Lock lock;
        return Boundary() && !engineSaveLoading && (!State.RestoreObserved ||
            (State.MassInitialized && State.PostSaveCompleted));
    }
    SeedToken BeginSeed(bool engineSaveLoading) noexcept
    {
        Lock lock;
        return Boundary() && !engineSaveLoading && (!State.RestoreObserved ||
            (State.MassInitialized && State.PostSaveCompleted)) ?
            SeedToken{State.Epoch, State.Sequence, State.LossRevision} : SeedToken{};
    }
    bool AcceptSeed(SeedToken token, bool engineSaveLoading) noexcept
    {
        Lock lock;
        if (!Boundary() || engineSaveLoading || !token.Epoch || token.Epoch != State.Epoch || token.Watermark > State.Sequence ||
            token.LossRevision != State.LossRevision || Count ||
            (State.RestoreObserved && (!State.MassInitialized || !State.PostSaveCompleted))) return false;
        State.ObservationLost = false; State.SeedRequired = false;
        return true;
    }
}
#endif
