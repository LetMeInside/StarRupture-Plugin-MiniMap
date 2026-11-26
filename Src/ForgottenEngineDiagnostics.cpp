// Captures explicitly requested local-player facts for Forgotten Engine map calibration.
#include "DiagnosticExports.h"

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS && defined(MODLOADER_CLIENT_BUILD)

#include "ForgottenEngineDiagnostics.h"

#include "Adapters/AlienX/Compatibility.h"
#include "Adapters/AlienX/Input.h"
#include "Map/Map.h"
#include "Services/Input.h"
#include "SDK/Chimera_classes.hpp"
#include "SDK/Chimera_parameters.hpp"
#include "SDK/Engine_classes.hpp"
#include "SDK/GameplayAbilities_classes.hpp"
#include "SDK/GameplayAbilities_parameters.hpp"
#include "SDK/GameplayTags_structs.hpp"
#include "plugin_helpers.h"

#include <atomic>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <string>

namespace
{
    MiniMap::Services::Registration g_sampleKey{};
    std::atomic<bool> g_samplePending{false};
    std::uint64_t g_sampleSequence = 0;
    constexpr const char* kSampleKey = "F9";
    constexpr const wchar_t* kForgottenEngineTag = L"Status.InForgottenEngine";
}

bool MiniMapForgottenEngineDiagnostics::RegisterDiagnostics(IPluginSelf* self)
{
    if (g_sampleKey.id)
    {
        LOG_ERROR("MiniMap: Forgotten Engine diagnostic registration is already active or unresolved");
        return false;
    }

    if (!self || !self->hooks || !MiniMap::Adapters::AlienX::KeyInputAvailable())
    {
        LOG_ERROR("MiniMap: cannot register Forgotten Engine sample key; input hooks are unavailable");
        return false;
    }

    if (!RegisterSampleKey())
    {
        return false;
    }

    g_sampleSequence = 0;
    g_samplePending.store(false, std::memory_order_release);
    LOG_INFO("MiniMap: registered Forgotten Engine one-shot sample key: %s", kSampleKey);
    return true;
}

bool MiniMapForgottenEngineDiagnostics::RegisterSampleKey()
{
    const auto receipt = MiniMap::Adapters::AlienX::GetInput().BindForgottenEngineDiagnostic(
        kSampleKey, &MiniMapForgottenEngineDiagnostics::OnSampleKeyPressed, nullptr);
    g_sampleKey = receipt.token;
    const bool rejected = receipt.acceptance == MiniMap::Services::Acceptance::Rejected ||
        receipt.acceptance == MiniMap::Services::Acceptance::BackendUnknown;
    if (!rejected)
    {
        return true;
    }

    if (g_sampleKey.id)
    {
        const auto removal = MiniMap::Adapters::AlienX::GetInput().Remove(g_sampleKey);
        if (removal.registry == MiniMap::Services::RegistryRemoval::RequestReturned &&
            removal.core == MiniMap::Services::CoreDrain::Quiescent)
        {
            g_sampleKey = {};
        }
    }
    LOG_ERROR("MiniMap: failed to register Forgotten Engine sample key");
    return false;
}

void MiniMapForgottenEngineDiagnostics::UnregisterDiagnostics()
{
    g_samplePending.store(false, std::memory_order_release);
    if (g_sampleKey.id)
    {
        const auto removal = MiniMap::Adapters::AlienX::GetInput().Remove(g_sampleKey);
        if (removal.registry == MiniMap::Services::RegistryRemoval::RequestReturned &&
            removal.core == MiniMap::Services::CoreDrain::Quiescent)
        {
            g_sampleKey = {};
        }
    }

}

void MiniMapForgottenEngineDiagnostics::OnSampleKeyPressed(void*)
{
    g_samplePending.store(true, std::memory_order_release);
}

void MiniMapForgottenEngineDiagnostics::ConsumePendingSample()
{
    if (g_samplePending.exchange(false, std::memory_order_acq_rel))
    {
        RecordSample();
    }
}

void MiniMapForgottenEngineDiagnostics::RecordSample()
{
    Sample sample{};
    sample.Sequence = ++g_sampleSequence;
    SDK::ACrCharacterPlayerBase* const character = CaptureCoordinates(sample);
    CaptureOccupancy(character, sample);
    CaptureMovement(character, sample);
    LogSample(sample);
}

SDK::ACrCharacterPlayerBase* MiniMapForgottenEngineDiagnostics::CaptureCoordinates(Sample& sample)
{
    SDK::UWorld* const world = MiniMapMap::GetWorld();
    sample.HasWorld = world != nullptr;
    sample.WorldIdentity = reinterpret_cast<std::uintptr_t>(world);
    if (world)
    {
        sample.WorldName = world->GetName();
    }

    SDK::ACrCharacterPlayerBase* character = nullptr;
    if (!MiniMapMap::TryGetPhysicalCharacter(character))
    {
        return nullptr;
    }

    SDK::FVector position{};
    if (MiniMapMap::TryGetPlayerWorldPosition(position))
    {
        sample.HasCoordinates = true;
        sample.X = position.X;
        sample.Y = position.Y;
        sample.Z = position.Z;
    }
    return character;
}

void MiniMapForgottenEngineDiagnostics::CaptureOccupancy(
    SDK::ACrCharacterPlayerBase* character, Sample& sample)
{
    if (!character)
    {
        return;
    }

    SDK::UAbilitySystemComponent* abilitySystem = nullptr;
    if (!TryGetAbilitySystemComponent(character, abilitySystem) ||
        !MiniMapMap::PlayerFieldAccessors::IsObjectRegistered(abilitySystem) ||
        !abilitySystem->IsA(SDK::UAbilitySystemComponent::StaticClass()))
    {
        return;
    }

    bool isInside = false;
    if (TryQueryForgottenEngineTag(abilitySystem, isInside))
    {
        sample.Occupancy = isInside ? "inside" : "outside";
    }
}

bool MiniMapForgottenEngineDiagnostics::InvokeReflectedFunction(
    SDK::UObject* object, const char* ownerClass, const char* functionName, void* parameters)
{
    if (!MiniMapMap::PlayerFieldAccessors::IsObjectRegistered(object) ||
        !object->Class || !MiniMapMap::PlayerFieldAccessors::IsObjectRegistered(object->Class) ||
        !ownerClass || !functionName || !parameters)
    {
        return false;
    }

    auto* function = object->Class->GetFunction(ownerClass, functionName);
    if (!MiniMapMap::PlayerFieldAccessors::IsObjectRegistered(function))
    {
        return false;
    }

    object->ProcessEvent(function, parameters);
    return true;
}

bool MiniMapForgottenEngineDiagnostics::TryGetAbilitySystemComponent(
    SDK::ACrCharacterPlayerBase* character, SDK::UAbilitySystemComponent*& abilitySystem)
{
    abilitySystem = nullptr;
    SDK::Params::CrCharacterPlayerBase_GetAbilitySystemComponent parameters{};
    if (!InvokeReflectedFunction(character, "CrCharacterPlayerBase", "GetAbilitySystemComponent", &parameters))
    {
        return false;
    }

    abilitySystem = parameters.ReturnValue;
    return abilitySystem != nullptr;
}

bool MiniMapForgottenEngineDiagnostics::TryQueryForgottenEngineTag(
    SDK::UAbilitySystemComponent* abilitySystem, bool& isInside)
{
    isInside = false;
    SDK::FGameplayTag statusTag{};
    statusTag.TagName = SDK::BasicFilesImplUtils::StringToName(kForgottenEngineTag);
    if (statusTag.TagName.IsNone())
    {
        return false;
    }

    SDK::Params::GameplayTagAssetInterface_HasMatchingGameplayTag parameters{};
    parameters.TagToCheck = statusTag;
    if (!InvokeReflectedFunction(
        abilitySystem, "GameplayTagAssetInterface", "HasMatchingGameplayTag", &parameters))
    {
        return false;
    }

    isInside = parameters.ReturnValue;
    return true;
}

void MiniMapForgottenEngineDiagnostics::CaptureMovement(
    SDK::ACrCharacterPlayerBase* character, Sample& sample)
{
    if (!character)
    {
        return;
    }

    SDK::UCrCharacterMovementComponent* movement = nullptr;
    if (!TryGetMovementComponent(character, movement) ||
        !MiniMapMap::PlayerFieldAccessors::IsObjectRegistered(movement) ||
        !movement->IsA(SDK::UCrCharacterMovementComponent::StaticClass()))
    {
        return;
    }

    sample.HasMovement = true;
    sample.Grounded = movement->IsMovingOnGround();
    sample.WalkableFloor = movement->CurrentFloor.bWalkableFloor;
    sample.MovementMode = static_cast<int>(movement->MovementMode);
    auto* movementBase = movement->GetMovementBase();
    if (!MiniMapMap::PlayerFieldAccessors::IsObjectRegistered(movementBase) ||
        !movementBase->IsA(SDK::UPrimitiveComponent::StaticClass()))
    {
        return;
    }

    sample.HasMovementBase = true;
    sample.MovementBaseIdentity = reinterpret_cast<std::uintptr_t>(movementBase);
    sample.MovementBaseName = movementBase->GetName();
}

bool MiniMapForgottenEngineDiagnostics::TryGetMovementComponent(
    SDK::ACrCharacterPlayerBase* character, SDK::UCrCharacterMovementComponent*& movement)
{
    movement = nullptr;
    SDK::Params::CrCharacterPlayerBase_GetCrMovementComponent parameters{};
    if (!InvokeReflectedFunction(character, "CrCharacterPlayerBase", "GetCrMovementComponent", &parameters))
    {
        return false;
    }

    movement = parameters.ReturnValue;
    return movement != nullptr;
}

void MiniMapForgottenEngineDiagnostics::LogSample(const Sample& sample)
{
    std::ostringstream record;
    record << std::fixed << std::setprecision(3);
    record << "{\"type\":\"forgotten_engine_coordinate_sample\",\"seq\":" << sample.Sequence;
    record << ",\"world\":{";
    if (sample.HasWorld)
    {
        record << "\"name\":\"" << EscapeJsonString(sample.WorldName) << "\"";
        record << ",\"identity\":\"0x" << std::hex << sample.WorldIdentity << std::dec << "\"";
    }
    else
    {
        record << "\"name\":null,\"identity\":null";
    }
    record << "},\"coordinates_cm\":";
    if (sample.HasCoordinates)
    {
        record << "{\"x\":" << sample.X << ",\"y\":" << sample.Y << ",\"z\":" << sample.Z << "}";
    }
    else
    {
        record << "null";
    }
    record << ",\"status_tag\":\"Status.InForgottenEngine\",\"fe_status\":\"";
    record << sample.Occupancy << "\"";
    record << ",\"grounded\":" << (sample.HasMovement ? (sample.Grounded ? "true" : "false") : "null");
    record << ",\"walkable_floor\":" << (sample.HasMovement ? (sample.WalkableFloor ? "true" : "false") : "null");
    record << ",\"movement_mode\":" << (sample.HasMovement ? std::to_string(sample.MovementMode) : "null");
    record << ",\"movement_base\":";
    if (sample.HasMovementBase)
    {
        record << "{\"name\":\"" << EscapeJsonString(sample.MovementBaseName) << "\",\"identity\":\"0x";
        record << std::hex << sample.MovementBaseIdentity << std::dec << "\"}";
    }
    else
    {
        record << "null";
    }
    record << ",\"floor_id\":null,\"data_layers\":null}";
    LOG_INFO("%s", record.str().c_str());
}

std::string MiniMapForgottenEngineDiagnostics::EscapeJsonString(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (const unsigned char character : value)
    {
        if (character == '"' || character == '\\')
        {
            escaped.push_back('\\');
            escaped.push_back(static_cast<char>(character));
        }
        else if (character < 0x20)
        {
            char encoded[7]{};
            std::snprintf(encoded, sizeof(encoded), "\\u%04x", character);
            escaped.append(encoded);
        }
        else
        {
            escaped.push_back(static_cast<char>(character));
        }
    }
    return escaped;
}

#endif
