#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

#include <cstdint>

namespace SDK
{
    class UObject;
    class UWorld;
    class ACrCharacterPlayerBase;
    class UCrPlayerMapMenuDataComponent;
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
        uint64_t TrackingRevision = 0;
        int32_t TrackingObjectIndex = -1; // Reconciliation identity, never an object pointer.
        bool DroneActive = false;
        bool InForgottenEngine = false;
        bool Valid = false;
    };

    /** Holds resolved offsets for tracking fields and actor identity validation. */
    struct PlayerFieldOffsets
    {
        int32_t PlayerMainCharacter = -1;
        int32_t CharacterDead = -1;
        int32_t BuildingDrone = -1;
        int32_t DroneOwnerPlayer = -1;
        int32_t IsLocalPlayerController = -1;
        int32_t ControllerPawn = -1;
        int32_t PawnController = -1;
        int32_t CharacterStatus = -1;
        int32_t PlayerMapDataComponent = -1;
        int32_t ObjectIndex = -1;
        int32_t ObjectOuter = -1;
        int32_t ActorRootComponent = -1;
        int32_t ActorBeingDestroyed = -1;
        uint8_t ActorBeingDestroyedMask = 0;
        int32_t LevelOwningWorld = -1;
    };

    /** Publishes player field offsets resolved during plugin load for read-only access. */
    class PlayerFieldAccessors final
    {
    public:
        /** Stores resolved offsets for gameplay reads; fields remain read-only afterward. */
        static void SetOffsets(const PlayerFieldOffsets& offsets);
        /** Checks a live, game-thread UObject reference against its GObjects slot and flags. */
        static bool IsObjectRegistered(const SDK::UObject* object);

    private:
        /** Confirms a resolved object index points back to the same live object slot. */
        static bool MatchesObjectArrayEntry(
            const SDK::UObject* object,
            int32_t objectIndex,
            const void* objectArray);
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

    // Game-thread-only references borrowed from the current live player.
    bool TryGetPlayerMapDataComponent(
        SDK::ACrCharacterPlayerBase*& outPlayer,
        SDK::UCrPlayerMapMenuDataComponent*& outComponent);

    bool TryGetPlayerPose(
        PlayerPose& outPose);

    bool TryGetPlayerWorldPosition(
        SDK::FVector& outPosition);

    bool TryIsPlayerInForgottenEngine(
        bool& outIsInForgottenEngine);

    void Shutdown();
}

#endif
