// Defines the developer-only one-shot Forgotten Engine coordinate sampler.
#pragma once

#include "DiagnosticExports.h"
#include <cstdint>
#include <string>

namespace SDK
{
    class ACrCharacterPlayerBase;
    class UAbilitySystemComponent;
    class UObject;
    class UCrCharacterMovementComponent;
}

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS && defined(MODLOADER_CLIENT_BUILD)

struct IPluginSelf;

/** Owns a manual coordinate-sample key and emits records only on explicit requests. */
class MiniMapForgottenEngineDiagnostics final
{
public:
    /**
     * Registers F9 for one-shot requests, using the already active diagnostic tick.
     * @param self The current plugin identity used to confirm host input readiness.
     * @return True when the dedicated F9 binding is accepted.
     */
    static bool RegisterDiagnostics(IPluginSelf* self);

    /** Removes the F9 binding and clears any pending sample request. */
    static void UnregisterDiagnostics();

    /** Consumes one pending F9 request from the existing developer diagnostic tick. */
    static void ConsumePendingSample();

private:
    /** Owns only copied values for one log record; it never retains UObjects. */
    struct Sample
    {
        std::uint64_t Sequence = 0;
        bool HasWorld = false;
        std::uintptr_t WorldIdentity = 0;
        std::string WorldName;
        bool HasCoordinates = false;
        double X = 0.0;
        double Y = 0.0;
        double Z = 0.0;
        std::string Occupancy = "unknown";
        bool HasMovement = false;
        bool Grounded = false;
        bool WalkableFloor = false;
        int MovementMode = 0;
        bool HasMovementBase = false;
        std::uintptr_t MovementBaseIdentity = 0;
        std::string MovementBaseName;
    };

    /** Latches the key callback's request for consumption on the next game-thread tick. */
    static void OnSampleKeyPressed(void* user);

    /** Registers F9 in its isolated developer-diagnostic input slot. */
    static bool RegisterSampleKey();

    /** Reads current local-player facts and logs one numbered JSON record. */
    static void RecordSample();

    /** Copies coordinates into sample and returns the current borrowed main character, if valid. */
    static SDK::ACrCharacterPlayerBase* CaptureCoordinates(Sample& sample);

    /** Queries character's ASC and stores inside/outside only when that ASC is valid. */
    static void CaptureOccupancy(SDK::ACrCharacterPlayerBase* character, Sample& sample);

    /** Resolves a reflected function on object and invokes it with the supplied parameter buffer. */
    static bool InvokeReflectedFunction(
        SDK::UObject* object, const char* ownerClass, const char* functionName, void* parameters);

    /** Reads the local main character's ASC through its reflected SDK function. */
    static bool TryGetAbilitySystemComponent(
        SDK::ACrCharacterPlayerBase* character, SDK::UAbilitySystemComponent*& abilitySystem);

    /** Reads the owned FE tag through ASC's reflected gameplay-tag interface. */
    static bool TryQueryForgottenEngineTag(SDK::UAbilitySystemComponent* abilitySystem, bool& isInside);

    /** Copies grounded, walkable-floor and movement-base evidence when movement is valid. */
    static void CaptureMovement(SDK::ACrCharacterPlayerBase* character, Sample& sample);

    /** Reads the local main character's movement component through its reflected SDK function. */
    static bool TryGetMovementComponent(
        SDK::ACrCharacterPlayerBase* character, SDK::UCrCharacterMovementComponent*& movement);

    /** Formats copied sample values as one JSON object and sends it through MiniMap logging. */
    static void LogSample(const Sample& sample);

    /** Escapes value's UTF-8 bytes for safe insertion into a JSON string. */
    static std::string EscapeJsonString(const std::string& value);
};

#endif
