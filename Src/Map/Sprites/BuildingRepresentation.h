#pragma once
#include <cstdint>
#include <cstddef>
#include <array>
#include <memory>
#include <string>
#include <string_view>
#include "Native/DefinitionAcquisitionCore.h"
#if defined(MODLOADER_CLIENT_BUILD) && defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
namespace MiniMapSpriteValidation { struct Result; }
namespace MiniMapBuildingCollector { struct Record; }
namespace MiniMapSpriteClassification { struct Observation; }
namespace SDK { class UObject; }
namespace MiniMapBuildingRepresentation
{
    // Stage A only: ordinary loader post-engine game tick, never GC callbacks.
    // No scheduling/admission authority. Borrow ends with visitor;
    // never store the pointer or invoke loading/GC/yield operations in visitor.
    MiniMapDefinitionAcquisition::Status VisitResidentDefinition(
        const MiniMapDefinitionAcquisition::Token&,void(*visitor)(const SDK::UObject*,void*),void*,
        MiniMapDefinitionAcquisition::ScopeObservation* observation=nullptr);
    struct BlueprintIdentityResult
    {
        std::shared_ptr<const std::string> Path;
        const char* Reason="identity_not_resolved";
        explicit operator bool()const noexcept{return bool(Path);}
    };
    // Exact engine-produced generated-class paths only. No entries initially.
    inline constexpr std::array<std::string_view,0> kBlacklistedBlueprints{};
    inline bool IsBlueprintBlacklisted(std::string_view path)
    {for(const auto entry:kBlacklistedBlueprints)if(entry==path)return true;return false;}
    // Copied generation-owned metadata only; safe for readers, no native call.
    BlueprintIdentityResult GetBlueprintIdentity(const MiniMapBuildingCollector::Record&);
    bool ResolvePrerequisites();
    void ServiceIdentityMaintenance(); // bounded CPU-only retired identity disposal, including inactive gameplay
    bool PrepareNativeClasses(); // one retained resident native class per service tick
    void Initialize();
    void Shutdown();
    bool ValidateCaptureObject(uintptr_t);
    // Game-thread resident /Script metadata only. Success transfers one strong
    // reference to the caller; release after destroying queries using the type.
    uintptr_t AcquireResidentMetadata(const wchar_t* path,bool scriptStruct,size_t instanceSize);
    void ReleaseResidentMetadata(uintptr_t);
    // Bounded fresh Mass/definition identity and classification read. No graph
    // traversal or borrowed native pointer escapes this synchronous call.
    bool RevalidateCaptureSource(const MiniMapBuildingCollector::Record&,MiniMapSpriteClassification::Observation&);
    bool ValidateCaptureMembership(uintptr_t capture,const uintptr_t* components,size_t count);
    bool ValidateCaptureMaterialSlots(uintptr_t component,const uintptr_t* materials,size_t count,size_t& failedSlot);
#if defined(MINIMAP_STAGE_R26B_SPRITE_VALIDATION)
    // Synchronous game-thread audit. Returned data owns no native references.
    MiniMapSpriteValidation::Result ValidateRepresentation(const MiniMapBuildingCollector::Record&,const MiniMapBuildingCollector::Record*);
    MiniMapSpriteValidation::Result ValidateAndCapture(const MiniMapBuildingCollector::Record&,const MiniMapBuildingCollector::Record*,bool deferCapture=false);
#endif
}
#endif
