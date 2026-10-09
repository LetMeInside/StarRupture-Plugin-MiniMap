#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include <cstdint>

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
        TextureApi texture;
    };


    bool Initialize(
        const MiniMapFingerprints::ResolvedAddresses& addresses);

    void Shutdown();

    bool IsInitialized();

    const NativeApi* Get();
}

#endif