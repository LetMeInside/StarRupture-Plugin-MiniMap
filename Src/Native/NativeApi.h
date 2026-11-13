#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include <cstdint>
#include <cstddef>

class FRHIComputeCommandList;
struct FRHITransitionInfo;
template<typename InElementType, typename InSizeType> class TArrayView;
enum class ERHITransitionCreateFlags;

class FRHICommandList;
class FRHICommandListBase;
class FRHICommandListImmediate;
class FRHIGPUTextureReadback;
class FRHIGPUFence;
class FRHIResource;
class FRHITexture;
class FDynamicRHI;
struct FRHITextureCreateDesc;
class FName;
struct FNameEntryId;
namespace UE::Math { template<typename IntType> struct TIntVector3; }
struct TStatId;
enum class EName : uint32_t;
template<typename FuncType> class TUniqueFunction;
template<typename ReferencedType> class TRefCountPtr;

struct FMassEntityManager;
struct FMassExecutionContext;
struct FMassEntityQuery;
template<typename FuncType> class TFunction;

namespace SDK
{
    class FName;
    class UActorComponent;
    class UTextureRenderTarget2D;
    class UStaticMesh;
    class USceneCaptureComponent2D;
    struct FTransform;
    enum class EMassFragmentAccess : uint8_t;
    enum class EMassFragmentPresence : uint8_t;
    class UObject;
    class UCommonActivatableWidget;
    template<typename ClassType> class TSubclassOf;
    class UTexture2D;
    class UTexture;
    class UCrBuildingStabilitySubsystem;
    class UCrMassSaveSubsystem;
    enum class EPixelFormat : uint8_t;
    class UStreamableRenderAsset;

    class UWorld;
    class UEngine;
    class APlayerController;
    class AController;
    class ACrCharacterPlayerBase;
    class USceneComponent;
    class UClass;
    class UCrMapMenuDevSettings;
    class UCrMapMenuPOIData;
    class UCrMapMenuCategoryData;
    class UCrPlayerMapMenuDataComponent;
    class UMassEntitySubsystem;
    class UScriptStruct;

    struct FCrAbandonBaseData;
    struct FVector;
    struct FRotator;
    struct FSlateBrush;
    struct FAuAPPayloadDataHandle;
    struct FAuAPSplineActorPayloadData;
}

namespace MiniMapFingerprints
{
    struct ResolvedAddresses;
}

namespace MiniMapNative
{
    struct CommonUIApi
    {
        using NativeOnActivatedFn =
            void (__fastcall*)(SDK::UCommonActivatableWidget* widget);
        using NativeOnDeactivatedFn =
            void (__fastcall*)(SDK::UCommonActivatableWidget* widget);

        NativeOnActivatedFn nativeOnActivated = nullptr;
        NativeOnDeactivatedFn nativeOnDeactivated = nullptr;
    };

    struct AssetApi
    {
        const bool* activeGarbageCollection = nullptr;
        const uint8_t* exitPurge = nullptr;
        const void* objectArray = nullptr; // Opaque enclosing FUObjectArray; private slot interpretation stays with consumers.
        using NameEqualsWideFn = bool (*)(SDK::FName, const wchar_t*);
        NameEqualsWideFn nameEqualsWide = nullptr;

        using MemoryMallocFn = void* (*)(size_t, uint32_t);
        MemoryMallocFn memoryMalloc = nullptr;
        using FromValidENameFn = FNameEntryId* (*)(FNameEntryId*, EName);
        FromValidENameFn fromValidEName = nullptr;

        using SpawnObjectFn = SDK::UObject* (*)(const SDK::TSubclassOf<SDK::UObject>*, SDK::UObject*);
        SpawnObjectFn spawnObject = nullptr;
        using SetObjectFlagsFn = void (*)(SDK::UObject*, uint32_t);
        SetObjectFlagsFn setObjectFlags = nullptr;
        using RetainObjectFn = void (*)(SDK::UObject*);
        RetainObjectFn retainObject = nullptr;
        // Native GExitPurge-guarded wrapper; never substitute direct ReleaseRef.
        using ReleaseObjectFn = void (*)(SDK::UObject*);
        ReleaseObjectFn releaseObject = nullptr;

        using LoadSynchronousFn =
            SDK::UObject* (*)(void*);

        LoadSynchronousFn loadSynchronous = nullptr;
    };


    struct PlayerApi
    {
        using RegisterComponentFn = void (*)(SDK::UActorComponent*, SDK::UWorld*, void*);
        RegisterComponentFn registerComponent = nullptr;
        using UnregisterComponentFn = void (*)(SDK::UActorComponent*);
        UnregisterComponentFn unregisterComponent = nullptr;
        using DestroyComponentFn = void (*)(SDK::UActorComponent*, bool);
        DestroyComponentFn destroyComponent = nullptr;
        using IsGameThreadFn = bool (*)();
        IsGameThreadFn isGameThread = nullptr;

        using GetFirstPlayerControllerFn =
            SDK::APlayerController* (*)(
                const SDK::UWorld*);

        using GetControlRotationFn =
            SDK::FRotator* (__fastcall*)(
                const SDK::AController* controller,
                SDK::FRotator* outRotation);

        using IsPlayerInForgottenEngineFn =
            bool(__fastcall*)(
                const SDK::ACrCharacterPlayerBase* player);

        using GetComponentLocationFn =
            SDK::FVector* (*)(
                const SDK::USceneComponent*,
                SDK::FVector*);

        GetFirstPlayerControllerFn getFirstPlayerController = nullptr;
        GetControlRotationFn getControlRotation = nullptr;
        IsPlayerInForgottenEngineFn isPlayerInForgottenEngine = nullptr;
        GetComponentLocationFn getComponentLocation = nullptr;
    };


    struct PointsOfInterestApi
    {
        using GetAllActorsOfClassFn =
            void (__fastcall*)(
                const SDK::UObject* worldContext,
                const void* actorClassStorage,
                void* outActors);

        using POIStaticClassFn =
            SDK::UClass* (__fastcall*)();

        using FindPOIMarkerCategoryDataFn =
            const SDK::UCrMapMenuPOIData* (__fastcall*)(
                const SDK::UCrMapMenuDevSettings* settings,
                uint8_t pointOfInterestType);

        using GetMarkerFilterStatusFn =
            bool (__fastcall*)(
                SDK::UCrPlayerMapMenuDataComponent* component,
                uint8_t filter);

        using IsAbandonBaseCompletedFn =
            bool (__fastcall*)(
                const SDK::FCrAbandonBaseData* data);

        using FindFoundableMarkerCategoryDataFn =
            const SDK::UCrMapMenuCategoryData* (__fastcall*)(
                const SDK::UCrMapMenuDevSettings* settings,
                uint8_t foundableType);

        GetAllActorsOfClassFn getAllActorsOfClass = nullptr;
        POIStaticClassFn pointOfInterestStaticClass = nullptr;
        FindPOIMarkerCategoryDataFn findPOIMarkerCategoryData = nullptr;
        GetMarkerFilterStatusFn getMarkerFilterStatus = nullptr;
        IsAbandonBaseCompletedFn isAbandonBaseCompleted = nullptr;
        FindFoundableMarkerCategoryDataFn findFoundableMarkerCategoryData = nullptr;
    };


    struct MassEntityHandle
    {
        int32_t Index = 0;
        int32_t SerialNumber = 0;
    };

    static_assert(
        sizeof(MassEntityHandle) ==
        0x08);


    struct MassEntityHandleArray
    {
        MassEntityHandle* Data = nullptr;
        int32_t Num = 0;
        int32_t Max = 0;
    };

    // Native by-value argument, including the non-reflected cached full handle.
    struct alignas(8) PersistentEntityId
    {
        uint32_t Id = 0;
        uint32_t Padding = 0;
        MassEntityHandle CachedHandle;
    };
    static_assert(sizeof(PersistentEntityId) == 16);
    static_assert(offsetof(PersistentEntityId, CachedHandle) == 8);
    static_assert(offsetof(MassEntityHandle, SerialNumber) == 4);

    struct MassLifecycleApi
    {
        using BuildingNotificationFn = void (__fastcall*)(SDK::UCrBuildingStabilitySubsystem*, PersistentEntityId);
        BuildingNotificationFn onCreate = nullptr, onDestroy = nullptr;
    };

    struct ConstByteView64 { const uint8_t* Data = nullptr; int64_t Num = 0; };
    static_assert(sizeof(ConstByteView64) == 16 && alignof(ConstByteView64) == 8);
    static_assert(offsetof(ConstByteView64, Num) == 8);

    static_assert(
        sizeof(MassEntityHandleArray) ==
        0x10);


    struct MassApi
    {
        using GetMassEntitySubsystemFn =
            SDK::UMassEntitySubsystem* (__fastcall*)(
                const SDK::UWorld* world);

        using QueryConstructFn =
            void* (__fastcall*)(
                void* query,
                const void* managerSharedPtrStorage);

        using QueryDestructFn =
            void (__fastcall*)(
                void* query);

        using ForEachEntityChunkFn =
            void (__fastcall*)(
                FMassEntityQuery* query,
                FMassExecutionContext& executionContext,
                const TFunction<void(FMassExecutionContext&)>& callback);

        using ExecutionContextConstructFn =
            void (__fastcall*)(
                FMassExecutionContext* executionContext,
                FMassEntityManager& entityManager,
                float deltaTimeSeconds,
                bool flushDeferredCommands);

        using ExecutionContextDestructFn =
            void (__fastcall*)(
                FMassExecutionContext* executionContext);

        using AddFragmentRequirementFn =
            void* (__fastcall*)(
                void* requirements,
                uint8_t access,
                uint8_t presence);

        using AddEnemyStateRequirementFn =
            void* (__fastcall*)(void* requirements,
                SDK::EMassFragmentAccess access,
                SDK::EMassFragmentPresence presence);

        using AddTagRequirementFn =
            void (__fastcall*)(void* requirements,
                const SDK::UScriptStruct* tag,
                SDK::EMassFragmentPresence presence);

        using AddSharedRequirementFn =
            void* (__fastcall*)(
                void* requirements,
                uint8_t presence);

        using GetMatchingEntityHandlesFn =
            MassEntityHandleArray* (__fastcall*)(
                void* query,
                MassEntityHandleArray* returnStorage);

        using GetFragmentDataPtrFn =
            void* (__fastcall*)(
                const void* manager,
                MassEntityHandle entity,
                const SDK::UScriptStruct* fragmentType);

        using GetConstSharedFragmentPtrFn =
            const void* (__fastcall*)(
                const void* manager,
                MassEntityHandle entity,
                const SDK::UScriptStruct* fragmentType);

        using StaticStructFn =
            SDK::UScriptStruct* (__fastcall*)();

        GetMassEntitySubsystemFn getMassEntitySubsystem = nullptr;
        // Published once after Client load-hook field validation; -1 means unavailable.
        int32_t entityManagerFieldOffset = -1;
        QueryConstructFn queryConstruct = nullptr;
        QueryDestructFn queryDestruct = nullptr;
        ForEachEntityChunkFn forEachEntityChunk = nullptr;
        ExecutionContextConstructFn executionContextConstruct = nullptr;
        ExecutionContextDestructFn executionContextDestruct = nullptr;

        AddFragmentRequirementFn addInventoryRequirement = nullptr;
        AddFragmentRequirementFn addTransformRequirement = nullptr;
        AddFragmentRequirementFn addOptionalSplineRequirement = nullptr;
        AddFragmentRequirementFn addOptionalAPRequirement = nullptr;
        AddEnemyStateRequirementFn addEnemyStateRequirement = nullptr;
        AddTagRequirementFn addTagRequirement = nullptr;
        StaticStructFn enemyStateFragmentStaticStruct = nullptr;
        StaticStructFn enemyTagStaticStruct = nullptr;
        StaticStructFn neutralTagStaticStruct = nullptr;
        AddSharedRequirementFn addFoundableParametersRequirement = nullptr;
        AddSharedRequirementFn addFoundableTagRequirement = nullptr;
        AddSharedRequirementFn addOptionalBuildingParametersRequirement = nullptr;

        GetMatchingEntityHandlesFn getMatchingEntityHandles = nullptr;
        GetFragmentDataPtrFn getFragmentDataPtr = nullptr;
        GetConstSharedFragmentPtrFn getConstSharedFragmentPtr = nullptr;

        StaticStructFn transformFragmentStaticStruct = nullptr;
        StaticStructFn inventoryFragmentStaticStruct = nullptr;
        StaticStructFn foundableParametersStaticStruct = nullptr;
    };


    struct TextureApi
    {
        using CreateTransientFn = SDK::UTexture2D* (__fastcall*)(int32_t, int32_t,
            SDK::EPixelFormat, SDK::FName, ConstByteView64);
        using BulkLockFn = void* (__fastcall*)(void*, uint32_t);
        using BulkUnlockFn = void (__fastcall*)(const void*);
        using UpdateResourceFn = void (__fastcall*)(SDK::UTexture*);
        using GetResourceFn = void* (__fastcall*)(SDK::UTexture*);
        CreateTransientFn createTransient = nullptr;
        BulkLockFn bulkLock = nullptr;
        BulkUnlockFn bulkUnlock = nullptr;
        UpdateResourceFn updateResource = nullptr;
        GetResourceFn getResource = nullptr;
        using SubmitRenderCommandFn = void (*)(void*, const wchar_t*, uint32_t&, TStatId,
            TUniqueFunction<void(FRHICommandListImmediate&)>&&);
        SubmitRenderCommandFn submitRenderCommand = nullptr;
        using ReadbackConstructFn = FRHIGPUTextureReadback* (*)(FRHIGPUTextureReadback*, FName);
        ReadbackConstructFn readbackConstruct = nullptr;
        using ReadbackDestroyFn = void (*)(FRHIGPUTextureReadback*);
        ReadbackDestroyFn readbackDestroy = nullptr;
        using ReadbackCopyFn = void (*)(FRHIGPUTextureReadback*, FRHICommandList&, FRHITexture*, const UE::Math::TIntVector3<int32_t>&, uint32_t, const UE::Math::TIntVector3<int32_t>&);
        ReadbackCopyFn readbackCopy = nullptr;
        using ReadbackLockFn = void* (*)(FRHIGPUTextureReadback*, int32_t&, int32_t*);
        ReadbackLockFn readbackLock = nullptr;
        using ReadbackUnlockFn = void (*)(FRHIGPUTextureReadback*);
        ReadbackUnlockFn readbackUnlock = nullptr;
        using MarkRHIResourceForDeleteFn = void (*)(const FRHIResource*);
        MarkRHIResourceForDeleteFn markRHIResourceForDelete = nullptr;
        void* renderCommandPipe = nullptr;
        const uint32_t* explicitGpuCount = nullptr;

        using InitRenderTargetFn = void (*)(SDK::UTextureRenderTarget2D*, uint32_t, uint32_t, uint8_t, bool);
        InitRenderTargetFn initRenderTarget = nullptr;
        using GetRenderTargetResourceFn = void* (*)(SDK::UTextureRenderTarget2D*);
        GetRenderTargetResourceFn getRenderTargetResource = nullptr;
        using TransitionResourcesFn = void (*)(FRHIComputeCommandList*, TArrayView<const FRHITransitionInfo, int32_t>, ERHITransitionCreateFlags);
        TransitionResourcesFn transitionResources = nullptr;
        using RenderFenceConstructFn = void (*)(void*);
        RenderFenceConstructFn renderFenceConstruct = nullptr;
        using RenderFenceDestroyFn = void (*)(void*);
        RenderFenceDestroyFn renderFenceDestroy = nullptr;
        using RenderFenceBeginFn = void (*)(void*, int32_t);
        RenderFenceBeginFn renderFenceBegin = nullptr;
        using RenderFencePollFn = bool (*)(const void*);
        RenderFencePollFn renderFencePoll = nullptr;

        using GetBrushTextureFn =
            SDK::UTexture2D* (*)(
                const SDK::FSlateBrush&);

        using SetForceMipLevelsToBeResidentFn =
            void (*)(
                SDK::UStreamableRenderAsset*,
                float,
                int32_t);

        using WaitForStreamingFn =
            void (*)(
                SDK::UStreamableRenderAsset*,
                bool,
                bool);

        using GetNumResidentMipsFn =
            int32_t(*)(
                const SDK::UTexture2D*);

        using GetNumMipsAllowedFn =
            int32_t(*)(
                const SDK::UTexture2D*,
                bool);

        using GetNumMipsFn =
            int32_t(*)(
                const SDK::UTexture2D*);

        using StreamInFn =
            bool (*)(
                SDK::UTexture2D*,
                int32_t,
                bool);

        using WaitForPendingInitOrStreamingFn =
            void (*)(
                SDK::UStreamableRenderAsset*,
                bool,
                bool);

        using GetPlatformDataFn =
            void* (*)(
                SDK::UTexture2D*);

        using GetBulkDataSizeFn =
            int64_t(*)(
                const void*);

        using CanLoadFromDiskFn =
            bool (*)(
                const void*);

        using GetBulkDataCopyFn =
            void (*)(
                void*,
                void**,
                bool);

        using MemoryFreeFn =
            void (*)(
                void*);

        GetBrushTextureFn getBrushTexture = nullptr;

        SetForceMipLevelsToBeResidentFn
            setForceMipLevelsToBeResident = nullptr;

        WaitForStreamingFn
            waitForStreaming = nullptr;

        GetNumResidentMipsFn
            getNumResidentMips = nullptr;

        GetNumMipsAllowedFn
            getNumMipsAllowed = nullptr;

        GetNumMipsFn
            getNumMips = nullptr;

        StreamInFn
            streamIn = nullptr;

        WaitForPendingInitOrStreamingFn
            waitForPendingInitOrStreaming = nullptr;

        GetPlatformDataFn
            getPlatformData = nullptr;

        GetBulkDataSizeFn
            getBulkDataSize = nullptr;

        CanLoadFromDiskFn
            canLoadFromDisk = nullptr;

        GetBulkDataCopyFn
            getBulkDataCopy = nullptr;

        MemoryFreeFn
            memoryFree = nullptr;
    };


    // Existing private-world native argument layout, moved here solely to type its binding.
    // Win64 passes the 16-byte InitWorld value indirectly; CreateWorld takes a pointer.
    struct alignas(8) WorldInitializationValues
    {
        uint32_t Flags = 1;
        uint32_t Padding = 0;
        uintptr_t DefaultGameMode = 0;
    };
    static_assert(sizeof(WorldInitializationValues) == 16);
    static_assert(offsetof(WorldInitializationValues, DefaultGameMode) == 8);

    struct WorldApi
    {
        using CreateWorldFn = uintptr_t (*)(int32_t, bool, SDK::FName, uintptr_t, bool,
                                            int32_t, const WorldInitializationValues*, bool);
        using InitWorldFn = void (*)(uintptr_t, WorldInitializationValues);
        using UpdateWorldComponentsFn = void (*)(uintptr_t, bool, bool, void*);
        using CleanupWorldFn = void (*)(uintptr_t, bool, bool, uintptr_t);
        using PredicateFn = bool (*)();
        using DestroyActorFn = bool (*)(uintptr_t, bool, bool);
        CreateWorldFn createWorld = nullptr;
        InitWorldFn initWorld = nullptr;
        UpdateWorldComponentsFn updateWorldComponents = nullptr;
        CleanupWorldFn cleanupWorld = nullptr;
        PredicateFn isGarbageCollectingAndLocking = nullptr;
        PredicateFn isIncrementalPurgePending = nullptr;
        PredicateFn isIncrementalReachabilityPending = nullptr;
        DestroyActorFn destroyActor = nullptr;
    };

    // Stage 42G. Use the scoped BuildingSplinePayload adapter for numerical copies.
    // The caller must own a valid engine read scope on the game thread.
    struct SplinePayloadApi
    {
        // Explicit Win64 hidden return buffer (RCX), const handle (RDX), buffer in RAX.
        using GetFn = SDK::FAuAPSplineActorPayloadData* (__fastcall*)(
            SDK::FAuAPSplineActorPayloadData*, const SDK::FAuAPPayloadDataHandle*);
        // Non-deleting destructor: RCX is the constructed value, never its owner buffer.
        using DestroyFn = void (__fastcall*)(SDK::FAuAPSplineActorPayloadData*);
        using PayloadHandleCopyConstructFn = void (__fastcall*)(
            SDK::FAuAPPayloadDataHandle*,
            const SDK::FAuAPPayloadDataHandle*);
        using PayloadHandleDestructFn = void (__fastcall*)(
            SDK::FAuAPPayloadDataHandle*);
        GetFn get = nullptr;
        DestroyFn destroy = nullptr;
        PayloadHandleCopyConstructFn payloadHandleCopyConstruct = nullptr;
        PayloadHandleDestructFn payloadHandleDestruct = nullptr;
        SDK::UScriptStruct* payloadType = nullptr; // Engine static metadata, warmed only at initialization.
    };

    struct NativeApi
    {
        CommonUIApi commonUI;
        MassLifecycleApi massLifecycle;
        AssetApi asset;
        WorldApi world;
        PlayerApi player;
        PointsOfInterestApi pointsOfInterest;
        MassApi mass;
        SplinePayloadApi splinePayload;
        TextureApi texture;
    };


    bool Initialize(
        const MiniMapFingerprints::ResolvedAddresses& addresses);

    /** Publishes the exact validated native shared-pointer member for this binding lifecycle. */
    bool PublishMassEntityManagerField(
        int32_t offset,
        uint32_t size) noexcept;

    bool IsMemoryRangeReadable(
        uintptr_t address,
        size_t bytes,
        size_t alignment = 1);

    bool ReadMemory(
        uintptr_t address,
        void* output,
        size_t bytes);

    bool ValidateObjectIdentity(
        uintptr_t object,
        uintptr_t objectArray,
        size_t extent = 0x28);

    void Shutdown();

    bool IsInitialized();

    const NativeApi* Get();
}

#endif
