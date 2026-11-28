// Separates the private partition's synchronous registration boundary from final native retirement.
// Every additional native retention is transferred into the caller's session-owned retirement list.
#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include <vector>

namespace SDK
{
    class UObject;
    class UWorld;
    class UWorldPartition;
    class UWorldPartitionSubsystem;
    class UWorldPartitionStreamingSourceComponent;
}

namespace Services::Map::Terrain::Capture
{
    /** Performs only early unregistration on the game thread outside native streaming/loading callbacks.
     * The caller exclusively owns the private world and keeps nativeOwners, world/subsystem/source,
     * level/request and render owners until their separate final-drain conditions are established.
     */
    class WorldPartitionUnregistration final
    {
    public:
        /** Removes the private source and uninitializes registered partitions before gameplay unregisters.
         * True proves empty registration after native return, or failed admission with no subsystem,
         * initialized root, runtime partition managers/policy, source or streaming levels.
         * It does not prove packages, levels, components, resources or callbacks are retired.
         * nativeOwners receives AddRef-owned objects, including on failure; no references are released here.
         */
        static bool Unregister(SDK::UWorld* world,
            SDK::UWorldPartitionStreamingSourceComponent* source,
            std::vector<SDK::UObject*>& nativeOwners);

    private:
        /** Borrows the collection result and durable retention list during one synchronous enumeration. */
        struct EnumerationState;
        /** Appends and retains one partition using the borrowed EnumerationState context. */
        static bool CollectPartition(void* context, SDK::UWorldPartition* partition);
        /** Marks the borrowed boolean false and stops after the first registered partition. */
        static bool ObservePartition(void* context, SDK::UWorldPartition* partition);
        /** Adds one distinct extra native reference to the caller's durable retirement ownership. */
        static void RetainOwner(SDK::UObject* object, std::vector<SDK::UObject*>& nativeOwners);
        /** Copies registered partitions without mutating the engine collection during enumeration. */
        static std::vector<SDK::UWorldPartition*> CollectPartitions(SDK::UWorldPartitionSubsystem* subsystem,
            std::vector<SDK::UObject*>& nativeOwners);
        /** Uninitializes subpartitions first and the persistent root last, preserving native ownership. */
        static void UnregisterPartitions(const std::vector<SDK::UWorldPartition*>& partitions,
            SDK::UWorldPartition* root);
        /** Observes the private registration set through the native enumerator after uninitialization. */
        static bool IsRegistrationEmpty(SDK::UWorldPartitionSubsystem* subsystem);

        static thread_local bool active_;
    };
}
#endif
