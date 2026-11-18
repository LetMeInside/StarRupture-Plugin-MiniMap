#if defined(MODLOADER_CLIENT_BUILD)

#include "Map.h"
#include "../Config/Config.h"
#include "SDK/Chimera_classes.hpp"
#include "SDK/Chimera_structs.hpp"
#include "SDK/Engine_classes.hpp"
#include "SDK/Engine_parameters.hpp"
#include "Map/RuntimeFieldAccess.h"
#include "../Native/NativeApi.h"
#include "../Native/DefinitionAcquisitionCore.h"
#include "../plugin_helpers.h"
#include "../DiagnosticExports.h"
#include <cstddef>
#include <cstring>
#include <cmath>
#include <atomic>
#include <type_traits>

static_assert(sizeof(void*) == 0x08);
static_assert(sizeof(decltype(SDK::ACrPlayerControllerBase::PlayerMainCharacter)) == sizeof(void*));
static_assert(std::is_same_v<
    decltype(SDK::ACrPlayerControllerBase::PlayerMainCharacter), SDK::ACrCharacterPlayerBase*>);
static_assert(sizeof(decltype(SDK::ACrCharacterPlayerBase::bDead)) == 0x01);
static_assert(std::is_same_v<decltype(SDK::ACrCharacterPlayerBase::bDead), bool>);
static_assert(sizeof(decltype(SDK::ACrCharacterPlayerBase::BuildingDrone)) == sizeof(void*));
static_assert(std::is_same_v<
    decltype(SDK::ACrCharacterPlayerBase::BuildingDrone), SDK::ACrCharacterDroneBase*>);
static_assert(sizeof(decltype(SDK::ACrCharacterDroneBase::OwnerPlayer)) == sizeof(void*));
static_assert(std::is_same_v<
    decltype(SDK::ACrCharacterDroneBase::OwnerPlayer), SDK::ACrCharacterPlayerBase*>);
static_assert(sizeof(decltype(SDK::APlayerController::bIsLocalPlayerController)) == 0x01);
static_assert(std::is_same_v<decltype(SDK::APlayerController::bIsLocalPlayerController), bool>);
static_assert(sizeof(decltype(SDK::AController::Pawn)) == sizeof(void*));
static_assert(std::is_same_v<decltype(SDK::AController::Pawn), SDK::APawn*>);
static_assert(sizeof(decltype(SDK::APawn::Controller)) == sizeof(void*));
static_assert(std::is_same_v<decltype(SDK::APawn::Controller), SDK::AController*>);
static_assert(sizeof(decltype(SDK::ACrCharacterPlayerBase::Status)) == 0x04);
static_assert(std::is_same_v<
    decltype(SDK::ACrCharacterPlayerBase::Status), SDK::EPlayerCharacterStatus>);
static_assert(std::is_same_v<
    std::underlying_type_t<SDK::EPlayerCharacterStatus>, uint32_t>);

namespace
{
    SDK::UWorld* g_world = nullptr;
    std::atomic<bool> g_gameplayReady{false};
    std::atomic<uint64_t> g_worldGeneration{0};
    int32_t g_playerMainCharacterOffset = -1;
    int32_t g_characterDeadOffset = -1;
    int32_t g_buildingDroneOffset = -1;
    int32_t g_droneOwnerPlayerOffset = -1;
    int32_t g_isLocalPlayerControllerOffset = -1;
    int32_t g_controllerPawnOffset = -1;
    int32_t g_pawnControllerOffset = -1;
    int32_t g_characterStatusOffset = -1;
    int32_t g_playerMapDataComponentOffset = -1;
    int32_t g_objectIndexOffset = -1;
    int32_t g_objectOuterOffset = -1;
    int32_t g_actorRootComponentOffset = -1;
    int32_t g_actorBeingDestroyedOffset = -1;
    uint8_t g_actorBeingDestroyedMask = 0;
    int32_t g_levelOwningWorldOffset = -1;

    template<class T> bool IsObject(const SDK::UObject* object)
    {
        if (!MiniMapMap::PlayerFieldAccessors::IsObjectRegistered(object) ||
            !MiniMapMap::PlayerFieldAccessors::IsObjectRegistered(object->Class)) return false;
        auto* type = T::StaticClass();
        return MiniMapMap::PlayerFieldAccessors::IsObjectRegistered(type) && object->IsA(type);
    }

    bool InWorld(const SDK::AActor* actor)
    {
        if (!MiniMapMap::PlayerFieldAccessors::IsObjectRegistered(actor) ||
            g_actorBeingDestroyedOffset < 0 ||
            g_actorBeingDestroyedMask == 0 ||
            MiniMap::MapRuntimeAccess::GetBit(
                actor, g_actorBeingDestroyedOffset, g_actorBeingDestroyedMask)) return false;
        SDK::UObject* const* outer = MiniMap::MapRuntimeAccess::Field<SDK::UObject*>(
            actor, g_objectOuterOffset);
        if (!outer || !IsObject<SDK::ULevel>(*outer)) return false;
        const auto* level = static_cast<const SDK::ULevel*>(*outer);
        SDK::UWorld* const* owningWorld = MiniMap::MapRuntimeAccess::Field<SDK::UWorld*>(
            level, g_levelOwningWorldOffset);
        return owningWorld && *owningWorld == g_world;
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
            !native->player.getFirstPlayerController ||
            !MiniMapMap::PlayerFieldAccessors::IsObjectRegistered(g_world)) return false;
        auto* controller = native->player.getFirstPlayerController(g_world);
        if (!IsObject<SDK::ACrPlayerControllerBase>(controller) || !InWorld(controller)) return false;
        const bool* isLocalPlayerController = MiniMap::MapRuntimeAccess::Field<bool>(
            controller, g_isLocalPlayerControllerOffset);
        if (!isLocalPlayerController || !*isLocalPlayerController) return false;
        auto* pc = static_cast<SDK::ACrPlayerControllerBase*>(controller);
        SDK::ACrCharacterPlayerBase* const* mainCharacterField =
            MiniMap::MapRuntimeAccess::Field<SDK::ACrCharacterPlayerBase*>(
                pc, g_playerMainCharacterOffset);
        if (!mainCharacterField) return false;
        auto* character = *mainCharacterField;
        if (!IsObject<SDK::ACrCharacterPlayerBase>(character) || !InWorld(character)) return false;
        const bool* characterDead = MiniMap::MapRuntimeAccess::Field<bool>(
            character, g_characterDeadOffset);
        if (!characterDead || *characterDead) return false;
        SDK::APawn* const* controllerPawn = MiniMap::MapRuntimeAccess::Field<SDK::APawn*>(
            pc, g_controllerPawnOffset);
        if (!controllerPawn) return false;
        auto* pawn = *controllerPawn;
        if (!IsObject<SDK::APawn>(pawn) || !InWorld(pawn)) return false;
        SDK::AController* const* pawnController = MiniMap::MapRuntimeAccess::Field<SDK::AController*>(
            pawn, g_pawnControllerOffset);
        if (!pawnController || *pawnController != pc) return false;
        if (pawn != character)
        {
            if (!IsObject<SDK::ACrCharacterDroneBase>(pawn)) return false;
            auto* drone = static_cast<SDK::ACrCharacterDroneBase*>(pawn);
            SDK::ACrCharacterPlayerBase* const* droneOwner =
                MiniMap::MapRuntimeAccess::Field<SDK::ACrCharacterPlayerBase*>(
                    drone, g_droneOwnerPlayerOffset);
            if (!droneOwner || *droneOwner != character) return false;
        }
        out = {pc, character, pawn};
        return true;
    }

    bool ReadPosition(const SDK::AActor* actor, MiniMapMap::PlayerPose& pose)
    {
        const auto* native = MiniMapNative::Get();
        if (!native || !native->player.getComponentLocation) return false;
        // Both callers validate the actor before this resolved field access.
        SDK::USceneComponent* const* rootField = MiniMap::MapRuntimeAccess::Field<SDK::USceneComponent*>(
            actor, g_actorRootComponentOffset);
        if (!rootField || !IsObject<SDK::USceneComponent>(*rootField)) return false;
        auto* root = *rootField;
        SDK::UObject* const* rootOuter = MiniMap::MapRuntimeAccess::Field<SDK::UObject*>(
            root, g_objectOuterOffset);
        if (!rootOuter || *rootOuter != actor) return false;
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
        if (!MiniMapMap::PlayerFieldAccessors::IsObjectRegistered(function)) return false;
        SDK::Params::Actor_K2_GetActorRotation parameters{};
        actor->ProcessEvent(function, &parameters);
        yaw = parameters.ReturnValue.Yaw;
        return std::isfinite(yaw);
    }

    /** Stateless helper that applies drone tracking after validating its live associations. */
    class BuildingDroneTracking final
    {
    public:
        /**
         * Updates the sample when the selected building drone is fully validated.
         * @param player The already validated local controller, character, and pawn.
         * @param followBuildDrone Whether settings permit selecting the controlled drone.
         * @param sample The character sample updated only when the drone pose is usable.
         * @return False when a required resolved field or the character heading cannot be read.
         */
        static bool TryApply(
            const LocalPlayer& player,
            bool followBuildDrone,
            MiniMapMap::PresentationSample& sample)
        {
            SDK::ACrCharacterDroneBase* const* buildingDrone =
                MiniMap::MapRuntimeAccess::Field<SDK::ACrCharacterDroneBase*>(
                    player.Character, g_buildingDroneOffset);
            if (!buildingDrone)
                return false;

            auto* drone = *buildingDrone;
            const SDK::EPlayerCharacterStatus* status =
                MiniMap::MapRuntimeAccess::Field<SDK::EPlayerCharacterStatus>(
                    player.Character, g_characterStatusOffset);
            if (!status)
                return false;

            if (!followBuildDrone || *status != SDK::EPlayerCharacterStatus::BuildingDrone ||
                !IsObject<SDK::ACrCharacterDroneBase>(drone) || !InWorld(drone))
                return true;

            SDK::ACrCharacterPlayerBase* const* owner =
                MiniMap::MapRuntimeAccess::Field<SDK::ACrCharacterPlayerBase*>(
                    drone, g_droneOwnerPlayerOffset);
            if (!owner)
                return false;

            if (*owner != player.Character ||
                (player.Pawn != player.Character && player.Pawn != drone))
                return true;

            const int32_t* trackingObjectIndex = MiniMap::MapRuntimeAccess::Field<int32_t>(
                drone, g_objectIndexOffset);
            if (!trackingObjectIndex)
                return false;

            MiniMapMap::PlayerPose dronePose{};
            if (!ReadPosition(drone, dronePose))
                return true;

            dronePose.ControlYawDegrees = sample.Character.ControlYawDegrees;
            if (!ReadWorldYaw(player.Character, sample.CharacterWorldYawDegrees))
                return false;

            sample.Tracking = dronePose;
            sample.TrackingObjectIndex = *trackingObjectIndex;
            sample.DroneActive = true;
            return true;
        }
    };
}

namespace MiniMapMap
{
    void SetWorld(SDK::UWorld* world)
    {
        g_gameplayReady.store(false, std::memory_order_release);
        g_world = world;
        g_worldGeneration.fetch_add(1, std::memory_order_acq_rel);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        LOG_INFO("MiniMap: active gameplay world = %p", g_world);
#endif
    }
    SDK::UWorld* GetWorld() { return g_world; }
    bool HasWorld() { return g_world != nullptr; }
    bool GameplayReady() { return g_gameplayReady.load(std::memory_order_acquire); }
    void SetGameplayReady(bool ready) { g_gameplayReady.store(ready, std::memory_order_release); }
    uint64_t GetWorldGeneration() { return g_worldGeneration.load(std::memory_order_acquire); }

    bool PlayerFieldAccessors::IsObjectRegistered(const SDK::UObject* object)
    {
        // The object is borrowed synchronously from the active world, a live
        // UObject member, or the SDK class registry before its index is read.
        const auto* native = MiniMapNative::Get();
        if (!object || !native || !native->asset.objectArray)
            return false;

        const int32_t* objectIndex = MiniMap::MapRuntimeAccess::Field<int32_t>(
            object, g_objectIndexOffset);
        return objectIndex && *objectIndex >= 0 &&
            MatchesObjectArrayEntry(object, *objectIndex, native->asset.objectArray);
    }

    bool PlayerFieldAccessors::MatchesObjectArrayEntry(
        const SDK::UObject* object,
        int32_t objectIndex,
        const void* objectArrayData)
    {
        if (!object || !objectArrayData || objectIndex < 0)
            return false;

        const uintptr_t objectArray = reinterpret_cast<uintptr_t>(objectArrayData);
        const uintptr_t objectPointer = reinterpret_cast<uintptr_t>(object);
        if (objectArray > UINTPTR_MAX - 0x30 || objectPointer > UINTPTR_MAX - 0x10)
            return false;

        int32_t maxElements = 0;
        int32_t numElements = 0;
        int32_t maxChunks = 0;
        int32_t numChunks = 0;
        std::memcpy(&maxElements, reinterpret_cast<const void*>(objectArray + 0x20), sizeof(maxElements));
        std::memcpy(&numElements, reinterpret_cast<const void*>(objectArray + 0x24), sizeof(numElements));
        std::memcpy(&maxChunks, reinterpret_cast<const void*>(objectArray + 0x28), sizeof(maxChunks));
        std::memcpy(&numChunks, reinterpret_cast<const void*>(objectArray + 0x2C), sizeof(numChunks));
        if (numElements < 0 || maxElements < numElements || maxElements > 64 * 1024 * 1024 ||
            objectIndex >= numElements || numChunks < 0 || maxChunks < numChunks || maxChunks > 4096 ||
            int64_t(numElements) > int64_t(numChunks) * 65536 ||
            int64_t(maxElements) > int64_t(maxChunks) * 65536)
            return false;

        const size_t chunkIndex = uint32_t(objectIndex) >> 16;
        if (chunkIndex >= size_t(numChunks))
            return false;

        uintptr_t chunkTable = 0;
        std::memcpy(&chunkTable, reinterpret_cast<const void*>(objectArray + 0x10), sizeof(chunkTable));
        if (!chunkTable || chunkTable > UINTPTR_MAX - ((chunkIndex + 1) * sizeof(uintptr_t)))
            return false;

        uintptr_t chunk = 0;
        std::memcpy(&chunk, reinterpret_cast<const void*>(chunkTable + chunkIndex * sizeof(uintptr_t)),
            sizeof(chunk));
        const size_t itemOffset = size_t(uint32_t(objectIndex) & 65535) * 0x18;
        if (!chunk || chunk > UINTPTR_MAX - itemOffset - 0x10)
            return false;

        uintptr_t registeredObject = 0;
        uint32_t internalFlags = 0;
        std::memcpy(&registeredObject, reinterpret_cast<const void*>(chunk + itemOffset), sizeof(registeredObject));
        std::memcpy(&internalFlags, reinterpret_cast<const void*>(chunk + itemOffset + 8), sizeof(internalFlags));
        if (registeredObject != objectPointer)
            return false;

        uint32_t objectFlags = 0;
        std::memcpy(&objectFlags, reinterpret_cast<const void*>(objectPointer + 8), sizeof(objectFlags));
        return !(internalFlags & MiniMapDefinitionAcquisition::UnusableObjectItemFlags) &&
            !(objectFlags & 0x1b400);
    }

    void PlayerFieldAccessors::SetOffsets(const PlayerFieldOffsets& offsets)
    {
        g_playerMainCharacterOffset = offsets.PlayerMainCharacter;
        g_characterDeadOffset = offsets.CharacterDead;
        g_buildingDroneOffset = offsets.BuildingDrone;
        g_droneOwnerPlayerOffset = offsets.DroneOwnerPlayer;
        g_isLocalPlayerControllerOffset = offsets.IsLocalPlayerController;
        g_controllerPawnOffset = offsets.ControllerPawn;
        g_pawnControllerOffset = offsets.PawnController;
        g_characterStatusOffset = offsets.CharacterStatus;
        g_playerMapDataComponentOffset = offsets.PlayerMapDataComponent;
        g_objectIndexOffset = offsets.ObjectIndex;
        g_objectOuterOffset = offsets.ObjectOuter;
        g_actorRootComponentOffset = offsets.ActorRootComponent;
        g_actorBeingDestroyedOffset = offsets.ActorBeingDestroyed;
        g_actorBeingDestroyedMask = offsets.ActorBeingDestroyedMask;
        g_levelOwningWorldOffset = offsets.LevelOwningWorld;
    }

    bool TryGetPresentationSample(PresentationSample& outSample)
    {
        const auto settings = MiniMapConfig::GetTrackingSettings();
        outSample = {};
        LocalPlayer player;
        PresentationSample sample{};
        const auto* native = MiniMapNative::Get();
        if (!ResolveLocalPlayer(player) || !native->player.isPlayerInForgottenEngine ||
            !ReadControlPose(player, sample.Character)) return false;
        sample.WorldGeneration = GetWorldGeneration();
        sample.TrackingRevision = settings.Revision;
        sample.InForgottenEngine = native->player.isPlayerInForgottenEngine(player.Character);
        sample.Tracking = sample.Character;
        const int32_t* trackingObjectIndex = MiniMap::MapRuntimeAccess::Field<int32_t>(
            player.Character, g_objectIndexOffset);
        if (!trackingObjectIndex) return false;
        sample.TrackingObjectIndex = *trackingObjectIndex;
        const SDK::EPlayerCharacterStatus* characterStatus =
            MiniMap::MapRuntimeAccess::Field<SDK::EPlayerCharacterStatus>(
                player.Character, g_characterStatusOffset);
        if (!characterStatus) return false;
        if (!settings.FollowBuildDrone && (player.Pawn != player.Character ||
            *characterStatus == SDK::EPlayerCharacterStatus::BuildingDrone))
        {
            // Drone control rotation belongs to its view. Keep physical tracking
            // on the character's world heading, even during drone retirement.
            if (!ReadWorldYaw(player.Character, sample.Character.ControlYawDegrees)) return false;
            sample.Tracking = sample.Character;
        }
        // Only a complete current association can select the drone. Missing or
        // retiring drone state uses the valid physical character, never history.
        if (!BuildingDroneTracking::TryApply(player, settings.FollowBuildDrone, sample)) return false;
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

    bool TryGetPlayerMapDataComponent(
        SDK::ACrCharacterPlayerBase*& outPlayer,
        SDK::UCrPlayerMapMenuDataComponent*& outComponent)
    {
        outPlayer = nullptr;
        outComponent = nullptr;

        LocalPlayer player;
        if (!ResolveLocalPlayer(player))
            return false;

        SDK::UCrPlayerMapMenuDataComponent* const* componentField =
            MiniMap::MapRuntimeAccess::Field<SDK::UCrPlayerMapMenuDataComponent*>(
                player.Character, g_playerMapDataComponentOffset);
        if (!componentField)
            return false;

        auto* component = *componentField;
        if (!IsObject<SDK::UCrPlayerMapMenuDataComponent>(component))
            return false;

        SDK::UObject* const* owner = MiniMap::MapRuntimeAccess::Field<SDK::UObject*>(
            component, g_objectOuterOffset);
        if (!owner || *owner != player.Character)
            return false;

        outPlayer = player.Character;
        outComponent = component;
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
