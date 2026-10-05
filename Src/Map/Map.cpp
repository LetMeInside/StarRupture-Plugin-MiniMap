#if defined(MODLOADER_CLIENT_BUILD)

#include "Map.h"

#include "SDK/Chimera_classes.hpp"
#include "SDK/Engine_classes.hpp"

#include "../Native/NativeApi.h"
#include "../plugin_helpers.h"

#include <cmath>

namespace
{
    SDK::UWorld* g_world = nullptr;
}


namespace MiniMapMap
{
    void SetWorld(
        SDK::UWorld* world)
    {
        g_world = world;

        LOG_INFO(
            "MiniMap: active gameplay world = %p",
            g_world);
    }


    SDK::UWorld* GetWorld()
    {
        return g_world;
    }


    bool HasWorld()
    {
        return g_world != nullptr;
    }


    bool TryGetPlayerPose(
        PlayerPose& outPose)
    {
        outPose = {};

        if (g_world == nullptr)
        {
            LOG_ERROR(
                "MiniMap: active gameplay world is unavailable");
            return false;
        }

        const MiniMapNative::NativeApi* native =
            MiniMapNative::Get();

        if (native == nullptr)
        {
            LOG_ERROR(
                "MiniMap: native API is unavailable");
            return false;
        }

        const auto& playerApi =
            native->player;

        if (playerApi.getFirstPlayerController == nullptr ||
            playerApi.getPlayerPawn == nullptr ||
            playerApi.getControlRotation == nullptr ||
            playerApi.getComponentLocation == nullptr)
        {
            LOG_ERROR(
                "MiniMap: player native API is incomplete");
            return false;
        }

        SDK::APlayerController* playerController =
            playerApi.getFirstPlayerController(
                g_world);

        if (playerController == nullptr)
        {
            LOG_ERROR(
                "MiniMap: first player controller is unavailable");
            return false;
        }

        SDK::ACrCharacterPlayerBase* playerPawn =
            playerApi.getPlayerPawn(
                static_cast<const SDK::AController*>(
                    playerController));

        if (playerPawn == nullptr)
        {
            LOG_ERROR(
                "MiniMap: local player pawn is unavailable");
            return false;
        }

        SDK::USceneComponent* rootComponent =
            playerPawn->RootComponent;

        if (rootComponent == nullptr)
        {
            LOG_ERROR(
                "MiniMap: local player root component is unavailable");
            return false;
        }

        SDK::FVector playerLocation = {};
        playerApi.getComponentLocation(
            rootComponent,
            &playerLocation);

        SDK::FRotator controlRotation = {};
        playerApi.getControlRotation(
            static_cast<const SDK::AController*>(
                playerController),
            &controlRotation);

        if (!std::isfinite(playerLocation.X) ||
            !std::isfinite(playerLocation.Y) ||
            !std::isfinite(playerLocation.Z) ||
            !std::isfinite(controlRotation.Yaw))
        {
            return false;
        }

        outPose.WorldX = playerLocation.X;
        outPose.WorldY = playerLocation.Y;
        outPose.WorldZ = playerLocation.Z;
        outPose.ControlYawDegrees = controlRotation.Yaw;
        return true;
    }


    bool TryGetPlayerWorldPosition(
        SDK::FVector& outPosition)
    {
        PlayerPose pose = {};

        if (!TryGetPlayerPose(
            pose))
        {
            return false;
        }

        outPosition = {};
        outPosition.X = pose.WorldX;
        outPosition.Y = pose.WorldY;
        outPosition.Z = pose.WorldZ;
        return true;
    }

    bool TryIsPlayerInForgottenEngine(
        bool& outIsInForgottenEngine)
    {
        outIsInForgottenEngine =
            false;

        if (g_world == nullptr)
        {
            return false;
        }

        const MiniMapNative::NativeApi* native =
            MiniMapNative::Get();

        if (native == nullptr)
        {
            return false;
        }

        const auto& playerApi =
            native->player;

        if (playerApi.getFirstPlayerController == nullptr ||
            playerApi.getPlayerPawn == nullptr ||
            playerApi.isPlayerInForgottenEngine == nullptr)
        {
            return false;
        }

        SDK::APlayerController* playerController =
            playerApi.getFirstPlayerController(
                g_world);

        if (playerController == nullptr)
        {
            return false;
        }

        SDK::ACrCharacterPlayerBase* playerPawn =
            playerApi.getPlayerPawn(
                static_cast<const SDK::AController*>(
                    playerController));

        if (playerPawn == nullptr)
        {
            return false;
        }

        outIsInForgottenEngine =
            playerApi.isPlayerInForgottenEngine(
                playerPawn);

        return true;
    }


    void Shutdown()
    {
        g_world = nullptr;
    }
}

#endif
