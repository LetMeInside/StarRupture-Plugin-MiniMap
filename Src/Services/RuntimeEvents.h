#pragma once

#include "ServiceTypes.h"
#include <string_view>

namespace SDK { class UWorld; }

namespace MiniMap::Services
{
    struct WorldEvent
    {
        SDK::UWorld* world; // Borrowed identity only.
        std::string_view displayName; // Borrowed for this callback.
    };
    using TickFn = void (*)(void* user, float deltaSeconds);
    using WorldFn = void (*)(void* user, const WorldEvent&);
    using NotifyFn = void (*)(void* user);
    enum class WorldPhase { GameplayBegan, ExperienceReady, BeforeEnd, AfterEnd };

    class IRuntimeEvents
    {
    public:
        virtual ~IRuntimeEvents() = default;
        virtual RegistrationReceipt<Registration> OnTick(TickFn, void*) = 0;
        virtual RegistrationReceipt<Registration> OnWorld(WorldPhase, WorldFn, void*) = 0;
        virtual RegistrationReceipt<Registration> OnEngineShutdown(NotifyFn, void*) = 0;
        virtual Removal Remove(Registration) = 0;
    };
}
