// Uses the normal native partition uninitializer while retaining owners for later asynchronous drainage.
// This is an early registration boundary, not world cleanup, a global GC manager or a completion shortcut.
#if defined(MODLOADER_CLIENT_BUILD)
#include "WorldPartitionUnregistration.h"
#include "WorldPartitionEnumeration.h"
#include "Native/NativeApi.h"
#include "SDK/Engine_classes.hpp"
#include <algorithm>

namespace Services::Map::Terrain::Capture
{
    thread_local bool WorldPartitionUnregistration::active_ = false;

    /** References SDK-owned collections only for the duration of the synchronous native call. */
    struct WorldPartitionUnregistration::EnumerationState final
    {
        std::vector<SDK::UWorldPartition*>& Partitions;
        std::vector<SDK::UObject*>& NativeOwners;
    };

    /** Preserves collection-before-mutation and publishes each partition's native retention. */
    bool WorldPartitionUnregistration::CollectPartition(void* context, SDK::UWorldPartition* partition)
    {
        auto& state = *static_cast<EnumerationState*>(context);
        state.Partitions.push_back(partition);
        RetainOwner(partition, state.NativeOwners);
        return true;
    }

    /** Preserves the native early-stop observation used for final empty-registration verification. */
    bool WorldPartitionUnregistration::ObservePartition(void* context, SDK::UWorldPartition*)
    {
        *static_cast<bool*>(context) = false;
        return false;
    }

    /** Publishes retention ownership before any native call that can advance partition teardown. */
    void WorldPartitionUnregistration::RetainOwner(SDK::UObject* object,
        std::vector<SDK::UObject*>& nativeOwners)
    {
        if (object && std::find(nativeOwners.begin(), nativeOwners.end(), object) == nativeOwners.end())
        {
            nativeOwners.push_back(object);
            MiniMapNative::Get()->asset.retainObject(object);
        }
    }

    /** Copies SDK partition pointers through the synchronous bridge and retains them in the session pool. */
    std::vector<SDK::UWorldPartition*> WorldPartitionUnregistration::CollectPartitions(
        SDK::UWorldPartitionSubsystem* subsystem, std::vector<SDK::UObject*>& nativeOwners)
    {
        std::vector<SDK::UWorldPartition*> partitions;
        EnumerationState state{partitions, nativeOwners};
        WorldPartitionEnumeration::ForEach(subsystem, &CollectPartition, &state);
        return partitions;
    }

    /** Calls normal native uninitialization, which joins prior policy work before broadcasting removal. */
    void WorldPartitionUnregistration::UnregisterPartitions(
        const std::vector<SDK::UWorldPartition*>& partitions, SDK::UWorldPartition* root)
    {
        const auto& bindings = MiniMapNative::Get()->captureAdmission;
        for (auto* partition : partitions)
        {
            if (partition != root && bindings.isPartitionInitialized(partition))
                bindings.uninitializePartition(partition);
        }
        if (root && bindings.isPartitionInitialized(root))
            bindings.uninitializePartition(root);
    }

    /** A successful native return plus an empty registration inventory closes the GC snapshot boundary. */
    bool WorldPartitionUnregistration::IsRegistrationEmpty(SDK::UWorldPartitionSubsystem* subsystem)
    {
        bool empty = true;
        WorldPartitionEnumeration::ForEach(subsystem, &ObservePartition, &empty);
        return empty;
    }

    /** Retains the graph, suppresses only private demand, then acknowledges registration rather than final retirement. */
    bool WorldPartitionUnregistration::Unregister(SDK::UWorld* world,
        SDK::UWorldPartitionStreamingSourceComponent* source, std::vector<SDK::UObject*>& nativeOwners)
    {
        const auto* api = MiniMapNative::Get();
        if (active_ || !api || !api->player.isGameThread() || !api->captureAdmission.IsComplete() ||
            !world || !api->captureAdmission.isGameWorld(world) || !api->captureAdmission.isPreviewWorld(world) ||
            world->NetDriver || world->OwningGameInstance || !world->PersistentLevel ||
            !world->PersistentLevel->WorldSettings || api->world.isGarbageCollectingAndLocking())
            return false;
        auto* subsystem = static_cast<SDK::UWorldPartitionSubsystem*>(
            SDK::USubsystemBlueprintLibrary::GetWorldSubsystem(world, SDK::UWorldPartitionSubsystem::StaticClass()));
        if (!subsystem)
        {
            // Failed admission skips native partition initialization and its subsystem-owned registration.
            auto* root = world->PersistentLevel->WorldSettings->WorldPartition;
            return !source && root && !api->captureAdmission.isPartitionInitialized(root) &&
                !root->StreamingPolicy && !root->DataLayerManager && !root->ExternalDataLayerManager &&
                world->StreamingLevels.Num() == 0 && world->StreamingLevelsToConsider.StreamingLevels.Num() == 0;
        }
        auto* actor = source ? source->GetOwner() : nullptr;
        if (subsystem->Outer != world ||
            (source && (!actor || actor->Outer != world->PersistentLevel)))
            return false;
        RetainOwner(world, nativeOwners);
        RetainOwner(world->Outer, nativeOwners);
        RetainOwner(subsystem, nativeOwners);
        RetainOwner(source, nativeOwners);
        RetainOwner(actor, nativeOwners);
        active_ = true;
        try
        {
            if (source)
            {
                source->DisableStreamingSource();
                api->player.unregisterComponent(source);
            }
            auto partitions = CollectPartitions(subsystem, nativeOwners);
            api->captureAdmission.forceUnload(world, true);
            UnregisterPartitions(partitions, world->PersistentLevel->WorldSettings->WorldPartition);
            const bool empty = IsRegistrationEmpty(subsystem);
            active_ = false;
            return empty;
        }
        catch (...)
        {
            active_ = false;
            throw;
        }
    }
}
#endif
