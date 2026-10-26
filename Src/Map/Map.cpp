#if defined(MODLOADER_CLIENT_BUILD)

#include "Map.h"
#include "SDK/Chimera_classes.hpp"
#include "SDK/Engine_classes.hpp"
#include "SDK/Engine_parameters.hpp"
#include "../Native/NativeApi.h"
#include "../plugin_helpers.h"
#include <cmath>
#include <atomic>

namespace
{
    SDK::UWorld* g_world = nullptr;
    std::atomic<bool> g_gameplayReady{false};
    std::atomic<uint64_t> g_worldGeneration{0};

    bool ValidObject(const SDK::UObject* object, size_t extent)
    {
        const auto* native = MiniMapNative::Get();
        return native && MiniMapNative::ValidateObjectIdentity(
            reinterpret_cast<uintptr_t>(object),
            reinterpret_cast<uintptr_t>(native->asset.objectArray), extent);
    }

    template<class T> bool IsObject(const SDK::UObject* object)
    {
        if (!ValidObject(object, sizeof(T)) || !ValidObject(object->Class, sizeof(SDK::UClass))) return false;
        auto* type = T::StaticClass();
        return ValidObject(type, sizeof(SDK::UClass)) && object->IsA(type);
    }

    bool InWorld(const SDK::AActor* actor)
    {
        if (!ValidObject(actor, sizeof(SDK::AActor)) || actor->bActorIsBeingDestroyed) return false;
        // Actors are owned by a level; use its current SDK world association.
        auto* level = actor->Outer;
        return IsObject<SDK::ULevel>(level) &&
            static_cast<const SDK::ULevel*>(level)->OwningWorld == g_world;
    }

    struct LocalPlayer
    {
        SDK::ACrPlayerControllerBase* Controller = nullptr;
        SDK::ACrCharacterPlayerBase* Character = nullptr;
        SDK::APawn* Pawn = nullptr;
    };

    bool ResolveLocalPlayer(LocalPlayer& out)
    {
        out = {};
        const auto* native = MiniMapNative::Get();
        if (!g_world || !native || !native->player.isGameThread || !native->player.isGameThread() ||
            !native->player.getFirstPlayerController || !ValidObject(g_world, sizeof(SDK::UWorld))) return false;
        auto* controller = native->player.getFirstPlayerController(g_world);
        if (!IsObject<SDK::ACrPlayerControllerBase>(controller) || !InWorld(controller) ||
            !controller->bIsLocalPlayerController) return false;
        auto* pc = static_cast<SDK::ACrPlayerControllerBase*>(controller);
        auto* character = pc->PlayerMainCharacter;
        if (!IsObject<SDK::ACrCharacterPlayerBase>(character) || !InWorld(character) || character->bDead) return false;
        auto* pawn = pc->Pawn;
        if (!IsObject<SDK::APawn>(pawn) || !InWorld(pawn) || pawn->Controller != pc) return false;
        if (pawn != character)
        {
            if (!IsObject<SDK::ACrCharacterDroneBase>(pawn) ||
                static_cast<SDK::ACrCharacterDroneBase*>(pawn)->OwnerPlayer != character) return false;
        }
        out = {pc, character, pawn};
        return true;
    }

    bool ReadPosition(const SDK::AActor* actor, MiniMapMap::PlayerPose& pose)
    {
        const auto* native = MiniMapNative::Get();
        auto* root = actor->RootComponent;
        if (!native || !native->player.getComponentLocation ||
            !IsObject<SDK::USceneComponent>(root) || root->Outer != actor) return false;
        SDK::FVector location{};
        native->player.getComponentLocation(root, &location);
        if (!std::isfinite(location.X) || !std::isfinite(location.Y) || !std::isfinite(location.Z)) return false;
        pose.WorldX = location.X; pose.WorldY = location.Y; pose.WorldZ = location.Z;
        return true;
    }

    bool ReadControlPose(const LocalPlayer& player, MiniMapMap::PlayerPose& pose)
    {
        pose = {};
        const auto* native = MiniMapNative::Get();
        if (!native || !native->player.getControlRotation || !ReadPosition(player.Character, pose)) return false;
        SDK::FRotator rotation{};
        native->player.getControlRotation(player.Controller, &rotation);
        pose.ControlYawDegrees = rotation.Yaw;
        return std::isfinite(rotation.Yaw);
    }

    bool ReadWorldYaw(const SDK::AActor* actor, double& yaw)
    {
        // Existing SDK reflection access, with an explicit failure check rather
        // than the generated wrapper's unchecked cached UFunction dereference.
        auto* function = actor->Class->GetFunction("Actor", "K2_GetActorRotation");
        if (!ValidObject(function, sizeof(SDK::UFunction))) return false;
        SDK::Params::Actor_K2_GetActorRotation parameters{};
        actor->ProcessEvent(function, &parameters);
        yaw = parameters.ReturnValue.Yaw;
        return std::isfinite(yaw);
    }
}

namespace MiniMapMap
{
    void SetWorld(SDK::UWorld* world)
    {
        g_gameplayReady.store(false, std::memory_order_release);
        g_world = world;
        g_worldGeneration.fetch_add(1, std::memory_order_acq_rel);
        LOG_INFO("MiniMap: active gameplay world = %p", g_world);
    }
    SDK::UWorld* GetWorld() { return g_world; }
    bool HasWorld() { return g_world != nullptr; }
    bool GameplayReady() { return g_gameplayReady.load(std::memory_order_acquire); }
    void SetGameplayReady(bool ready) { g_gameplayReady.store(ready, std::memory_order_release); }
    uint64_t GetWorldGeneration() { return g_worldGeneration.load(std::memory_order_acquire); }

    bool TryGetPresentationSample(PresentationSample& outSample)
    {
        outSample = {};
        LocalPlayer player;
        PresentationSample sample{};
        const auto* native = MiniMapNative::Get();
        if (!ResolveLocalPlayer(player) || !native->player.isPlayerInForgottenEngine ||
            !ReadControlPose(player, sample.Character)) return false;
        sample.WorldGeneration = GetWorldGeneration();
        sample.InForgottenEngine = native->player.isPlayerInForgottenEngine(player.Character);
        sample.Tracking = sample.Character;
        sample.TrackingObjectIndex = player.Character->Index;
        // Only a complete current association can select the drone. Missing or
        // retiring drone state uses the valid physical character, never history.
        auto* drone = player.Character->BuildingDrone;
        if (player.Character->Status == SDK::EPlayerCharacterStatus::BuildingDrone &&
            IsObject<SDK::ACrCharacterDroneBase>(drone) && InWorld(drone) &&
            drone->OwnerPlayer == player.Character &&
            (player.Pawn == player.Character || player.Pawn == drone))
        {
            PlayerPose dronePose{};
            if (ReadPosition(drone, dronePose))
            {
                if (!ReadWorldYaw(drone, dronePose.ControlYawDegrees) ||
                    !ReadWorldYaw(player.Character, sample.CharacterWorldYawDegrees)) return false;
                sample.Tracking = dronePose;
                sample.TrackingObjectIndex = drone->Index;
                sample.DroneActive = true;
            }
        }
        sample.LocalVisibilityOrigin = {sample.Tracking.WorldX, sample.Tracking.WorldY, sample.Tracking.WorldZ};
        sample.Valid = sample.WorldGeneration == GetWorldGeneration();
        if (!sample.Valid) return false;
        outSample = sample;
        return true;
    }

    bool TryGetPhysicalCharacter(SDK::ACrCharacterPlayerBase*& outCharacter)
    {
        outCharacter = nullptr;
        LocalPlayer player;
        if (!ResolveLocalPlayer(player)) return false;
        outCharacter = player.Character;
        return true;
    }

    bool TryGetPlayerPose(PlayerPose& outPose)
    {
        outPose = {};
        LocalPlayer player;
        return ResolveLocalPlayer(player) && ReadControlPose(player, outPose);
    }

    bool TryGetPlayerWorldPosition(SDK::FVector& outPosition)
    {
        outPosition = {};
        PlayerPose pose{};
        if (!TryGetPlayerPose(pose)) return false;
        outPosition.X = pose.WorldX; outPosition.Y = pose.WorldY; outPosition.Z = pose.WorldZ;
        return true;
    }

    bool TryIsPlayerInForgottenEngine(bool& outIsInForgottenEngine)
    {
        outIsInForgottenEngine = false;
        LocalPlayer player;
        const auto* native = MiniMapNative::Get();
        if (!ResolveLocalPlayer(player) || !native->player.isPlayerInForgottenEngine) return false;
        outIsInForgottenEngine = native->player.isPlayerInForgottenEngine(player.Character);
        return true;
    }

    void Shutdown() { SetWorld(nullptr); }
}
#endif
