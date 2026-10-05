#pragma once

#if defined(MODLOADER_CLIENT_BUILD)

namespace SDK
{
    class UWorld;
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


    void SetWorld(SDK::UWorld* world);
    SDK::UWorld* GetWorld();
    bool HasWorld();

    bool TryGetPlayerPose(
        PlayerPose& outPose);

    bool TryGetPlayerWorldPosition(
        SDK::FVector& outPosition);

    bool TryIsPlayerInForgottenEngine(
        bool& outIsInForgottenEngine);

    void Shutdown();
}

#endif
