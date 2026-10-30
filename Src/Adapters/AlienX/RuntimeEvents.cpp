#include "RuntimeEvents.h"
#include "Compatibility.h"

#if defined(MODLOADER_CLIENT_BUILD)
namespace MiniMap::Adapters::AlienX
{
    namespace S = Services;
    thread_local RuntimeEvents::Invocation* RuntimeEvents::CurrentInvocation = nullptr;

    bool RuntimeEvents::EnteredOnThisThread(const Entry& entry) const noexcept
    {
        for (auto* call = CurrentInvocation; call; call = call->Previous)
            if (call->Record == &entry) return true;
        return false;
    }

    bool RuntimeEvents::Available(Kind kind) const noexcept
    {
        const auto* self = GetSelf();
        const auto* hooks = self ? self->hooks : nullptr;
        const auto* engine = hooks ? hooks->Engine : nullptr;
        const auto* world = hooks ? hooks->World : nullptr;
        switch (kind)
        {
        case Kind::Tick: return engine && engine->RegisterOnTick && engine->UnregisterOnTick;
        case Kind::Shutdown: return engine && engine->RegisterOnShutdown && engine->UnregisterOnShutdown;
        case Kind::Begin: return world && world->RegisterOnWorldBeginPlay && world->UnregisterOnWorldBeginPlay;
        case Kind::Experience: return world && world->RegisterOnExperienceLoadComplete && world->UnregisterOnExperienceLoadComplete;
        case Kind::Before: return world && world->RegisterOnBeforeWorldEndPlay && world->UnregisterOnBeforeWorldEndPlay;
        case Kind::After: return world && world->RegisterOnAfterWorldEndPlay && world->UnregisterOnAfterWorldEndPlay;
        }
        return false;
    }

    template<std::size_t Index> void RuntimeEvents::TickThunk(float delta) noexcept
    { GetRuntimeEvents().Dispatch(Kind::Tick, Index, delta, nullptr, nullptr); }
    PluginEngineTickCallback RuntimeEvents::NativeTick(std::size_t slot) noexcept
    {
        static constexpr std::array<PluginEngineTickCallback, 9> callbacks{
            &TickThunk<0>, &TickThunk<1>, &TickThunk<2>, &TickThunk<3>, &TickThunk<4>,
            &TickThunk<5>, &TickThunk<6>, &TickThunk<7>, &TickThunk<8>};
        return callbacks[slot];
    }
    void RuntimeEvents::BeginThunk(SDK::UWorld* world) noexcept
    { GetRuntimeEvents().Dispatch(Kind::Begin, 0, 0, world, nullptr); }
    void RuntimeEvents::ExperienceThunk() noexcept
    { GetRuntimeEvents().Dispatch(Kind::Experience, 0, 0, nullptr, nullptr); }
    void RuntimeEvents::BeforeThunk(SDK::UWorld* world, const char* name) noexcept
    { GetRuntimeEvents().Dispatch(Kind::Before, 0, 0, world, name); }
    void RuntimeEvents::AfterThunk(SDK::UWorld* world, const char* name) noexcept
    { GetRuntimeEvents().Dispatch(Kind::After, 0, 0, world, name); }
    void RuntimeEvents::ShutdownThunk() noexcept
    { GetRuntimeEvents().Dispatch(Kind::Shutdown, 0, 0, nullptr, nullptr); }

    void RuntimeEvents::Submit(Entry& entry, bool remove)
    {
        auto* hooks = GetSelf()->hooks;
        switch (entry.Type)
        {
        case Kind::Tick:
            (remove ? hooks->Engine->UnregisterOnTick : hooks->Engine->RegisterOnTick)(NativeTick(entry.Slot)); break;
        case Kind::Begin:
            (remove ? hooks->World->UnregisterOnWorldBeginPlay : hooks->World->RegisterOnWorldBeginPlay)(&BeginThunk); break;
        case Kind::Experience:
            (remove ? hooks->World->UnregisterOnExperienceLoadComplete : hooks->World->RegisterOnExperienceLoadComplete)(&ExperienceThunk); break;
        case Kind::Before:
            (remove ? hooks->World->UnregisterOnBeforeWorldEndPlay : hooks->World->RegisterOnBeforeWorldEndPlay)(&BeforeThunk); break;
        case Kind::After:
            (remove ? hooks->World->UnregisterOnAfterWorldEndPlay : hooks->World->RegisterOnAfterWorldEndPlay)(&AfterThunk); break;
        case Kind::Shutdown:
            (remove ? hooks->Engine->UnregisterOnShutdown : hooks->Engine->RegisterOnShutdown)(&ShutdownThunk); break;
        }
    }

    S::RegistrationReceipt<S::Registration> RuntimeEvents::OnTick(S::TickFn fn, void* user)
    { return Register(Kind::Tick, fn, nullptr, nullptr, user); }
    S::RegistrationReceipt<S::Registration> RuntimeEvents::OnEngineShutdown(S::NotifyFn fn, void* user)
    { return Register(Kind::Shutdown, nullptr, nullptr, fn, user); }
    S::RegistrationReceipt<S::Registration> RuntimeEvents::OnWorld(S::WorldPhase phase, S::WorldFn fn, void* user)
    {
        Kind kind;
        switch (phase)
        {
        case S::WorldPhase::GameplayBegan: kind = Kind::Begin; break;
        case S::WorldPhase::ExperienceReady: kind = Kind::Experience; break;
        case S::WorldPhase::BeforeEnd: kind = Kind::Before; break;
        case S::WorldPhase::AfterEnd: kind = Kind::After; break;
        default: return {{}, S::Acceptance::Rejected, {S::Error::InvalidArgument, {}}};
        }
        return Register(kind, nullptr, fn, nullptr, user);
    }

    S::RegistrationReceipt<S::Registration> RuntimeEvents::Register(
        Kind kind, S::TickFn tick, S::WorldFn world, S::NotifyFn notify, void* user)
    {
        if (!tick && !world && !notify)
            return {{}, S::Acceptance::Rejected, {S::Error::InvalidArgument, {}}};
        if (!Available(kind)) return {{}, S::Acceptance::Rejected, {S::Error::Unavailable, {}}};
        Entry* entry;
        try
        {
            std::lock_guard lock(Mutex);
            std::size_t slot = 0;
            Entry** current;
            if (kind == Kind::Tick)
            {
                // Re-registration uses the same owner's native identity, as the
                // original serialized Engine registration/unregistration did.
                for (; slot < Ticks.size(); ++slot)
                    if (Ticks[slot] && Ticks[slot]->Tick == tick && Ticks[slot]->User == user) break;
                if (slot == Ticks.size())
                    for (slot = 0; slot < Ticks.size() && Ticks[slot]; ++slot) {}
                if (slot == Ticks.size()) return {{}, S::Acceptance::Rejected, {S::Error::Capacity, {}}};
                current = &Ticks[slot];
            }
            else current = &Notifications[std::size_t(kind) - 1];
            if (*current && ((*current)->Admitting || (*current)->InFlight ||
                (*current)->Registry != S::RegistryRemoval::RequestReturned))
                return {{}, S::Acceptance::Rejected, {S::Error::OwnershipUncertain, {}}};
            if (!NextToken) return {{}, S::Acceptance::Rejected, {S::Error::Capacity, {}}};
            Entries.push_back({{NextToken}, kind, slot, tick, world, notify, user});
            ++NextToken;
            *current = entry = &Entries.back(); // Complete before possible inline replay.
        }
        catch (...) { return {{}, S::Acceptance::Rejected, {S::Error::BackendFailure, {}}}; }
        try
        {
            Submit(*entry, false); // No adapter lock across host or core execution.
            return {entry->Token, S::Acceptance::AcceptedUnverified, {}};
        }
        catch (...)
        {
            std::lock_guard lock(Mutex);
            entry->Admitting = false;
            entry->Registry = S::RegistryRemoval::Unknown;
            return {entry->Token, S::Acceptance::BackendUnknown, {S::Error::BackendFailure, {}}};
        }
    }

    S::Removal RuntimeEvents::Remove(S::Registration token)
    {
        Entry* entry = nullptr;
        {
            std::unique_lock lock(Mutex);
            if (!token.id) return {true, S::RegistryRemoval::NotRequested, S::CoreDrain::Quiescent, {}};
            for (auto& candidate : Entries) if (candidate.Token.id == token.id) { entry = &candidate; break; }
            if (!entry) return {false, S::RegistryRemoval::NotRequested, S::CoreDrain::Unknown, {S::Error::InvalidArgument, {}}};
            entry->Admitting = false;
            if (entry->DetachRequested)
            {
                if (!EnteredOnThisThread(*entry))
                    Drained.wait(lock, [&] { return entry->InFlight == 0; });
                return {true, entry->Registry, entry->InFlight ? S::CoreDrain::Pending : S::CoreDrain::Quiescent, {entry->RemovalError, {}}};
            }
            entry->DetachRequested = true;
            entry->Registry = S::RegistryRemoval::Unknown;
            entry->RemovalError = S::Error::OwnershipUncertain;
        }
        auto registry = S::RegistryRemoval::Unknown;
        auto error = S::Error::None;
        try
        {
            if (Available(entry->Type)) { Submit(*entry, true); registry = S::RegistryRemoval::RequestReturned; }
            else error = S::Error::Unavailable;
        }
        catch (...) { error = S::Error::BackendFailure; }
        std::unique_lock lock(Mutex);
        entry->Registry = registry;
        entry->RemovalError = error;
        // Consumers call Remove outside their lifecycle/input/render locks.
        // Wait releases Mutex; self-removal (notably engine shutdown) reports
        // Pending instead. This drains core bodies, not host thunks/DLL code.
        if (!EnteredOnThisThread(*entry))
            Drained.wait(lock, [&] { return entry->InFlight == 0; });
        return {true, registry, entry->InFlight ? S::CoreDrain::Pending : S::CoreDrain::Quiescent, {error, {}}};
    }

    void RuntimeEvents::Dispatch(Kind kind, std::size_t slot, float delta, SDK::UWorld* world, const char* name) noexcept
    {
        Entry* entry;
        {
            std::lock_guard lock(Mutex);
            entry = kind == Kind::Tick ? Ticks[slot] : Notifications[std::size_t(kind) - 1];
            if (!entry || !entry->Admitting) return;
            ++entry->InFlight; // Entry/count and admission closure share one mutex.
            if (kind == Kind::Begin) CurrentWorld = world;
            if (kind == Kind::Experience) world = CurrentWorld;
        }
        Invocation invocation{entry, CurrentInvocation};
        CurrentInvocation = &invocation;
        try
        {
            if (entry->Tick) entry->Tick(entry->User, delta);
            else if (entry->World) entry->World(entry->User, {world, name ? std::string_view(name) : std::string_view{}});
            else entry->Notify(entry->User);
        }
        catch (...) {} // Never cross the native callback boundary.
        CurrentInvocation = invocation.Previous;
        std::lock_guard lock(Mutex);
        if (kind == Kind::After && CurrentWorld == world) CurrentWorld = nullptr;
        --entry->InFlight;
        Drained.notify_all();
    }
}
#endif
