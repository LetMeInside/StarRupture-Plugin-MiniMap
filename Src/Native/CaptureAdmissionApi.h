// Typed native prerequisites for selective admission, private streaming progression and early unregistration.
// This non-owning binding group does not activate streaming or install a hook.
#pragma once

#if defined(MODLOADER_CLIENT_BUILD)
namespace SDK
{
    class UObject;
    class UPackage;
    class UWorld;
    class UWorldSubsystem;
    class UWorldPartition;
    class UWorldPartitionSubsystem;
}
class UWorldPartition;
class IStreamingWorldSubsystemInterface;
template<typename Signature> class TFunctionRef;

namespace MiniMapNative
{
    /** Borrows resolved engine entry points for one NativeApi binding lifetime.
     * Consumers own the session; completeness does not prove runtime readiness or resource retirement.
     */
    struct CaptureAdmissionApi final
    {
        using ShouldCreateSubsystem = bool(const SDK::UWorldSubsystem*, SDK::UObject*);
        using WorldPredicate = bool(const SDK::UWorld*);
        using FindWorldInPackage = SDK::UWorld*(SDK::UPackage*);
        using PartitionPredicate = bool(const SDK::UWorldPartition*);
        using UninitializePartition = void(SDK::UWorldPartition*);
        using EnumeratePartitions = void(SDK::UWorldPartitionSubsystem*, TFunctionRef<bool(::UWorldPartition*)>);
        using ForceUnload = void(SDK::UWorld*, bool);
        // The resolved virtual entry receives the secondary streaming interface subobject.
        using UpdatePartition = void(::IStreamingWorldSubsystemInterface*);
        using UpdateLevels = void(); // Transport only; the UE bridge restores the genuine TOptional signature.
        using WorldAction = void(SDK::UWorld*);
        using FlushVisibility = void(); // Restored to the genuine enum signature in the UE-only bridge.
        using WaitRenderFence = void(const void*, bool);

        ShouldCreateSubsystem* shouldCreateSubsystem = nullptr;
        WorldPredicate* isGameWorld = nullptr;
        WorldPredicate* isPreviewWorld = nullptr;
        FindWorldInPackage* findWorldInPackage = nullptr;
        PartitionPredicate* isPartitionInitialized = nullptr;
        UninitializePartition* uninitializePartition = nullptr;
        EnumeratePartitions* enumeratePartitions = nullptr;
        ForceUnload* forceUnload = nullptr;
        UpdatePartition* updatePartition = nullptr;
        UpdateLevels* updateLevels = nullptr;
        WorldAction* sendEndOfFrame = nullptr;
        FlushVisibility* flushVisibility = nullptr;
        WaitRenderFence* waitRenderFence = nullptr;
        WorldPredicate* visibilityPending = nullptr;
        WorldPredicate* hasEndOfFrameUpdates = nullptr;
        PartitionPredicate* canStream = nullptr;
        PartitionPredicate* serverStreamingEnabled = nullptr;
        PartitionPredicate* serverStreamingOutEnabled = nullptr;

        /** Reports whether every admission prerequisite was published together. */
        bool IsComplete() const noexcept
        {
            return shouldCreateSubsystem && isGameWorld && isPreviewWorld && findWorldInPackage &&
                isPartitionInitialized && uninitializePartition && enumeratePartitions && forceUnload &&
                updatePartition && updateLevels && sendEndOfFrame && visibilityPending &&
                canStream && serverStreamingEnabled && serverStreamingOutEnabled && flushVisibility && waitRenderFence &&
                hasEndOfFrameUpdates;
        }
    };
}
#endif
