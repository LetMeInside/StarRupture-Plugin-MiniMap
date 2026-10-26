#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include <cstdint>

namespace SDK
{
    class UWorld;
    class ACrCharacterPlayerBase;
    struct FVector;
}

namespace MiniMapMap
{
    struct PlayerPose
    {
        double WorldX = 0.0;
        double WorldY = 0.0;
        double WorldZ = 0.0;
        double ControlYawDegrees = 0.0;
    };

    struct WorldPosition
    {
        double WorldX = 0.0;
        double WorldY = 0.0;
        double WorldZ = 0.0;
    };

    // Values only: no actor/component ownership crosses the game-thread boundary.
    struct PresentationSample
    {
        PlayerPose Character = {};
        double CharacterWorldYawDegrees = 0.0; // Required/acquired only for DroneActive.
        PlayerPose Tracking = {};
        WorldPosition LocalVisibilityOrigin = {};
        uint64_t WorldGeneration = 0;
        int32_t TrackingObjectIndex = -1; // Reconciliation identity, never an object pointer.
        bool DroneActive = false;
        bool InForgottenEngine = false;
        bool Valid = false;
    };


    void SetWorld(SDK::UWorld* world);
    SDK::UWorld* GetWorld();
    bool HasWorld();
    bool GameplayReady();
    void SetGameplayReady(bool ready);
    uint64_t GetWorldGeneration();

    bool TryGetPresentationSample(PresentationSample& outSample);

    // Game-thread-only borrowed pointer, valid only for the current acquisition.
    // Resolves the current physical character without pose/heading dependencies.
    bool TryGetPhysicalCharacter(SDK::ACrCharacterPlayerBase*& outCharacter);

    bool TryGetPlayerPose(
        PlayerPose& outPose);

    bool TryGetPlayerWorldPosition(
        SDK::FVector& outPosition);

    bool TryIsPlayerInForgottenEngine(
        bool& outIsInForgottenEngine);

    void Shutdown();
}

#endif
