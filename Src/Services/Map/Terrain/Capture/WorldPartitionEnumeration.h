// Bridges synchronous partition enumeration without exposing either external header family.
// Object pointers and callback context are borrowed only until ForEach returns.
#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
namespace SDK
{
    class UWorldPartition;
    class UWorldPartitionSubsystem;
}

namespace Services::Map::Terrain::Capture
{
    /** Constructs the genuine engine function reference in a UE-only implementation TU.
     * The visitor executes synchronously; neither it nor context is retained by this boundary.
     */
    class WorldPartitionEnumeration final
    {
    public:
        using Visitor = bool (*)(void* context, SDK::UWorldPartition* partition);

        /** Invokes visitor for native registered partitions until it returns false or enumeration ends.
         * Requires the existing valid subsystem/bindings and a non-null visitor; propagates callback exceptions.
         */
        static void ForEach(SDK::UWorldPartitionSubsystem* subsystem, Visitor visitor, void* context);
    };
}
#endif
