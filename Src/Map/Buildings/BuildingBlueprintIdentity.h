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
    struct Result
    {
        std::shared_ptr<const std::string> Path;
        Status State = Status::Unavailable;
    };
    // Borrowing lasts only for this synchronous call. No definition or class
    // pointer is cached; success returns independently owned UTF-8 storage.
    Result Resolve(const SDK::UAuActorPlacementData* definition, const SDK::UClass* placementClass, DefinitionIdentity identity);
}
#endif
