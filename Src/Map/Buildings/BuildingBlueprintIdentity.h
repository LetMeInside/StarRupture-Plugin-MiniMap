#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include <cstdint>
#include <memory>
#include <string>
namespace SDK { class UAuActorPlacementData; class UClass; }
namespace MiniMapBuildingBlueprintIdentity
{
    struct DefinitionIdentity
    {
        int32_t ObjectIndex, NameIndex;
        uint32_t NameNumber, BuildingId;
        int32_t ObjectSerial;
    };
    enum class Status { Resolved, Unavailable, Invalid, Interrupted };
    // Diagnostic categories only; neither eligibility nor retry policy uses these.
    enum class Issue : uint8_t
    {
        ResidentPrerequisites, ConversionDisabled, InvocationPrerequisites, DefinitionIdentity,
        RetainDefinition, SoftClassRead, SoftPath, ConversionMetadata, FunctionLookup,
        FunctionMetadata, LibraryDefaultObject, Invocation, ReturnedAllocation,
        ReturnedLength, ReturnedCharacter, CanonicalPath, DefinitionChanged, Allocation, NativeCleanup
    };
    static_assert(unsigned(Issue::NativeCleanup) < 62); // Collector reserves bits 62/63 for lifetime/provenance.
    struct Result
    {
        std::shared_ptr<const std::string> Path;
        Status State = Status::Unavailable;
        Issue Failure = Issue::ResidentPrerequisites;
        const char* Detail = "resident metadata unavailable";
    };
    // Borrowing lasts only for this synchronous call. No definition or class
    // pointer is cached; success returns independently owned UTF-8 storage.
    Result Resolve(const SDK::UAuActorPlacementData* definition, const SDK::UClass* placementClass, DefinitionIdentity identity);
}
#endif
