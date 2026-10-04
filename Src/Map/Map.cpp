#if defined(MODLOADER_CLIENT_BUILD)

#include "Map.h"

#include "../plugin.h"
#include "../plugin_helpers.h"

#include "SDK/Chimera_classes.hpp"
#include "SDK/Engine_classes.hpp"

namespace
{
    SDK::UWorld* g_world = nullptr;


    using GetFirstPlayerControllerFn =
        SDK::APlayerController* (*)(
            const SDK::UWorld*);

    using GetPlayerPawnFn =
        SDK::ACrCharacterPlayerBase* (*)(
            const SDK::AController*);

    using GetComponentLocationFn =
        SDK::FVector* (*)(
            const SDK::USceneComponent*,
            SDK::FVector*);
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

        const uintptr_t getFirstControllerAddress =
            GetFirstPlayerControllerAddress();

        const uintptr_t getPlayerPawnAddress =
            GetPlayerPawnAddress();

        const uintptr_t getComponentLocationAddress =
            GetComponentLocationAddress();

        if (getFirstControllerAddress == 0 ||
            getPlayerPawnAddress == 0 ||
            getComponentLocationAddress == 0)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "one or more required native functions are unavailable");

            return false;
        }

        auto getFirstPlayerController =
            reinterpret_cast<GetFirstPlayerControllerFn>(
                getFirstControllerAddress);

        auto getPlayerPawn =
            reinterpret_cast<GetPlayerPawnFn>(
                getPlayerPawnAddress);

        auto getComponentLocation =
            reinterpret_cast<GetComponentLocationFn>(
                getComponentLocationAddress);

        SDK::APlayerController* playerController =
            getFirstPlayerController(
                g_world);

        if (playerController == nullptr)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "first player controller was not found");

            return false;
        }

        SDK::ACrCharacterPlayerBase* playerPawn =
            getPlayerPawn(
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

        getComponentLocation(
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