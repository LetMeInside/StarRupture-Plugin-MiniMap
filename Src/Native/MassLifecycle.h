#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "NativeApi.h"
#include <span>

namespace MiniMapMassLifecycle
{
    enum class EventKind : uint8_t { Add, Remove };
    struct Event
    {
        uint64_t Epoch = 0, Sequence = 0;
        MiniMapNative::MassEntityHandle Entity;
        uintptr_t Observer = 0;
        uint32_t PersistentId = 0; // Provenance only; never the membership key.
        EventKind Kind = EventKind::Add;
    };
    struct LifecycleStatus
    {
        uint64_t WorldGeneration = 0, Epoch = 0, RestoreToken = 0, Sequence = 0, LossRevision = 0;
        size_t Queued = 0;
        uint32_t InFlight = 0;
        bool Available = false, Admitted = false, Consumer = false;
        bool RestoreObserved = false, MassInitialized = false, PostSaveCompleted = false;
        bool Cancelled = true, ObservationLost = false, SeedRequired = true;
    };
    struct SeedToken { uint64_t Epoch = 0, Watermark = 0, LossRevision = 0; };
    // Called only by the existing plugin lifecycle. No second event dispatcher.
    bool Initialize();
    void Shutdown() noexcept;
    void BeginWorld(SDK::UWorld*, uint64_t generation);
    void AdmitWorld(bool admitted) noexcept;
    void EndWorld() noexcept;

    // One future world-service consumer. Null is the default dormant policy:
    // hooks retain original semantics but do not enqueue entity notifications.
    using ServiceFn = void (*)();
    bool SetConsumer(ServiceFn) noexcept;
    void Service(); // Existing admitted GT callback; no work/callback when dormant.
    LifecycleStatus Status() noexcept;
    bool InAdmittedService() noexcept;
    // These functions succeed only within Service's admitted GT scope, outside
    // all native notification stacks. No producer borrows reach this boundary.
    size_t Drain(std::span<Event> output) noexcept;
    bool CanAcquire(bool engineSaveLoading) noexcept;
    SeedToken BeginSeed(bool engineSaveLoading) noexcept;
    // Caller supplies current typed IsLoading and has seeded/replayed >Watermark.
    bool AcceptSeed(SeedToken, bool engineSaveLoading) noexcept;
}
#endif
