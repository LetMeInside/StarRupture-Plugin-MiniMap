#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include <cstdint>

namespace SDK
{
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
    class UCrPlayerMapMenuDataComponent;

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

        GetAllActorsOfClassFn getAllActorsOfClass = nullptr;
        POIStaticClassFn pointOfInterestStaticClass = nullptr;
        FindPOIMarkerCategoryDataFn findPOIMarkerCategoryData = nullptr;
        GetMarkerFilterStatusFn getMarkerFilterStatus = nullptr;
        IsAbandonBaseCompletedFn isAbandonBaseCompleted = nullptr;
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
        TextureApi texture;
    };


    bool Initialize(
        const MiniMapFingerprints::ResolvedAddresses& addresses);

    void Shutdown();

    bool IsInitialized();

    const NativeApi* Get();
}

#endif