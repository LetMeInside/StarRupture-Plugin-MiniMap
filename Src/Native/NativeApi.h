#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include <cstdint>
#include <cstddef>

namespace SDK
{
    enum class EMassFragmentAccess : uint8_t;
    enum class EMassFragmentPresence : uint8_t;
    class UObject;
    class UTexture2D;
    class UStreamableRenderAsset;

    class UWorld;
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
    struct FBox;
}

namespace MiniMapFingerprints
{
    struct ResolvedAddresses;
}

namespace MiniMapNative
{
    struct AssetApi
    {
        using LoadSynchronousFn =
            SDK::UObject* (*)(void*);

        LoadSynchronousFn loadSynchronous = nullptr;
    };


    struct PlayerApi
    {
        using GetFirstPlayerControllerFn =
            SDK::APlayerController* (*)(
                const SDK::UWorld*);

        using GetPlayerPawnFn =
            SDK::ACrCharacterPlayerBase* (*)(
                const SDK::AController*);

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
        GetPlayerPawnFn getPlayerPawn = nullptr;
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
        QueryConstructFn queryConstruct = nullptr;
        QueryDestructFn queryDestruct = nullptr;

        AddFragmentRequirementFn addInventoryRequirement = nullptr;
        AddFragmentRequirementFn addTransformRequirement = nullptr;
        AddFragmentRequirementFn addSplineRequirement = nullptr;
        using EntityCheckFn = bool (__fastcall*)(const void*, MassEntityHandle);
        EntityCheckFn isEntityValid = nullptr;
        EntityCheckFn isEntityBuilt = nullptr;
        AddEnemyStateRequirementFn addEnemyStateRequirement = nullptr;
        AddTagRequirementFn addTagRequirement = nullptr;
        StaticStructFn enemyStateFragmentStaticStruct = nullptr;
        StaticStructFn enemyTagStaticStruct = nullptr;
        StaticStructFn neutralTagStaticStruct = nullptr;
        AddSharedRequirementFn addFoundableParametersRequirement = nullptr;
        AddSharedRequirementFn addFoundableTagRequirement = nullptr;

        GetMatchingEntityHandlesFn getMatchingEntityHandles = nullptr;
        GetFragmentDataPtrFn getFragmentDataPtr = nullptr;
        GetConstSharedFragmentPtrFn getConstSharedFragmentPtr = nullptr;

        StaticStructFn transformFragmentStaticStruct = nullptr;
        StaticStructFn inventoryFragmentStaticStruct = nullptr;
        StaticStructFn foundableParametersStaticStruct = nullptr;
    };


    // CL-127004 FCrGridEntityHandle: handle + TSharedPtr<Data, ESPMode::ThreadSafe>.
    // Opaque shared-pointer words are owned by native code, never copied out.
    struct GridEntityHandle
    {
        MassEntityHandle Entity;
        void* Data = nullptr;
        void* ReferenceController = nullptr;
    };
    static_assert(sizeof(GridEntityHandle) == 0x18);
    static_assert(alignof(GridEntityHandle) == 8);
    static_assert(offsetof(GridEntityHandle, Data) == 0x08);
    static_assert(offsetof(GridEntityHandle, ReferenceController) == 0x10);

    struct GridResultArray
    {
        GridEntityHandle* Data = nullptr;
        int32_t Num = 0;
        int32_t Max = 0;
    };
    static_assert(sizeof(GridResultArray) == 0x10);

    struct GridDiagnosticApi
    {
        // Verified CL-127004 TFunctionRef: Callable +0, Storage.Object +8.
        // Synchronous borrowed callback; no native ownership/destruction.
        struct VisitorRef
        {
            using InvokeFn = bool (__fastcall*)(void*, const GridEntityHandle&);
            InvokeFn Callable = nullptr;
            void* Object = nullptr;
        };
        static_assert(sizeof(VisitorRef) == 0x10);
        static_assert(offsetof(VisitorRef, Object) == 8);
        using ForEachCellInRadiusFn = void (__fastcall*)(void*, const SDK::FVector&,
            float, const VisitorRef&, bool testEntityRadius);
        using GetSubsystemFn = void* (__fastcall*)(const SDK::UWorld*);
        using FindInBoxFn = void (__fastcall*)(void* subsystem, const SDK::FBox&,
            GridResultArray&, const void* requiredTags, const void* excludedTags);
        using DestructItemsFn = void (__fastcall*)(GridEntityHandle*, int32_t);
        using ViewConstructFn = void* (__fastcall*)(void* view, const void* manager, MassEntityHandle);
        using ViewHasTagFn = bool (__fastcall*)(const void* view, const SDK::UScriptStruct*);

        GetSubsystemFn getSubsystem = nullptr;
        ForEachCellInRadiusFn forEachCellInRadius = nullptr;
        FindInBoxFn findInBox = nullptr;
        DestructItemsFn destructItems = nullptr;
        ViewConstructFn viewConstruct = nullptr;
        ViewHasTagFn viewHasTag = nullptr;

        bool IsAvailable() const
        {
            return getSubsystem && findInBox && destructItems && viewConstruct && viewHasTag;
        }
    };

    struct TextureApi
    {
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


    struct NativeApi
    {
        AssetApi asset;
        PlayerApi player;
        PointsOfInterestApi pointsOfInterest;
        MassApi mass;
        GridDiagnosticApi gridDiagnostic;
        TextureApi texture;
    };


    bool Initialize(
        const MiniMapFingerprints::ResolvedAddresses& addresses);

    void Shutdown();

    bool IsInitialized();

    const NativeApi* Get();
}

#endif
