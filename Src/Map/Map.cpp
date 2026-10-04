#if defined(MODLOADER_CLIENT_BUILD)

#include "Map.h"
#include "../Native/NativeApi.h"
#include "../plugin.h"
#include "../plugin_helpers.h"

#include "SDK/Chimera_classes.hpp"
#include "SDK/Engine_classes.hpp"

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


    bool HasWorld()
    {
        return g_world != nullptr;
    }


    bool TryGetPlayerWorldPosition(
        SDK::FVector& outPosition)
    {
        if (g_world == nullptr)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "active gameplay world is unavailable");

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
                "MiniMap: F8 diagnostic: "
                "first player controller was not found");

            return false;
        }

        SDK::ACrCharacterPlayerBase* playerPawn =
            playerApi.getPlayerPawn(
                static_cast<const SDK::AController*>(
                    playerController));

        if (playerPawn == nullptr)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "local CrCharacterPlayerBase pawn was not found");

            return false;
        }

        SDK::USceneComponent* rootComponent =
            playerPawn->RootComponent;

        if (rootComponent == nullptr)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "local player pawn has no RootComponent");

            return false;
        }

        outPosition = {};

        playerApi.getComponentLocation(
            rootComponent,
            &outPosition);

        return true;
    }


    void Shutdown()
    {
        g_world = nullptr;
    }
}

#endif