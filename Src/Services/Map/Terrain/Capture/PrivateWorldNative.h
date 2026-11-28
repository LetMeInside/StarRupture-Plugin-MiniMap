// Bridges private streaming and actor routing to genuine UE types and owns native render-fence operations.
#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include <memory>
#include <cstdint>
#include <vector>
namespace SDK { class UWorld; class ULevel; class AActor; class UActorComponent; class UWorldPartitionSubsystem; class UWorldPartitionStreamingSourceComponent; }

namespace Services::Map::Terrain::Capture
{
    struct PrivateStreamingDefinition;
    /** Keeps ABI-sensitive engine work isolated from the SDK session. It never owns or ticks gameplay. */
    class PrivateWorldNative final
    {
    public:
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        /** Summarizes native actor and component initialization for one level. */
        struct ActorInitializationDiagnostics final
        {
            std::uint32_t ActorCount = 0;
            std::uint32_t InitializedActorCount = 0;
            std::uint32_t ComponentCount = 0;
            std::uint32_t InitializedComponentCount = 0;
        };
#endif
        /** Allocates inert fence storage using the genuine type's size/alignment. */
        PrivateWorldNative();
        /** Requires every submitted fence to have retired before destruction. */
        ~PrivateWorldNative();
        /** Keeps native fence ownership unique. */
        PrivateWorldNative(const PrivateWorldNative&) = delete;
        /** Prevents replacing outstanding native work. */
        PrivateWorldNative& operator=(const PrivateWorldNative&) = delete;
        /** Advances the retained private subsystem through the native streaming interface entry. */
        static void AdvancePartition(SDK::UWorldPartitionSubsystem* subsystem);
        /** Advances only level streaming with a genuine unset native timeout, without an arbitrary budget. */
        static void AdvanceLevels(SDK::UWorld* world);
        /** Retirement-only native visibility flush; never selects the global/full loading-flush mode. */
        static void FlushVisibility(SDK::UWorld* world);
        /** Requires registered components and native visibility after AddToWorld's registration completion boundary. */
        static bool IsLevelReady(const SDK::ULevel* level);
        /** Routes the borrowed level without an actor limit and returns its native completion state. */
        static bool RouteActorInitialization(SDK::ULevel* level);
        /** Returns this borrowed level's native routing completion state without changing initialization flags. */
        static bool IsActorInitializationComplete(const SDK::ULevel* level);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        /** Returns native actor/component initialization counts for this borrowed level without changing it. */
        static ActorInitializationDiagnostics GetActorInitializationDiagnostics(const SDK::ULevel* level);
#endif
        /** Preserves the native component creation guard for a missing or tearing-down world. */
        static bool CanCreateComponentInWorld(const SDK::UWorld* world);
        /** Appends borrowed native component pointers for one admitted actor without actor reflection. */
        static void AppendOwnedComponents(const SDK::AActor* actor, std::vector<SDK::UActorComponent*>& components);
        /** Observes native component registration when the neutral SDK omits the inline query. */
        static bool IsComponentRegistered(const SDK::UActorComponent* component);
        /** Observes real registration and render-state creation for a required representative component. */
        static bool IsComponentReady(const SDK::UActorComponent* component);
        /** Writes genuine source arrays before registration; failureReason receives static rejection text on false. */
        static bool ConfigureSource(SDK::UWorldPartitionStreamingSourceComponent* source,
            const PrivateStreamingDefinition& definition, const char** failureReason);
        /** Starts an RHI-thread fence covering preceding native cleanup submissions. */
        void BeginFence();
        /** Returns true only after native fence completion and destruction; inactive is already complete. */
        bool PollFence();
        /** Explicit shutdown-only wait for this genuine native fence, followed by completion/destruction observation. */
        bool WaitFence();
        /** Reports native fence ownership, independently of logical readiness/cancellation. */
        bool HasFence() const noexcept;
    private:
        /** Contains genuine-sized native placement storage, never a substitute fence representation. */
        struct State;
        std::unique_ptr<State> state_;
    };
}
#endif
