#pragma once

#include "Services/RuntimeEvents.h"
#include "plugin_interface.h"
#include <array>
#include <condition_variable>
#include <list>
#include <mutex>

namespace MiniMap::Adapters::AlienX
{
    class RuntimeEvents final : public Services::IRuntimeEvents
    {
        enum class Kind { Tick, Begin, Experience, Before, After, Shutdown };
        struct Entry
        {
            Services::Registration Token;
            Kind Type;
            std::size_t Slot;
            Services::TickFn Tick = nullptr;
            Services::WorldFn World = nullptr;
            Services::NotifyFn Notify = nullptr;
            void* User = nullptr;
            bool Admitting = true, DetachRequested = false;
            std::size_t InFlight = 0;
            Services::RegistryRemoval Registry = Services::RegistryRemoval::NotRequested;
            Services::Error RemovalError = Services::Error::None;
        };
        // Nine actual tick owners; each keeps its own native callback identity.
        // Slots are never transferred to a different callback/user owner.
        std::array<Entry*, 9> Ticks{};
        std::array<Entry*, 5> Notifications{};
        std::list<Entry> Entries; // Retained closed records, including unknown submissions.
        std::uint64_t NextToken = 1;
        SDK::UWorld* CurrentWorld = nullptr; // Identity only; never rooted/dereferenced.
        std::mutex Mutex;
        std::condition_variable Drained;
        struct Invocation
        {
            Entry* Record;
            Invocation* Previous;
        };
        static thread_local Invocation* CurrentInvocation;
        bool EnteredOnThisThread(const Entry&) const noexcept;

        bool Available(Kind) const noexcept;
        Services::RegistrationReceipt<Services::Registration> Register(
            Kind, Services::TickFn, Services::WorldFn, Services::NotifyFn, void*);
        void Submit(Entry&, bool remove);
        void Dispatch(Kind, std::size_t, float, SDK::UWorld*, const char*) noexcept;
        template<std::size_t Index> static void TickThunk(float) noexcept;
        static PluginEngineTickCallback NativeTick(std::size_t) noexcept;
        static void BeginThunk(SDK::UWorld*) noexcept;
        static void ExperienceThunk() noexcept;
        static void BeforeThunk(SDK::UWorld*, const char*) noexcept;
        static void AfterThunk(SDK::UWorld*, const char*) noexcept;
        static void ShutdownThunk() noexcept;
    public:
        Services::RegistrationReceipt<Services::Registration> OnTick(Services::TickFn, void*) override;
        Services::RegistrationReceipt<Services::Registration> OnWorld(Services::WorldPhase, Services::WorldFn, void*) override;
        Services::RegistrationReceipt<Services::Registration> OnEngineShutdown(Services::NotifyFn, void*) override;
        Services::Removal Remove(Services::Registration) override;
    };
    RuntimeEvents& GetRuntimeEvents() noexcept;
}
