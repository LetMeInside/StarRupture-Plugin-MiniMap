#include "Bootstrap.h"
#include "Log.h"
#include "Configuration.h"
#include "Input.h"
#include "Objects.h"
#include "RequiredScanner.h"
#include "RuntimeEvents.h"
#include "NativeHooks.h"
#include "Viewport.h"
#include "Textures.h"
#include "Services/RuntimeBinding.h"
#include "plugin.h"
#include "plugin_helpers.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>

#if defined(MODLOADER_CLIENT_BUILD)
#include "SDK.hpp"
#include "SDK/AuActorPlacement_structs.hpp"
#include "SDK/AuItems_classes.hpp"
#include "SDK/AuItems_structs.hpp"
#include "SDK/Chimera_classes.hpp"
#include "SDK/Chimera_structs.hpp"
#include "SDK/ChimeraUI_classes.hpp"
#include "SDK/ChimeraUI_structs.hpp"
#include "SDK/Engine_classes.hpp"
#include "SDK/Engine_parameters.hpp"
#include "SDK/Engine_structs.hpp"
#include "SDK/MassRepresentation_classes.hpp"
#include "SDK/MassSpawner_classes.hpp"
#include "SDK/MassSpawner_structs.hpp"
#include "SDK/MassEntity_structs.hpp"
#include "plugin_map.h"
#include "Native/NativeApi.h"
#endif

#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "1.2.0"
#endif

namespace
{
    // One adapter-owned identity and one immutable Runtime for this DLL instance.
    IPluginSelf* g_self = nullptr;
    MiniMap::Adapters::AlienX::Log g_log;
    MiniMap::Adapters::AlienX::Configuration g_configuration;
    MiniMap::Adapters::AlienX::Input g_input;
#if defined(MODLOADER_CLIENT_BUILD)
    MiniMap::Adapters::AlienX::RuntimeEvents g_events;
    MiniMap::Adapters::AlienX::NativeHooks g_hooks;
    MiniMap::Adapters::AlienX::Viewport g_viewport;
    MiniMap::Adapters::AlienX::Textures g_textures;
    const MiniMap::Services::Runtime g_runtime{g_log, &g_configuration, nullptr, &g_input,
        &MiniMap::Adapters::AlienX::GetObjects(), &g_events, &g_hooks, &g_viewport, &g_textures};
#else
    const MiniMap::Services::Runtime g_runtime{g_log, &g_configuration};
#endif
    std::optional<MiniMap::Services::Detail::RuntimeBinding> g_binding;

#if defined(MODLOADER_CLIENT_BUILD)
    // Bridges the scanner-session field checks to PluginInit without retaining scanner metadata.
    bool g_buildingMassFieldsResolved = false;
    bool g_fowDataFieldResolved = false;
    bool g_playerMapDataComponentFieldResolved = false;
    bool g_terrainFieldsResolved = false;
    bool g_poiStoryFieldsResolved = false;
    bool g_playerTextureFieldsResolved = false;

    PluginFieldSpec g_fowDataField{
        "UCrPlayerMapMenuDataComponent::FOWData",
        "UCrPlayerMapMenuDataComponent",
        "FOWData",
        PLUGIN_FIELD_NATIVE | PLUGIN_FIELD_CHECK,
        0x10,
        0x100
    };

    constexpr PluginFieldCandidate kFowDataCandidates[] = {
        { 127004, 0x100, 0x10, 0, nullptr }
    };

    PluginFieldSpec g_playerMapDataComponentField{
        "ACrCharacterPlayerBase::PlayerMapMenuDataComponent",
        "ACrCharacterPlayerBase",
        "PlayerMapMenuDataComponent",
        PLUGIN_FIELD_CHECK,
        sizeof(decltype(SDK::ACrCharacterPlayerBase::PlayerMapMenuDataComponent)),
        static_cast<int32_t>(offsetof(
            SDK::ACrCharacterPlayerBase,
            PlayerMapMenuDataComponent))
    };

    /** Requires exact reflected layouts for fields read by TerrainSource. */
    class ClientTerrainFieldContract
    {
    public:
        /** Resolves consumed Terrain fields through the active loader scanner session. */
        static bool Resolve(IPluginHookScanner* scanner, const IPluginSelf* self)
        {
            static PluginFieldSpec fields[] = {
                { "UCrMapMenuDevSettings::TerrainData", "UCrMapMenuDevSettings", "TerrainData",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::UCrMapMenuDevSettings::TerrainData)),
                    static_cast<int32_t>(offsetof(SDK::UCrMapMenuDevSettings, TerrainData)) },
                { "UCrMapMenuTerrainData::MapTerrainTopLeftPivotPoint", "UCrMapMenuTerrainData",
                    "MapTerrainTopLeftPivotPoint", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::UCrMapMenuTerrainData::MapTerrainTopLeftPivotPoint)),
                    static_cast<int32_t>(offsetof(SDK::UCrMapMenuTerrainData, MapTerrainTopLeftPivotPoint)) },
                { "UCrMapMenuTerrainData::MapTerrainSegmentSize", "UCrMapMenuTerrainData",
                    "MapTerrainSegmentSize", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::UCrMapMenuTerrainData::MapTerrainSegmentSize)),
                    static_cast<int32_t>(offsetof(SDK::UCrMapMenuTerrainData, MapTerrainSegmentSize)) },
                { "UCrMapMenuTerrainData::TerrainSegmentsData", "UCrMapMenuTerrainData",
                    "TerrainSegmentsData", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::UCrMapMenuTerrainData::TerrainSegmentsData)),
                    static_cast<int32_t>(offsetof(SDK::UCrMapMenuTerrainData, TerrainSegmentsData)) },
                { "FVector3f::X", "FVector3f", "X", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FVector3f::X)),
                    static_cast<int32_t>(offsetof(SDK::FVector3f, X)) },
                { "FVector3f::Y", "FVector3f", "Y", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FVector3f::Y)),
                    static_cast<int32_t>(offsetof(SDK::FVector3f, Y)) },
                { "FVector3f::Z", "FVector3f", "Z", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FVector3f::Z)),
                    static_cast<int32_t>(offsetof(SDK::FVector3f, Z)) },
                { "FIntPoint::X", "FIntPoint", "X", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FIntPoint::X)),
                    static_cast<int32_t>(offsetof(SDK::FIntPoint, X)) },
                { "FIntPoint::Y", "FIntPoint", "Y", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FIntPoint::Y)),
                    static_cast<int32_t>(offsetof(SDK::FIntPoint, Y)) },
                { "FCrTerrainSegmentData::TerrainSegmentGridIndex", "FCrTerrainSegmentData",
                    "TerrainSegmentGridIndex", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FCrTerrainSegmentData::TerrainSegmentGridIndex)),
                    static_cast<int32_t>(offsetof(SDK::FCrTerrainSegmentData, TerrainSegmentGridIndex)) },
                { "FCrTerrainSegmentData::TerrainSegmentTexture", "FCrTerrainSegmentData",
                    "TerrainSegmentTexture", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FCrTerrainSegmentData::TerrainSegmentTexture)),
                    static_cast<int32_t>(offsetof(SDK::FCrTerrainSegmentData, TerrainSegmentTexture)) },
                { "FCrTerrainSegmentData::TerrainSegmentTextureRadiation2", "FCrTerrainSegmentData",
                    "TerrainSegmentTextureRadiation2", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FCrTerrainSegmentData::TerrainSegmentTextureRadiation2)),
                    static_cast<int32_t>(offsetof(SDK::FCrTerrainSegmentData, TerrainSegmentTextureRadiation2)) },
                { "UWorld::GameState", "UWorld", "GameState", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::UWorld::GameState)),
                    static_cast<int32_t>(offsetof(SDK::UWorld, GameState)) },
                { "ACrGameStateBase::MapMenuDataReplicationHelper", "ACrGameStateBase",
                    "MapMenuDataReplicationHelper", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::ACrGameStateBase::MapMenuDataReplicationHelper)),
                    static_cast<int32_t>(offsetof(SDK::ACrGameStateBase, MapMenuDataReplicationHelper)) },
                { "ACrMapMenuDataReplicationHelper::CurrentRadiationLevelReplicated",
                    "ACrMapMenuDataReplicationHelper", "CurrentRadiationLevelReplicated",
                    PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::ACrMapMenuDataReplicationHelper::CurrentRadiationLevelReplicated)),
                    static_cast<int32_t>(offsetof(
                        SDK::ACrMapMenuDataReplicationHelper, CurrentRadiationLevelReplicated)) }
            };

            bool allResolved = true;
            for (PluginFieldSpec& field : fields)
            {
                field.offset = -1;
                field.resolvedSize = 0;
                allResolved = PluginMap::ResolveField(scanner, self, field) && allResolved;
            }
            return allResolved;
        }
    };

    PluginBitField g_actorBeingDestroyedField{
        { "AActor::bActorIsBeingDestroyed", "AActor", "bActorIsBeingDestroyed",
            PLUGIN_FIELD_BITFIELD, 1, -1 }
    };

    PluginBitField g_textureSRGBField{
        { "UTexture::SRGB", "UTexture", "SRGB",
            PLUGIN_FIELD_BITFIELD | PLUGIN_FIELD_CHECK, 1, -1 }
    };

    /** Requires exact reflected layouts for POI, story-record, category, and actor fields. */
    class ClientPoiStoryFieldContract
    {
    public:
        /** Resolves the consumed fields through the active loader scanner session. */
        static bool Resolve(
            IPluginHookScanner* scanner,
            const IPluginSelf* self,
            MiniMap::Adapters::AlienX::RequiredScanner& requiredScanner)
        {
            bool allResolved = true;
            allResolved = ResolveReplicatorFields(scanner, self) && allResolved;
            allResolved = ResolveStoryRecordFields(scanner, self) && allResolved;
            allResolved = ResolvePoiActorFields(scanner, self) && allResolved;
            allResolved = ResolveCategoryFields(scanner, self) && allResolved;
            allResolved = ResolveActorIdentityFields(scanner, self) && allResolved;
            allResolved = ResolveActorBeingDestroyed(scanner, self, requiredScanner) && allResolved;
            return allResolved;
        }

    private:
        /** Resolves a fixed group of fields with exact compiled offset and extent requirements. */
        static bool ResolveFields(
            IPluginHookScanner* scanner,
            const IPluginSelf* self,
            PluginFieldSpec* fields,
            size_t fieldCount)
        {
            bool allResolved = true;
            for (size_t index = 0; index < fieldCount; ++index)
            {
                fields[index].offset = -1;
                fields[index].resolvedSize = 0;
                allResolved = PluginMap::ResolveField(scanner, self, fields[index]) && allResolved;
            }
            return allResolved;
        }

        /** Resolves the game-state path to the abandoned-base replication data. */
        static bool ResolveReplicatorFields(IPluginHookScanner* scanner, const IPluginSelf* self)
        {
            static PluginFieldSpec fields[] = {
                { "ACrGameStateBase::AbandonBaseInfoReplicator", "ACrGameStateBase",
                    "AbandonBaseInfoReplicator", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::ACrGameStateBase::AbandonBaseInfoReplicator)),
                    static_cast<int32_t>(offsetof(SDK::ACrGameStateBase, AbandonBaseInfoReplicator)) },
                { "ACrAbandonBaseInfoReplicator::AbandonBaseDataContainer",
                    "ACrAbandonBaseInfoReplicator", "AbandonBaseDataContainer", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::ACrAbandonBaseInfoReplicator::AbandonBaseDataContainer)),
                    static_cast<int32_t>(offsetof(SDK::ACrAbandonBaseInfoReplicator, AbandonBaseDataContainer)) },
                { "FCrAbandonBaseDataContainer::AbandonBaseData", "FCrAbandonBaseDataContainer",
                    "AbandonBaseData", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FCrAbandonBaseDataContainer::AbandonBaseData)),
                    static_cast<int32_t>(offsetof(SDK::FCrAbandonBaseDataContainer, AbandonBaseData)) }
            };
            return ResolveFields(scanner, self, fields, std::size(fields));
        }

        /** Resolves the consumed members of replicated abandoned-base story records. */
        static bool ResolveStoryRecordFields(IPluginHookScanner* scanner, const IPluginSelf* self)
        {
            static PluginFieldSpec fields[] = {
                { "FCrAbandonBaseData::AbandonBasePOIId", "FCrAbandonBaseData",
                    "AbandonBasePOIId", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FCrAbandonBaseData::AbandonBasePOIId)),
                    static_cast<int32_t>(offsetof(SDK::FCrAbandonBaseData, AbandonBasePOIId)) },
                { "FCrAbandonBaseData::StoryItemsCollectedStatus", "FCrAbandonBaseData",
                    "StoryItemsCollectedStatus", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FCrAbandonBaseData::StoryItemsCollectedStatus)),
                    static_cast<int32_t>(offsetof(SDK::FCrAbandonBaseData, StoryItemsCollectedStatus)) },
                { "FCrAbandonBaseDataCollectedStatus::Item", "FCrAbandonBaseDataCollectedStatus",
                    "Item", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FCrAbandonBaseDataCollectedStatus::Item)),
                    static_cast<int32_t>(offsetof(SDK::FCrAbandonBaseDataCollectedStatus, Item)) },
                { "FCrAbandonBaseDataCollectedStatus::Status", "FCrAbandonBaseDataCollectedStatus",
                    "Status", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FCrAbandonBaseDataCollectedStatus::Status)),
                    static_cast<int32_t>(offsetof(SDK::FCrAbandonBaseDataCollectedStatus, Status)) }
            };
            return ResolveFields(scanner, self, fields, std::size(fields));
        }

        /** Resolves the directly consumed POI marker fields. */
        static bool ResolvePoiActorFields(IPluginHookScanner* scanner, const IPluginSelf* self)
        {
            static PluginFieldSpec fields[] = {
                { "ACrPointOfInterestMarkerActor::PointOfInterestState", "ACrPointOfInterestMarkerActor",
                    "PointOfInterestState", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::ACrPointOfInterestMarkerActor::PointOfInterestState)),
                    static_cast<int32_t>(offsetof(SDK::ACrPointOfInterestMarkerActor, PointOfInterestState)) },
                { "ACrPointOfInterestMarkerActor::PointOfInterestType", "ACrPointOfInterestMarkerActor",
                    "PointOfInterestType", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::ACrPointOfInterestMarkerActor::PointOfInterestType)),
                    static_cast<int32_t>(offsetof(SDK::ACrPointOfInterestMarkerActor, PointOfInterestType)) },
                { "ACrPointOfInterestMarkerActor::Guid", "ACrPointOfInterestMarkerActor", "Guid",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::ACrPointOfInterestMarkerActor::Guid)),
                    static_cast<int32_t>(offsetof(SDK::ACrPointOfInterestMarkerActor, Guid)) },
                { "ACrPointOfInterestMarkerActor::AbandonBaseStoryItemsAssigned",
                    "ACrPointOfInterestMarkerActor", "AbandonBaseStoryItemsAssigned", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::ACrPointOfInterestMarkerActor::AbandonBaseStoryItemsAssigned)),
                    static_cast<int32_t>(offsetof(
                        SDK::ACrPointOfInterestMarkerActor, AbandonBaseStoryItemsAssigned)) }
            };
            return ResolveFields(scanner, self, fields, std::size(fields));
        }

        /** Resolves the consumed category metadata, icon variants, and story actor GUIDs. */
        static bool ResolveCategoryFields(IPluginHookScanner* scanner, const IPluginSelf* self)
        {
            static PluginFieldSpec fields[] = {
                { "ACrDatapadBase::Guid", "ACrDatapadBase", "Guid", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::ACrDatapadBase::Guid)),
                    static_cast<int32_t>(offsetof(SDK::ACrDatapadBase, Guid)) },
                { "ACrAudiologBase::Guid", "ACrAudiologBase", "Guid", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::ACrAudiologBase::Guid)),
                    static_cast<int32_t>(offsetof(SDK::ACrAudiologBase, Guid)) },
                { "UCrMapMenuCategoryData::Icon", "UCrMapMenuCategoryData", "Icon",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::UCrMapMenuCategoryData::Icon)),
                    static_cast<int32_t>(offsetof(SDK::UCrMapMenuCategoryData, Icon)) },
                { "UCrMapMenuCategoryData::CategoryBlocked", "UCrMapMenuCategoryData",
                    "CategoryBlocked", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::UCrMapMenuCategoryData::CategoryBlocked)),
                    static_cast<int32_t>(offsetof(SDK::UCrMapMenuCategoryData, CategoryBlocked)) },
                { "UCrMapMenuCategoryData::LegendFilter", "UCrMapMenuCategoryData", "LegendFilter",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::UCrMapMenuCategoryData::LegendFilter)),
                    static_cast<int32_t>(offsetof(SDK::UCrMapMenuCategoryData, LegendFilter)) },
                { "UCrMapMenuCategoryData::CanBeHiddenByFogOfWar", "UCrMapMenuCategoryData",
                    "CanBeHiddenByFogOfWar", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::UCrMapMenuCategoryData::CanBeHiddenByFogOfWar)),
                    static_cast<int32_t>(offsetof(SDK::UCrMapMenuCategoryData, CanBeHiddenByFogOfWar)) },
                { "UCrMapMenuPOIData::UnknownIcon", "UCrMapMenuPOIData", "UnknownIcon",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::UCrMapMenuPOIData::UnknownIcon)),
                    static_cast<int32_t>(offsetof(SDK::UCrMapMenuPOIData, UnknownIcon)) },
                { "UCrMapMenuPOIData::AbandonBaseCompletedIcon", "UCrMapMenuPOIData",
                    "AbandonBaseCompletedIcon", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::UCrMapMenuPOIData::AbandonBaseCompletedIcon)),
                    static_cast<int32_t>(offsetof(SDK::UCrMapMenuPOIData, AbandonBaseCompletedIcon)) }
            };
            return ResolveFields(scanner, self, fields, std::size(fields));
        }

        /** Resolves actor and UObject identity fields used while validating native actors. */
        static bool ResolveActorIdentityFields(IPluginHookScanner* scanner, const IPluginSelf* self)
        {
            static PluginFieldSpec fields[] = {
                { "AActor::RootComponent", "AActor", "RootComponent", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::AActor::RootComponent)),
                    static_cast<int32_t>(offsetof(SDK::AActor, RootComponent)) },
                { "ULevel::OwningWorld", "ULevel", "OwningWorld", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::ULevel::OwningWorld)),
                    static_cast<int32_t>(offsetof(SDK::ULevel, OwningWorld)) },
                { "UObject::ObjectFlags", "UObject", "ObjectFlags",
                    PLUGIN_FIELD_NATIVE | PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::UObject::Flags)),
                    static_cast<int32_t>(offsetof(SDK::UObject, Flags)) },
                { "UObject::OuterPrivate", "UObject", "OuterPrivate",
                    PLUGIN_FIELD_NATIVE | PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::UObject::Outer)),
                    static_cast<int32_t>(offsetof(SDK::UObject, Outer)) },
                { "UStruct::SuperStruct", "UStruct", "SuperStruct",
                    PLUGIN_FIELD_NATIVE | PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::UStruct::SuperStruct)),
                    static_cast<int32_t>(offsetof(SDK::UStruct, SuperStruct)) }
            };
            return ResolveFields(scanner, self, fields, std::size(fields));
        }

        /** Resolves and validates the reflected actor-destruction bitfield. */
        static bool ResolveActorBeingDestroyed(
            IPluginHookScanner* scanner,
            const IPluginSelf* self,
            MiniMap::Adapters::AlienX::RequiredScanner& requiredScanner)
        {
            g_actorBeingDestroyedField.offset = -1;
            g_actorBeingDestroyedField.resolvedSize = 0;
            g_actorBeingDestroyedField.boolMask = 0;
            if (!PluginMap::ResolveField(scanner, self, g_actorBeingDestroyedField))
            {
                return false;
            }
            if (g_actorBeingDestroyedField.offset != 0x5D ||
                g_actorBeingDestroyedField.resolvedSize != 1 ||
                g_actorBeingDestroyedField.boolMask != 0x02)
            {
                requiredScanner.Fail(
                    "AActor::bActorIsBeingDestroyed",
                    "resolved destruction bit does not match CL-127004 offset 0x5D, extent 1, and mask 0x02");
                return false;
            }
            return true;
        }
    };

    /** Requires exact player-tracking properties and the reflected texture SRGB bitfield. */
    class ClientPlayerTextureFieldContract
    {
    public:
        /** Resolves the consumed tracking fields and verifies the CL-127004 SRGB bit. */
        static bool Resolve(
            IPluginHookScanner* scanner,
            const IPluginSelf* self,
            MiniMap::Adapters::AlienX::RequiredScanner& requiredScanner)
        {
            bool allResolved = ResolvePlayerFields(scanner, self);
            const bool srgbResolved = ResolveTextureSRGB(scanner, self, requiredScanner);
            return allResolved && srgbResolved;
        }

    private:
        /** Resolves every reflected player/controller property consumed by Map.cpp. */
        static bool ResolvePlayerFields(IPluginHookScanner* scanner, const IPluginSelf* self)
        {
            static PluginFieldSpec fields[] = {
                { "ACrPlayerControllerBase::PlayerMainCharacter", "ACrPlayerControllerBase",
                    "PlayerMainCharacter", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::ACrPlayerControllerBase::PlayerMainCharacter)),
                    static_cast<int32_t>(offsetof(SDK::ACrPlayerControllerBase, PlayerMainCharacter)) },
                { "ACrCharacterPlayerBase::bDead", "ACrCharacterPlayerBase", "bDead",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::ACrCharacterPlayerBase::bDead)),
                    static_cast<int32_t>(offsetof(SDK::ACrCharacterPlayerBase, bDead)) },
                { "ACrCharacterPlayerBase::BuildingDrone", "ACrCharacterPlayerBase", "BuildingDrone",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::ACrCharacterPlayerBase::BuildingDrone)),
                    static_cast<int32_t>(offsetof(SDK::ACrCharacterPlayerBase, BuildingDrone)) },
                { "ACrCharacterDroneBase::OwnerPlayer", "ACrCharacterDroneBase", "OwnerPlayer",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::ACrCharacterDroneBase::OwnerPlayer)),
                    static_cast<int32_t>(offsetof(SDK::ACrCharacterDroneBase, OwnerPlayer)) },
                { "APlayerController::bIsLocalPlayerController", "APlayerController",
                    "bIsLocalPlayerController", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::APlayerController::bIsLocalPlayerController)),
                    static_cast<int32_t>(offsetof(SDK::APlayerController, bIsLocalPlayerController)) },
                { "AController::Pawn", "AController", "Pawn", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::AController::Pawn)),
                    static_cast<int32_t>(offsetof(SDK::AController, Pawn)) },
                { "APawn::Controller", "APawn", "Controller", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::APawn::Controller)),
                    static_cast<int32_t>(offsetof(SDK::APawn, Controller)) },
                { "ACrCharacterPlayerBase::Status", "ACrCharacterPlayerBase", "Status",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::ACrCharacterPlayerBase::Status)),
                    static_cast<int32_t>(offsetof(SDK::ACrCharacterPlayerBase, Status)) }
            };

            bool allResolved = true;
            for (size_t index = 0; index < std::size(fields); ++index)
            {
                fields[index].offset = -1;
                fields[index].resolvedSize = 0;
                allResolved = PluginMap::ResolveField(scanner, self, fields[index]) && allResolved;
            }
            return allResolved;
        }

        /** Requires the mapped UTexture bitfield to match the compiled CL-127004 ABI. */
        static bool ResolveTextureSRGB(
            IPluginHookScanner* scanner,
            const IPluginSelf* self,
            MiniMap::Adapters::AlienX::RequiredScanner& requiredScanner)
        {
            g_textureSRGBField.offset = -1;
            g_textureSRGBField.resolvedSize = 0;
            g_textureSRGBField.boolMask = 0;
            const bool srgbResolved = PluginMap::ResolveField(scanner, self, g_textureSRGBField);
            if (srgbResolved && (g_textureSRGBField.offset != 0x106 ||
                g_textureSRGBField.resolvedSize != 1 || g_textureSRGBField.boolMask != 0x01))
            {
                requiredScanner.Fail(
                    "UTexture::SRGB",
                    "resolved SRGB bit does not match CL-127004 offset 0x106, extent 1, and mask 0x01");
                return false;
            }
            return srgbResolved;
        }
    };


    PluginFieldSpec g_buildingDefinitionFields[] = {
                { "UAuActorPlacementData::BuildingID", "UAuActorPlacementData", "BuildingID",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::UAuActorPlacementData::BuildingID)),
                    static_cast<int32_t>(offsetof(SDK::UAuActorPlacementData, BuildingID)) },
                { "UAuActorPlacementData::ActorClass", "UAuActorPlacementData", "ActorClass",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::UAuActorPlacementData::ActorClass)),
                    static_cast<int32_t>(offsetof(SDK::UAuActorPlacementData, ActorClass)) },
                { "UAuActorPlacementData::EntityType", "UAuActorPlacementData", "EntityType",
                    PLUGIN_FIELD_CHECK, sizeof(SDK::FAuAPMassSpawnedEntityType),
                    static_cast<int32_t>(offsetof(SDK::UAuActorPlacementData, EntityType)) },
                { "FAuAPMassSpawnedEntityType::EntityConfig", "FAuAPMassSpawnedEntityType", "EntityConfig",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::FAuAPMassSpawnedEntityType::EntityConfig)),
                    static_cast<int32_t>(offsetof(SDK::FAuAPMassSpawnedEntityType, EntityConfig)) },
                { "FAuAPMassSpawnedEntityType::EntityConfigPtr", "FAuAPMassSpawnedEntityType", "EntityConfigPtr",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::FAuAPMassSpawnedEntityType::EntityConfigPtr)),
                    static_cast<int32_t>(offsetof(SDK::FAuAPMassSpawnedEntityType, EntityConfigPtr)) },
                { "UCrBuildingData::Tags", "UCrBuildingData", "Tags", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::UCrBuildingData::Tags)),
                    static_cast<int32_t>(offsetof(SDK::UCrBuildingData, Tags)) },
                { "UMassEntityConfigAsset::Config", "UMassEntityConfigAsset", "Config",
                    PLUGIN_FIELD_CHECK, sizeof(SDK::FMassEntityConfig),
                    static_cast<int32_t>(offsetof(SDK::UMassEntityConfigAsset, Config)) },
                { "FMassEntityConfig::Parent", "FMassEntityConfig", "Parent", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FMassEntityConfig::Parent)),
                    static_cast<int32_t>(offsetof(SDK::FMassEntityConfig, Parent)) },
                { "FMassEntityConfig::Traits", "FMassEntityConfig", "Traits", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FMassEntityConfig::Traits)),
                    static_cast<int32_t>(offsetof(SDK::FMassEntityConfig, Traits)) },
                { "UCrMassRepVisCosmeticsTrait::HighResTemplateActor", "UCrMassRepVisCosmeticsTrait",
                    "HighResTemplateActor", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::UCrMassRepVisCosmeticsTrait::HighResTemplateActor)),
                    static_cast<int32_t>(offsetof(SDK::UCrMassRepVisCosmeticsTrait, HighResTemplateActor)) },
                { "UCrMassRepVisCosmeticsTrait::LowResTemplateActor", "UCrMassRepVisCosmeticsTrait",
                    "LowResTemplateActor", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::UCrMassRepVisCosmeticsTrait::LowResTemplateActor)),
                    static_cast<int32_t>(offsetof(SDK::UCrMassRepVisCosmeticsTrait, LowResTemplateActor)) },
                { "UMassVisualizationTrait::HighResTemplateActor", "UMassVisualizationTrait",
                    "HighResTemplateActor", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::UMassVisualizationTrait::HighResTemplateActor)),
                    static_cast<int32_t>(offsetof(SDK::UMassVisualizationTrait, HighResTemplateActor)) },
                { "UMassVisualizationTrait::LowResTemplateActor", "UMassVisualizationTrait",
                    "LowResTemplateActor", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::UMassVisualizationTrait::LowResTemplateActor)),
                    static_cast<int32_t>(offsetof(SDK::UMassVisualizationTrait, LowResTemplateActor)) },
                { "UBlueprintGeneratedClass::ComponentTemplates", "UBlueprintGeneratedClass",
                    "ComponentTemplates", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::UBlueprintGeneratedClass::ComponentTemplates)),
                    static_cast<int32_t>(offsetof(SDK::UBlueprintGeneratedClass, ComponentTemplates)) },
                { "ACrModularRampWalkway::SplineMesh", "ACrModularRampWalkway", "SplineMesh",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::ACrModularRampWalkway::SplineMesh)),
                    static_cast<int32_t>(offsetof(SDK::ACrModularRampWalkway, SplineMesh)) },
                { "UStaticMeshComponent::StaticMesh", "UStaticMeshComponent", "StaticMesh",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::UStaticMeshComponent::StaticMesh)),
                    static_cast<int32_t>(offsetof(SDK::UStaticMeshComponent, StaticMesh)) },
                { "UStaticMesh::ExtendedBounds", "UStaticMesh", "ExtendedBounds", PLUGIN_FIELD_CHECK,
                    sizeof(SDK::FBoxSphereBounds), static_cast<int32_t>(offsetof(SDK::UStaticMesh, ExtendedBounds)) },
                { "USplineMeshComponent::SplineParams", "USplineMeshComponent", "SplineParams",
                    PLUGIN_FIELD_CHECK, sizeof(SDK::FSplineMeshParams),
                    static_cast<int32_t>(offsetof(SDK::USplineMeshComponent, SplineParams)) },
                { "USplineMeshComponent::SplineUpDir", "USplineMeshComponent", "SplineUpDir",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::USplineMeshComponent::SplineUpDir)),
                    static_cast<int32_t>(offsetof(SDK::USplineMeshComponent, SplineUpDir)) },
                { "USplineMeshComponent::ForwardAxis", "USplineMeshComponent", "ForwardAxis",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::USplineMeshComponent::ForwardAxis)),
                    static_cast<int32_t>(offsetof(SDK::USplineMeshComponent, ForwardAxis)) },
                { "USceneComponent::RelativeLocation", "USceneComponent", "RelativeLocation",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::USceneComponent::RelativeLocation)),
                    static_cast<int32_t>(offsetof(SDK::USceneComponent, RelativeLocation)) },
                { "USceneComponent::RelativeRotation", "USceneComponent", "RelativeRotation",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::USceneComponent::RelativeRotation)),
                    static_cast<int32_t>(offsetof(SDK::USceneComponent, RelativeRotation)) },
                { "USceneComponent::RelativeScale3D", "USceneComponent", "RelativeScale3D",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::USceneComponent::RelativeScale3D)),
                    static_cast<int32_t>(offsetof(SDK::USceneComponent, RelativeScale3D)) },
                { "FSplineMeshParams::StartScale", "FSplineMeshParams", "StartScale", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FSplineMeshParams::StartScale)),
                    static_cast<int32_t>(offsetof(SDK::FSplineMeshParams, StartScale)) },
                { "FSplineMeshParams::EndScale", "FSplineMeshParams", "EndScale", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FSplineMeshParams::EndScale)),
                    static_cast<int32_t>(offsetof(SDK::FSplineMeshParams, EndScale)) },
                { "FSplineMeshParams::StartOffset", "FSplineMeshParams", "StartOffset", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FSplineMeshParams::StartOffset)),
                    static_cast<int32_t>(offsetof(SDK::FSplineMeshParams, StartOffset)) },
                { "FSplineMeshParams::EndOffset", "FSplineMeshParams", "EndOffset", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FSplineMeshParams::EndOffset)),
                    static_cast<int32_t>(offsetof(SDK::FSplineMeshParams, EndOffset)) },
                { "FSplineMeshParams::StartRoll", "FSplineMeshParams", "StartRoll", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FSplineMeshParams::StartRoll)),
                    static_cast<int32_t>(offsetof(SDK::FSplineMeshParams, StartRoll)) },
                { "FSplineMeshParams::EndRoll", "FSplineMeshParams", "EndRoll", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FSplineMeshParams::EndRoll)),
                    static_cast<int32_t>(offsetof(SDK::FSplineMeshParams, EndRoll)) },
                { "FBoxSphereBounds::Origin", "FBoxSphereBounds", "Origin", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FBoxSphereBounds::Origin)),
                    static_cast<int32_t>(offsetof(SDK::FBoxSphereBounds, Origin)) },
                { "FBoxSphereBounds::BoxExtent", "FBoxSphereBounds", "BoxExtent", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FBoxSphereBounds::BoxExtent)),
                    static_cast<int32_t>(offsetof(SDK::FBoxSphereBounds, BoxExtent)) },
                { "FBoxSphereBounds::SphereRadius", "FBoxSphereBounds", "SphereRadius", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FBoxSphereBounds::SphereRadius)),
                    static_cast<int32_t>(offsetof(SDK::FBoxSphereBounds, SphereRadius)) },
                { "FVector::X", "FVector", "X", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FVector::X)), static_cast<int32_t>(offsetof(SDK::FVector, X)) },
                { "FVector::Y", "FVector", "Y", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FVector::Y)), static_cast<int32_t>(offsetof(SDK::FVector, Y)) },
                { "FVector::Z", "FVector", "Z", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FVector::Z)), static_cast<int32_t>(offsetof(SDK::FVector, Z)) },
                { "FVector2D::X", "FVector2D", "X", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FVector2D::X)), static_cast<int32_t>(offsetof(SDK::FVector2D, X)) },
                { "FVector2D::Y", "FVector2D", "Y", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FVector2D::Y)), static_cast<int32_t>(offsetof(SDK::FVector2D, Y)) },
                { "FRotator::Pitch", "FRotator", "Pitch", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FRotator::Pitch)), static_cast<int32_t>(offsetof(SDK::FRotator, Pitch)) },
                { "FRotator::Yaw", "FRotator", "Yaw", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FRotator::Yaw)), static_cast<int32_t>(offsetof(SDK::FRotator, Yaw)) },
                { "FRotator::Roll", "FRotator", "Roll", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FRotator::Roll)), static_cast<int32_t>(offsetof(SDK::FRotator, Roll)) },
                { "FSoftObjectPath::AssetPath", "FSoftObjectPath", "AssetPath", PLUGIN_FIELD_CHECK,
                    sizeof(SDK::FTopLevelAssetPath),
                    static_cast<int32_t>(offsetof(SDK::FSoftObjectPath, AssetPath)) },
                { "FTopLevelAssetPath::PackageName", "FTopLevelAssetPath", "PackageName",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::FTopLevelAssetPath::PackageName)),
                    static_cast<int32_t>(offsetof(SDK::FTopLevelAssetPath, PackageName)) },
                { "KismetSystemLibrary_Conv_SoftClassReferenceToString::SoftClassReference",
                    "KismetSystemLibrary_Conv_SoftClassReferenceToString", "SoftClassReference",
                    PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::Params::KismetSystemLibrary_Conv_SoftClassReferenceToString::SoftClassReference)),
                    static_cast<int32_t>(offsetof(SDK::Params::KismetSystemLibrary_Conv_SoftClassReferenceToString,
                        SoftClassReference)) },
                { "KismetSystemLibrary_Conv_SoftClassReferenceToString::ReturnValue",
                    "KismetSystemLibrary_Conv_SoftClassReferenceToString", "ReturnValue",
                    PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::Params::KismetSystemLibrary_Conv_SoftClassReferenceToString::ReturnValue)),
                    static_cast<int32_t>(offsetof(SDK::Params::KismetSystemLibrary_Conv_SoftClassReferenceToString,
                        ReturnValue)) },
                { "KismetSystemLibrary_Conv_SoftObjectReferenceToString::SoftObjectReference",
                    "KismetSystemLibrary_Conv_SoftObjectReferenceToString", "SoftObjectReference",
                    PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::Params::KismetSystemLibrary_Conv_SoftObjectReferenceToString::SoftObjectReference)),
                    static_cast<int32_t>(offsetof(SDK::Params::KismetSystemLibrary_Conv_SoftObjectReferenceToString,
                        SoftObjectReference)) },
                { "KismetSystemLibrary_Conv_SoftObjectReferenceToString::ReturnValue",
                    "KismetSystemLibrary_Conv_SoftObjectReferenceToString", "ReturnValue",
                    PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::Params::KismetSystemLibrary_Conv_SoftObjectReferenceToString::ReturnValue)),
                    static_cast<int32_t>(offsetof(SDK::Params::KismetSystemLibrary_Conv_SoftObjectReferenceToString,
                        ReturnValue)) },
                { "KismetSystemLibrary_Conv_SoftClassReferenceToString",
                    "KismetSystemLibrary_Conv_SoftClassReferenceToString", nullptr, PLUGIN_FIELD_CHECK,
                    sizeof(SDK::Params::KismetSystemLibrary_Conv_SoftClassReferenceToString), -1 },
                { "KismetSystemLibrary_Conv_SoftObjectReferenceToString",
                    "KismetSystemLibrary_Conv_SoftObjectReferenceToString", nullptr, PLUGIN_FIELD_CHECK,
                    sizeof(SDK::Params::KismetSystemLibrary_Conv_SoftObjectReferenceToString), -1 }
            };

    /** Resolves required Client Mass field layouts during the active scanner session. */
    class ClientBuildingMassFieldContract
    {
        /** Resolves an exact field group while the loader scanner session is active. */
        static bool ResolveFields(
            IPluginHookScanner* scanner,
            const IPluginSelf* self,
            PluginFieldSpec* fields,
            std::size_t fieldCount)
        {
            bool allResolved = true;
            for (std::size_t index = 0; index < fieldCount; ++index)
            {
                fields[index].offset = -1;
                fields[index].resolvedSize = 0;
                allResolved = PluginMap::ResolveField(scanner, self, fields[index]) && allResolved;
            }
            return allResolved;
        }

        /** Requires exact spline point/data and fallback payload member layouts. */
        static bool ResolveSplineDataFields(IPluginHookScanner* scanner, const IPluginSelf* self)
        {
            static PluginFieldSpec fields[] = {
                { "FAuAPSplineData::SplineData", "FAuAPSplineData", "SplineData", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FAuAPSplineData::SplineData)),
                    static_cast<int32_t>(offsetof(SDK::FAuAPSplineData, SplineData)) },
                { "FAuAPSplineData::SplineLocationWS", "FAuAPSplineData", "SplineLocationWS",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::FAuAPSplineData::SplineLocationWS)),
                    static_cast<int32_t>(offsetof(SDK::FAuAPSplineData, SplineLocationWS)) },
                { "FAuAPSplineData::SplineRotationWS", "FAuAPSplineData", "SplineRotationWS",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::FAuAPSplineData::SplineRotationWS)),
                    static_cast<int32_t>(offsetof(SDK::FAuAPSplineData, SplineRotationWS)) },
                { "FAuAPSplineData::SplineRealLength", "FAuAPSplineData", "SplineRealLength",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::FAuAPSplineData::SplineRealLength)),
                    static_cast<int32_t>(offsetof(SDK::FAuAPSplineData, SplineRealLength)) },
                { "FAuAPSplinePointData::Index", "FAuAPSplinePointData", "Index", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FAuAPSplinePointData::Index)),
                    static_cast<int32_t>(offsetof(SDK::FAuAPSplinePointData, Index)) },
                { "FAuAPSplinePointData::Position", "FAuAPSplinePointData", "Position", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FAuAPSplinePointData::Position)),
                    static_cast<int32_t>(offsetof(SDK::FAuAPSplinePointData, Position)) },
                { "FAuAPSplinePointData::Tangent", "FAuAPSplinePointData", "Tangent", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FAuAPSplinePointData::Tangent)),
                    static_cast<int32_t>(offsetof(SDK::FAuAPSplinePointData, Tangent)) },
                { "FAuAPSplineActorPayloadData::Data", "FAuAPSplineActorPayloadData", "Data",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::FAuAPSplineActorPayloadData::Data)),
                    static_cast<int32_t>(offsetof(SDK::FAuAPSplineActorPayloadData, Data)) }
            };
            return ResolveFields(scanner, self, fields, sizeof(fields) / sizeof(fields[0]));
        }

        /** Requires exact outer spline channel and version layouts. */
        static bool ResolveSplineCurveContainerFields(IPluginHookScanner* scanner, const IPluginSelf* self)
        {
            static PluginFieldSpec fields[] = {
                { "FSplineCurves::Position", "FSplineCurves", "Position", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FSplineCurves::Position)),
                    static_cast<int32_t>(offsetof(SDK::FSplineCurves, Position)) },
                { "FSplineCurves::Rotation", "FSplineCurves", "Rotation", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FSplineCurves::Rotation)),
                    static_cast<int32_t>(offsetof(SDK::FSplineCurves, Rotation)) },
                { "FSplineCurves::Scale", "FSplineCurves", "Scale", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FSplineCurves::Scale)),
                    static_cast<int32_t>(offsetof(SDK::FSplineCurves, Scale)) },
                { "FSplineCurves::ReparamTable", "FSplineCurves", "ReparamTable", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FSplineCurves::ReparamTable)),
                    static_cast<int32_t>(offsetof(SDK::FSplineCurves, ReparamTable)) },
                { "FSplineCurves::Version", "FSplineCurves", "Version", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FSplineCurves::Version)),
                    static_cast<int32_t>(offsetof(SDK::FSplineCurves, Version)) }
            };
            return ResolveFields(scanner, self, fields, sizeof(fields) / sizeof(fields[0]));
        }

        /** Requires the three curve container headers used by the four copied channels. */
        static bool ResolveSplineChannelFields(IPluginHookScanner* scanner, const IPluginSelf* self)
        {
            static PluginFieldSpec fields[] = {
                { "FInterpCurveVector::Points", "FInterpCurveVector", "Points", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FInterpCurveVector::Points)),
                    static_cast<int32_t>(offsetof(SDK::FInterpCurveVector, Points)) },
                { "FInterpCurveVector::bIsLooped", "FInterpCurveVector", "bIsLooped", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FInterpCurveVector::bIsLooped)),
                    static_cast<int32_t>(offsetof(SDK::FInterpCurveVector, bIsLooped)) },
                { "FInterpCurveVector::LoopKeyOffset", "FInterpCurveVector", "LoopKeyOffset",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::FInterpCurveVector::LoopKeyOffset)),
                    static_cast<int32_t>(offsetof(SDK::FInterpCurveVector, LoopKeyOffset)) },
                { "FInterpCurveQuat::Points", "FInterpCurveQuat", "Points", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FInterpCurveQuat::Points)),
                    static_cast<int32_t>(offsetof(SDK::FInterpCurveQuat, Points)) },
                { "FInterpCurveQuat::bIsLooped", "FInterpCurveQuat", "bIsLooped", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FInterpCurveQuat::bIsLooped)),
                    static_cast<int32_t>(offsetof(SDK::FInterpCurveQuat, bIsLooped)) },
                { "FInterpCurveQuat::LoopKeyOffset", "FInterpCurveQuat", "LoopKeyOffset",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::FInterpCurveQuat::LoopKeyOffset)),
                    static_cast<int32_t>(offsetof(SDK::FInterpCurveQuat, LoopKeyOffset)) },
                { "FInterpCurveFloat::Points", "FInterpCurveFloat", "Points", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FInterpCurveFloat::Points)),
                    static_cast<int32_t>(offsetof(SDK::FInterpCurveFloat, Points)) },
                { "FInterpCurveFloat::bIsLooped", "FInterpCurveFloat", "bIsLooped", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FInterpCurveFloat::bIsLooped)),
                    static_cast<int32_t>(offsetof(SDK::FInterpCurveFloat, bIsLooped)) },
                { "FInterpCurveFloat::LoopKeyOffset", "FInterpCurveFloat", "LoopKeyOffset",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::FInterpCurveFloat::LoopKeyOffset)),
                    static_cast<int32_t>(offsetof(SDK::FInterpCurveFloat, LoopKeyOffset)) }
            };
            return ResolveFields(scanner, self, fields, sizeof(fields) / sizeof(fields[0]));
        }

        /** Requires exact vector-curve point member layouts. */
        static bool ResolveVectorCurvePointFields(IPluginHookScanner* scanner, const IPluginSelf* self)
        {
            static PluginFieldSpec fields[] = {
                { "FInterpCurvePointVector::InVal", "FInterpCurvePointVector", "InVal", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FInterpCurvePointVector::InVal)),
                    static_cast<int32_t>(offsetof(SDK::FInterpCurvePointVector, InVal)) },
                { "FInterpCurvePointVector::OutVal", "FInterpCurvePointVector", "OutVal", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FInterpCurvePointVector::OutVal)),
                    static_cast<int32_t>(offsetof(SDK::FInterpCurvePointVector, OutVal)) },
                { "FInterpCurvePointVector::ArriveTangent", "FInterpCurvePointVector", "ArriveTangent",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::FInterpCurvePointVector::ArriveTangent)),
                    static_cast<int32_t>(offsetof(SDK::FInterpCurvePointVector, ArriveTangent)) },
                { "FInterpCurvePointVector::LeaveTangent", "FInterpCurvePointVector", "LeaveTangent",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::FInterpCurvePointVector::LeaveTangent)),
                    static_cast<int32_t>(offsetof(SDK::FInterpCurvePointVector, LeaveTangent)) },
                { "FInterpCurvePointVector::InterpMode", "FInterpCurvePointVector", "InterpMode",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::FInterpCurvePointVector::InterpMode)),
                    static_cast<int32_t>(offsetof(SDK::FInterpCurvePointVector, InterpMode)) }
            };
            return ResolveFields(scanner, self, fields, sizeof(fields) / sizeof(fields[0]));
        }

        /** Requires exact quaternion-curve point and component layouts. */
        static bool ResolveQuatCurvePointFields(IPluginHookScanner* scanner, const IPluginSelf* self)
        {
            static PluginFieldSpec fields[] = {
                { "FInterpCurvePointQuat::InVal", "FInterpCurvePointQuat", "InVal", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FInterpCurvePointQuat::InVal)),
                    static_cast<int32_t>(offsetof(SDK::FInterpCurvePointQuat, InVal)) },
                { "FInterpCurvePointQuat::OutVal", "FInterpCurvePointQuat", "OutVal", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FInterpCurvePointQuat::OutVal)),
                    static_cast<int32_t>(offsetof(SDK::FInterpCurvePointQuat, OutVal)) },
                { "FInterpCurvePointQuat::ArriveTangent", "FInterpCurvePointQuat", "ArriveTangent",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::FInterpCurvePointQuat::ArriveTangent)),
                    static_cast<int32_t>(offsetof(SDK::FInterpCurvePointQuat, ArriveTangent)) },
                { "FInterpCurvePointQuat::LeaveTangent", "FInterpCurvePointQuat", "LeaveTangent",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::FInterpCurvePointQuat::LeaveTangent)),
                    static_cast<int32_t>(offsetof(SDK::FInterpCurvePointQuat, LeaveTangent)) },
                { "FInterpCurvePointQuat::InterpMode", "FInterpCurvePointQuat", "InterpMode",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::FInterpCurvePointQuat::InterpMode)),
                    static_cast<int32_t>(offsetof(SDK::FInterpCurvePointQuat, InterpMode)) },
                { "FQuat::X", "FQuat", "X", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FQuat::X)), static_cast<int32_t>(offsetof(SDK::FQuat, X)) },
                { "FQuat::Y", "FQuat", "Y", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FQuat::Y)), static_cast<int32_t>(offsetof(SDK::FQuat, Y)) },
                { "FQuat::Z", "FQuat", "Z", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FQuat::Z)), static_cast<int32_t>(offsetof(SDK::FQuat, Z)) },
                { "FQuat::W", "FQuat", "W", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FQuat::W)), static_cast<int32_t>(offsetof(SDK::FQuat, W)) }
            };
            return ResolveFields(scanner, self, fields, sizeof(fields) / sizeof(fields[0]));
        }

        /** Requires exact scalar-curve point member layouts. */
        static bool ResolveFloatCurvePointFields(IPluginHookScanner* scanner, const IPluginSelf* self)
        {
            static PluginFieldSpec fields[] = {
                { "FInterpCurvePointFloat::InVal", "FInterpCurvePointFloat", "InVal", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FInterpCurvePointFloat::InVal)),
                    static_cast<int32_t>(offsetof(SDK::FInterpCurvePointFloat, InVal)) },
                { "FInterpCurvePointFloat::OutVal", "FInterpCurvePointFloat", "OutVal", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FInterpCurvePointFloat::OutVal)),
                    static_cast<int32_t>(offsetof(SDK::FInterpCurvePointFloat, OutVal)) },
                { "FInterpCurvePointFloat::ArriveTangent", "FInterpCurvePointFloat", "ArriveTangent",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::FInterpCurvePointFloat::ArriveTangent)),
                    static_cast<int32_t>(offsetof(SDK::FInterpCurvePointFloat, ArriveTangent)) },
                { "FInterpCurvePointFloat::LeaveTangent", "FInterpCurvePointFloat", "LeaveTangent",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::FInterpCurvePointFloat::LeaveTangent)),
                    static_cast<int32_t>(offsetof(SDK::FInterpCurvePointFloat, LeaveTangent)) },
                { "FInterpCurvePointFloat::InterpMode", "FInterpCurvePointFloat", "InterpMode",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::FInterpCurvePointFloat::InterpMode)),
                    static_cast<int32_t>(offsetof(SDK::FInterpCurvePointFloat, InterpMode)) }
            };
            return ResolveFields(scanner, self, fields, sizeof(fields) / sizeof(fields[0]));
        }

        /** Requires all exact spline data, channel, and curve point field groups. */
        static bool ResolveSplineFields(IPluginHookScanner* scanner, const IPluginSelf* self)
        {
            return ResolveSplineDataFields(scanner, self) &&
                ResolveSplineCurveContainerFields(scanner, self) &&
                ResolveSplineChannelFields(scanner, self) &&
                ResolveVectorCurvePointFields(scanner, self) &&
                ResolveQuatCurvePointFields(scanner, self) &&
                ResolveFloatCurvePointFields(scanner, self);
        }

        /** Requires exact reflected layouts for fields read by V2 definition acquisition. */
        static bool ResolveBuildingDefinitionFields(IPluginHookScanner* scanner, const IPluginSelf* self)
        {
            bool allResolved = true;
            for (PluginFieldSpec& field : g_buildingDefinitionFields)
            {
                field.offset = -1;
                field.resolvedSize = 0;
                allResolved = PluginMap::ResolveField(scanner, self, field) && allResolved;
            }
            return allResolved;
        }

        /** Requires exact reflected offsets and extents for fields read by Foundables. */
        static bool ResolveFoundablesFields(IPluginHookScanner* scanner, const IPluginSelf* self)
        {
            static PluginFieldSpec fields[] = {
                { "FCrInventoryFragment::InputInventoryContainer", "FCrInventoryFragment",
                    "InputInventoryContainer", PLUGIN_FIELD_CHECK,
                    sizeof(SDK::FCrItemsStorageContainer),
                    static_cast<int32_t>(offsetof(SDK::FCrInventoryFragment, InputInventoryContainer)) },
                { "FCrInventoryFragment::MainInventoryContainer", "FCrInventoryFragment",
                    "MainInventoryContainer", PLUGIN_FIELD_CHECK,
                    sizeof(SDK::FCrItemsStorageContainer),
                    static_cast<int32_t>(offsetof(SDK::FCrInventoryFragment, MainInventoryContainer)) },
                { "FCrItemsStorageContainer::Items", "FCrItemsStorageContainer", "Items",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::FCrItemsStorageContainer::Items)),
                    static_cast<int32_t>(offsetof(SDK::FCrItemsStorageContainer, Items)) },
                { "FCrStorageItem::Item", "FCrStorageItem", "Item", PLUGIN_FIELD_CHECK,
                    sizeof(SDK::FAuSimpleItem),
                    static_cast<int32_t>(offsetof(SDK::FCrStorageItem, Item)) },
                { "FAuSimpleItem::ItemId", "FAuSimpleItem", "ItemId", PLUGIN_FIELD_CHECK,
                    sizeof(SDK::FAuItemId),
                    static_cast<int32_t>(offsetof(SDK::FAuSimpleItem, ItemId)) },
                { "FAuSimpleItem::ItemDataBase", "FAuSimpleItem", "ItemDataBase", PLUGIN_FIELD_CHECK,
                    sizeof(SDK::UAuItemDataBase*),
                    static_cast<int32_t>(offsetof(SDK::FAuSimpleItem, ItemDataBase)) },
                { "FAuSimpleItem::Count", "FAuSimpleItem", "Count", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FAuSimpleItem::Count)),
                    static_cast<int32_t>(offsetof(SDK::FAuSimpleItem, Count)) },
                { "FAuItemId::Handle", "FAuItemId", "Handle", PLUGIN_FIELD_CHECK,
                    sizeof(SDK::FGuid), static_cast<int32_t>(offsetof(SDK::FAuItemId, Handle)) },
                { "UAuItemDataBase::UIItemType", "UAuItemDataBase", "UIItemType", PLUGIN_FIELD_CHECK,
                    sizeof(SDK::EUIItemType),
                    static_cast<int32_t>(offsetof(SDK::UAuItemDataBase, UIItemType)) },
                { "FCrMassFoundableParameters::FoundableType", "FCrMassFoundableParameters",
                    "FoundableType", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FCrMassFoundableParameters::FoundableType)),
                    static_cast<int32_t>(offsetof(SDK::FCrMassFoundableParameters, FoundableType)) }
            };

            bool allResolved = true;
            for (PluginFieldSpec& field : fields)
            {
                field.offset = -1;
                field.resolvedSize = 0;
                allResolved = PluginMap::ResolveField(scanner, self, field) && allResolved;
            }
            return allResolved;
        }

    public:
        /** Requires exact compiled offsets and extents through the active loader scanner session. */
        static bool Resolve(IPluginHookScanner* scanner, const IPluginSelf* self)
        {
            static PluginFieldSpec fields[] = {
                { "FMassEntityHandle::Index", "FMassEntityHandle", "Index", PLUGIN_FIELD_CHECK,
                    sizeof(int32_t), static_cast<int32_t>(offsetof(SDK::FMassEntityHandle, Index)) },
                { "FMassEntityHandle::SerialNumber", "FMassEntityHandle", "SerialNumber", PLUGIN_FIELD_CHECK,
                    sizeof(int32_t), static_cast<int32_t>(offsetof(SDK::FMassEntityHandle, SerialNumber)) },
                { "FTransformFragment::Transform", "FTransformFragment", "Transform", PLUGIN_FIELD_CHECK,
                    sizeof(SDK::FTransform), static_cast<int32_t>(offsetof(SDK::FTransformFragment, Transform)) },
                { "FTransform::Rotation", "FTransform", "Rotation", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FTransform::Rotation)),
                    static_cast<int32_t>(offsetof(SDK::FTransform, Rotation)) },
                { "FTransform::Translation", "FTransform", "Translation", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FTransform::Translation)),
                    static_cast<int32_t>(offsetof(SDK::FTransform, Translation)) },
                { "FTransform::Scale3D", "FTransform", "Scale3D", PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::FTransform::Scale3D)),
                    static_cast<int32_t>(offsetof(SDK::FTransform, Scale3D)) },
                { "FMassEnemyStateFragment::State", "FMassEnemyStateFragment", "State",
                    PLUGIN_FIELD_NATIVE | PLUGIN_FIELD_CHECK,
                    sizeof(SDK::EMassEnemyInitializationState), 0x01 },
                { "FAuAPMassFragment::Payload", "FAuAPMassFragment", "Payload", PLUGIN_FIELD_CHECK,
                    sizeof(SDK::FAuAPPayloadDataHandle),
                    static_cast<int32_t>(offsetof(SDK::FAuAPMassFragment, Payload)) },
                { "FAuSplineConnectionFragment::Data", "FAuSplineConnectionFragment", "Data",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::FAuSplineConnectionFragment::Data)),
                    static_cast<int32_t>(offsetof(SDK::FAuSplineConnectionFragment, Data)) },
                { "FAuSplineConnectionFragment::SplineCurves", "FAuSplineConnectionFragment", "SplineCurves",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::FAuSplineConnectionFragment::SplineCurves)),
                    static_cast<int32_t>(offsetof(SDK::FAuSplineConnectionFragment, SplineCurves)) },
                { "FAuSplineConnectionFragment::bSplineCurvesInitialized", "FAuSplineConnectionFragment",
                    "bSplineCurvesInitialized", PLUGIN_FIELD_CHECK, sizeof(bool),
                    static_cast<int32_t>(offsetof(SDK::FAuSplineConnectionFragment, bSplineCurvesInitialized)) },
                { "FCrBuildingParameters::PlacementData", "FCrBuildingParameters", "PlacementData",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::FCrBuildingParameters::PlacementData)),
                    static_cast<int32_t>(offsetof(SDK::FCrBuildingParameters, PlacementData)) }
            };

            bool allResolved = true;
            for (PluginFieldSpec& field : fields)
            {
                field.offset = -1;
                field.resolvedSize = 0;
                allResolved = PluginMap::ResolveField(scanner, self, field) && allResolved;
            }
            const bool foundablesResolved = ResolveFoundablesFields(scanner, self);
            const bool definitionsResolved = ResolveBuildingDefinitionFields(scanner, self);
            const bool splineFieldsResolved = ResolveSplineFields(scanner, self);
            return allResolved && foundablesResolved && definitionsResolved && splineFieldsResolved;
        }
    };

    /** Compares resident Client script-struct size and alignment with the compiled Mass SDK layouts. */
    class ClientBuildingMassTypeContract
    {
        /** Requires a reflected script struct to match the compiled Client SDK size and alignment. */
        static bool CheckType(
            const IPluginSelf* self,
            const char* objectPath,
            const char* displayName,
            std::size_t expectedSize,
            std::size_t expectedAlignment)
        {
            const SDK::UScriptStruct* type = SDK::UObject::FindObject<SDK::UScriptStruct>(
                objectPath, SDK::EClassCastFlags::ScriptStruct);
            if (type == nullptr || type->Size != static_cast<std::int32_t>(expectedSize) ||
                type->MinAlignment != static_cast<std::int32_t>(expectedAlignment))
            {
                if (self && self->logger && self->logger->Error)
                {
                    self->logger->Error(self,
                        "MiniMap: Client admission refused: %s SDK size=0x%X align=0x%X, "
                        "running size=0x%X align=0x%X",
                        displayName, static_cast<unsigned>(expectedSize),
                        static_cast<unsigned>(expectedAlignment),
                        type ? static_cast<unsigned>(type->Size) : 0,
                        type ? static_cast<unsigned>(type->MinAlignment) : 0);
                }
                return false;
            }
            return true;
        }

        /** Requires exact resident layouts for nested definition and spline-profile values. */
        static bool CheckBuildingDefinitionTypes(const IPluginSelf* self)
        {
            return CheckType(self, "ScriptStruct AuActorPlacement.AuAPMassSpawnedEntityType",
                    "FAuAPMassSpawnedEntityType", sizeof(SDK::FAuAPMassSpawnedEntityType),
                    alignof(SDK::FAuAPMassSpawnedEntityType)) &&
                CheckType(self, "ScriptStruct MassSpawner.MassEntityConfig", "FMassEntityConfig",
                    sizeof(SDK::FMassEntityConfig), alignof(SDK::FMassEntityConfig)) &&
                CheckType(self, "ScriptStruct Engine.SplineMeshParams", "FSplineMeshParams",
                    sizeof(SDK::FSplineMeshParams), alignof(SDK::FSplineMeshParams)) &&
                CheckType(self, "ScriptStruct CoreUObject.BoxSphereBounds", "FBoxSphereBounds",
                    sizeof(SDK::FBoxSphereBounds), alignof(SDK::FBoxSphereBounds)) &&
                CheckType(self, "ScriptStruct CoreUObject.Vector", "FVector",
                    sizeof(SDK::FVector), alignof(SDK::FVector)) &&
                CheckType(self, "ScriptStruct CoreUObject.Vector2D", "FVector2D",
                    sizeof(SDK::FVector2D), alignof(SDK::FVector2D)) &&
                CheckType(self, "ScriptStruct CoreUObject.Rotator", "FRotator",
                    sizeof(SDK::FRotator), alignof(SDK::FRotator)) &&
                CheckType(self, "ScriptStruct CoreUObject.SoftObjectPath", "FSoftObjectPath",
                    sizeof(SDK::FSoftObjectPath), alignof(SDK::FSoftObjectPath)) &&
                CheckType(self, "ScriptStruct CoreUObject.TopLevelAssetPath", "FTopLevelAssetPath",
                    sizeof(SDK::FTopLevelAssetPath), alignof(SDK::FTopLevelAssetPath));
        }

        /** Requires exact resident layouts for the nested spline and curve records copied by V2. */
        static bool CheckSplineTypes(const IPluginSelf* self)
        {
            return CheckType(self, "ScriptStruct AuActorPlacement.AuAPSplineData", "FAuAPSplineData",
                    sizeof(SDK::FAuAPSplineData), alignof(SDK::FAuAPSplineData)) &&
                CheckType(self, "ScriptStruct AuActorPlacement.AuAPSplinePointData", "FAuAPSplinePointData",
                    sizeof(SDK::FAuAPSplinePointData), alignof(SDK::FAuAPSplinePointData)) &&
                CheckType(self, "ScriptStruct Engine.SplineCurves", "FSplineCurves",
                    sizeof(SDK::FSplineCurves), alignof(SDK::FSplineCurves)) &&
                CheckType(self, "ScriptStruct CoreUObject.InterpCurveVector", "FInterpCurveVector",
                    sizeof(SDK::FInterpCurveVector), alignof(SDK::FInterpCurveVector)) &&
                CheckType(self, "ScriptStruct CoreUObject.InterpCurveQuat", "FInterpCurveQuat",
                    sizeof(SDK::FInterpCurveQuat), alignof(SDK::FInterpCurveQuat)) &&
                CheckType(self, "ScriptStruct CoreUObject.InterpCurveFloat", "FInterpCurveFloat",
                    sizeof(SDK::FInterpCurveFloat), alignof(SDK::FInterpCurveFloat)) &&
                CheckType(self, "ScriptStruct CoreUObject.InterpCurvePointVector", "FInterpCurvePointVector",
                    sizeof(SDK::FInterpCurvePointVector), alignof(SDK::FInterpCurvePointVector)) &&
                CheckType(self, "ScriptStruct CoreUObject.InterpCurvePointQuat", "FInterpCurvePointQuat",
                    sizeof(SDK::FInterpCurvePointQuat), alignof(SDK::FInterpCurvePointQuat)) &&
                CheckType(self, "ScriptStruct CoreUObject.InterpCurvePointFloat", "FInterpCurvePointFloat",
                    sizeof(SDK::FInterpCurvePointFloat), alignof(SDK::FInterpCurvePointFloat)) &&
                CheckType(self, "ScriptStruct CoreUObject.Quat", "FQuat",
                    sizeof(SDK::FQuat), alignof(SDK::FQuat)) &&
                CheckType(self, "ScriptStruct Engine.Vector_NetQuantize", "FVector_NetQuantize",
                    sizeof(SDK::FVector_NetQuantize), alignof(SDK::FVector_NetQuantize)) &&
                CheckType(self, "ScriptStruct Engine.Vector_NetQuantize10", "FVector_NetQuantize10",
                    sizeof(SDK::FVector_NetQuantize10), alignof(SDK::FVector_NetQuantize10));
        }

        /** Checks the exact resident type used by the native hidden-return payload getter. */
        static bool CheckSplinePayloadType(const IPluginSelf* self)
        {
            const MiniMapNative::NativeApi* native = MiniMapNative::Get();
            const SDK::UScriptStruct* type = native ? native->splinePayload.payloadType : nullptr;
            if (type != nullptr && type->Size == static_cast<std::int32_t>(sizeof(SDK::FAuAPSplineActorPayloadData)) &&
                type->MinAlignment == static_cast<std::int32_t>(alignof(SDK::FAuAPSplineActorPayloadData)))
                return true;

            if (self && self->logger && self->logger->Error)
                self->logger->Error(self,
                    "MiniMap: Client admission refused: native spline payload SDK size=0x%X align=0x%X, "
                    "running size=0x%X align=0x%X",
                    static_cast<unsigned>(sizeof(SDK::FAuAPSplineActorPayloadData)),
                    static_cast<unsigned>(alignof(SDK::FAuAPSplineActorPayloadData)),
                    type ? static_cast<unsigned>(type->Size) : 0,
                    type ? static_cast<unsigned>(type->MinAlignment) : 0);
            return false;
        }

        /** Requires exact resident Terrain record and member layouts used by typed traversal. */
        static bool CheckTerrainTypes(const IPluginSelf* self)
        {
            return CheckType(self, "ScriptStruct ChimeraUI.CrTerrainSegmentData", "FCrTerrainSegmentData",
                    sizeof(SDK::FCrTerrainSegmentData), alignof(SDK::FCrTerrainSegmentData)) &&
                CheckType(self, "ScriptStruct CoreUObject.Vector3f", "FVector3f",
                    sizeof(SDK::FVector3f), alignof(SDK::FVector3f)) &&
                CheckType(self, "ScriptStruct CoreUObject.IntPoint", "FIntPoint",
                    sizeof(SDK::FIntPoint), alignof(SDK::FIntPoint)) &&
                CheckType(self, "ScriptStruct SlateCore.SlateBrush", "FSlateBrush",
                    sizeof(SDK::FSlateBrush), alignof(SDK::FSlateBrush));
        }

        /** Requires resident layouts for the abandoned-base container and its typed array records. */
        static bool CheckPoiStoryTypes(const IPluginSelf* self)
        {
            return CheckType(self, "ScriptStruct Chimera.CrAbandonBaseDataContainer",
                    "FCrAbandonBaseDataContainer", sizeof(SDK::FCrAbandonBaseDataContainer),
                    alignof(SDK::FCrAbandonBaseDataContainer)) &&
                CheckType(self, "ScriptStruct Chimera.CrAbandonBaseData",
                    "FCrAbandonBaseData", sizeof(SDK::FCrAbandonBaseData),
                    alignof(SDK::FCrAbandonBaseData)) &&
                CheckType(self, "ScriptStruct Chimera.CrAbandonBaseDataCollectedStatus",
                    "FCrAbandonBaseDataCollectedStatus",
                    sizeof(SDK::FCrAbandonBaseDataCollectedStatus),
                    alignof(SDK::FCrAbandonBaseDataCollectedStatus));
        }

    public:
        /** Requires each consumed Mass fragment and typed subobject to match resident Client metadata. */
        static bool IsCompatible(const IPluginSelf* self)
        {
            try
            {
                return CheckType(self, "ScriptStruct MassEntity.MassEntityHandle", "FMassEntityHandle",
                        sizeof(SDK::FMassEntityHandle), alignof(SDK::FMassEntityHandle)) &&
                    CheckType(self, "ScriptStruct MassCommon.TransformFragment", "FTransformFragment",
                        sizeof(SDK::FTransformFragment), alignof(SDK::FTransformFragment)) &&
                    CheckType(self, "ScriptStruct CoreUObject.Transform", "FTransform",
                        sizeof(SDK::FTransform), alignof(SDK::FTransform)) &&
                    CheckType(self,
                        "ScriptStruct MassAIPrototypeEnemyRuntime.MassEnemyStateFragment",
                        "FMassEnemyStateFragment", sizeof(SDK::FMassEnemyStateFragment),
                        alignof(SDK::FMassEnemyStateFragment)) &&
                    CheckType(self, "ScriptStruct AuActorPlacement.AuAPMassFragment", "FAuAPMassFragment",
                        sizeof(SDK::FAuAPMassFragment), alignof(SDK::FAuAPMassFragment)) &&
                    CheckType(self, "ScriptStruct AuActorPlacement.AuAPPayloadDataHandle",
                        "FAuAPPayloadDataHandle", sizeof(SDK::FAuAPPayloadDataHandle),
                        alignof(SDK::FAuAPPayloadDataHandle)) &&
                    CheckType(self, "ScriptStruct AuActorPlacement.AuSplineConnectionFragment",
                        "FAuSplineConnectionFragment", sizeof(SDK::FAuSplineConnectionFragment),
                        alignof(SDK::FAuSplineConnectionFragment)) &&
                    CheckType(self, "ScriptStruct Chimera.CrBuildingParameters", "FCrBuildingParameters",
                        sizeof(SDK::FCrBuildingParameters), alignof(SDK::FCrBuildingParameters)) &&
                    CheckType(self, "ScriptStruct Chimera.CrInventoryFragment", "FCrInventoryFragment",
                        sizeof(SDK::FCrInventoryFragment), alignof(SDK::FCrInventoryFragment)) &&
                    CheckType(self, "ScriptStruct Chimera.CrItemsStorageContainer", "FCrItemsStorageContainer",
                        sizeof(SDK::FCrItemsStorageContainer), alignof(SDK::FCrItemsStorageContainer)) &&
                    CheckType(self, "ScriptStruct Chimera.CrStorageItem", "FCrStorageItem",
                        sizeof(SDK::FCrStorageItem), alignof(SDK::FCrStorageItem)) &&
                    CheckType(self, "ScriptStruct AuItems.AuSimpleItem", "FAuSimpleItem",
                        sizeof(SDK::FAuSimpleItem), alignof(SDK::FAuSimpleItem)) &&
                    CheckType(self, "ScriptStruct AuItems.AuItemId", "FAuItemId",
                        sizeof(SDK::FAuItemId), alignof(SDK::FAuItemId)) &&
                    CheckType(self, "ScriptStruct CoreUObject.Guid", "FGuid",
                        sizeof(SDK::FGuid), alignof(SDK::FGuid)) &&
                    CheckType(self, "ScriptStruct Chimera.CrMassFoundableParameters",
                        "FCrMassFoundableParameters", sizeof(SDK::FCrMassFoundableParameters),
                        alignof(SDK::FCrMassFoundableParameters)) &&
                    CheckBuildingDefinitionTypes(self) &&
                    CheckSplineTypes(self) &&
                    CheckTerrainTypes(self) &&
                    CheckPoiStoryTypes(self) &&
                    CheckSplinePayloadType(self);
            }
            catch (...)
            {
                if (self && self->logger && self->logger->Error)
                    self->logger->Error(self,
                        "MiniMap: Client admission refused: required script-struct metadata lookup failed");
                return false;
            }
        }
    };

    /** Checks the reflected query layout before a Client subsystem can construct fixed native query storage. */
    class ClientMassQueryStorageContract
    {
    public:
        /** Requires the running MassEntity struct to match the SDK size and alignment used by all query owners. */
        static bool IsCompatible(const IPluginSelf* self)
        {
            try
            {
                const SDK::UScriptStruct* queryType = SDK::UObject::FindObject<SDK::UScriptStruct>(
                    "ScriptStruct MassEntity.MassEntityQuery", SDK::EClassCastFlags::ScriptStruct);
                if (queryType == nullptr)
                {
                    if (self && self->logger && self->logger->Error)
                        self->logger->Error(self,
                            "MiniMap: Client admission refused: MassEntity.MassEntityQuery is unavailable");
                    return false;
                }

                const unsigned sdkSize = static_cast<unsigned>(sizeof(SDK::FMassEntityQuery));
                const unsigned sdkAlignment = static_cast<unsigned>(alignof(SDK::FMassEntityQuery));
                if (queryType->Size != static_cast<std::int32_t>(sdkSize) ||
                    queryType->MinAlignment != static_cast<std::int32_t>(sdkAlignment))
                {
                    if (self && self->logger && self->logger->Error)
                        self->logger->Error(self,
                            "MiniMap: Client admission refused: FMassEntityQuery SDK size=0x%X align=0x%X, "
                            "running size=0x%X align=0x%X",
                            sdkSize, sdkAlignment, static_cast<unsigned>(queryType->Size),
                            static_cast<unsigned>(queryType->MinAlignment));
                    return false;
                }

                return true;
            }
            catch (...)
            {
                if (self && self->logger && self->logger->Error)
                    self->logger->Error(self,
                        "MiniMap: Client admission refused: FMassEntityQuery metadata lookup failed");
                return false;
            }
        }
    };
#endif

    struct HookBaseline
    {
        bool Captured = false;
        const void* Self = nullptr;
        const void* Hooks = nullptr;
        const void* Engine = nullptr;
        const void* ImGuiTextures = nullptr;
    } g_baseline;

#ifdef MODLOADER_SERVER_BUILD
#define MINIMAP_EXPORTS_TARGET PLUGIN_TARGET_SERVER
#else
#define MINIMAP_EXPORTS_TARGET PLUGIN_TARGET_CLIENT
#endif

    PluginInfo g_pluginInfo = {
        "MiniMap",
        MODLOADER_BUILD_TAG,
        "Kian369",
        "A configurable on-screen minimap for StarRupture",
        PLUGIN_INTERFACE_VERSION,
        MINIMAP_EXPORTS_TARGET,
        PLUGIN_GAME_BUILT_CL,
        PluginLoadSetting(PluginReload::MENU) |
            PluginUnloadSetting(PluginReload::MENU)
    };

    void ReleaseRuntime() noexcept
    {
        // Called only after the existing core cleanup/rollback has returned.
#if defined(MODLOADER_CLIENT_BUILD)
        if (!g_textures.Stop())
        {
            LOG_ERROR("MiniMap: texture drain incomplete; runtime retained");
            return;
        }
        g_buildingMassFieldsResolved = false;
#endif
        g_baseline = {};
        g_binding.reset();
        g_self = nullptr;
    }
}

namespace MiniMap::Services
{
    ILog* TryLog() noexcept
    {
        if (const auto* runtime = TryRuntime()) return &runtime->log;
        // Explicit scan/bootstrap path: no partially initialized Runtime.
        return g_self ? &g_log : nullptr;
    }
}

namespace MiniMap::Adapters::AlienX
{
    IPluginSelf* GetSelf() noexcept { return g_self; }
    Configuration& GetConfiguration() noexcept { return g_configuration; }
    Input& GetInput() noexcept { return g_input; }
#if defined(MODLOADER_CLIENT_BUILD)
    RuntimeEvents& GetRuntimeEvents() noexcept { return g_events; }
    Viewport& GetViewport() noexcept { return g_viewport; }
    Textures& GetTextures() noexcept { return g_textures; }
#endif

    void CapturePluginInitHookBaseline(IPluginSelf* self) noexcept
    {
        auto* hooks = self ? self->hooks : nullptr;
        g_baseline = {true, self, hooks, hooks ? hooks->Engine : nullptr,
            hooks ? hooks->ImGuiTextures : nullptr};
    }

    void LogTerrainHookAvailability(IPluginSelf* self) noexcept
    {
        auto* hooks = self ? self->hooks : nullptr;
        auto* engine = hooks ? hooks->Engine : nullptr;
        auto* textures = hooks ? hooks->ImGuiTextures : nullptr;
        LOG_ERROR(
            "MiniMap: terrain initialization failed: required hooks are unavailable; "
            "available self=%d hooks=%d engine=%d imguiTextures=%d; "
            "matches PluginInit baseline captured=%d self=%d hooks=%d engine=%d imguiTextures=%d",
            int(self != nullptr), int(hooks != nullptr), int(engine != nullptr),
            int(textures != nullptr), int(g_baseline.Captured),
            int(g_baseline.Captured && self && g_baseline.Self && static_cast<const void*>(self) == g_baseline.Self),
            int(g_baseline.Captured && hooks && g_baseline.Hooks && static_cast<const void*>(hooks) == g_baseline.Hooks),
            int(g_baseline.Captured && engine && g_baseline.Engine && static_cast<const void*>(engine) == g_baseline.Engine),
            int(g_baseline.Captured && textures && g_baseline.ImGuiTextures &&
                static_cast<const void*>(textures) == g_baseline.ImGuiTextures));
    }
}

extern "C" __declspec(dllexport) PluginInfo* GetPluginInfo()
{
    return &g_pluginInfo;
}

extern "C" __declspec(dllexport) void OnPluginLoadHooks(
    IPluginSelf* self, IPluginHookScanner* scanner)
{
    // The loader calls this before PluginInit. Borrow self only for this scope;
    // the scanner adapter and its borrowed session exist only inside this call.
    if (g_binding) return;
#if defined(MODLOADER_CLIENT_BUILD)
    g_buildingMassFieldsResolved = false;
    g_fowDataFieldResolved = false;
    g_playerMapDataComponentFieldResolved = false;
    g_terrainFieldsResolved = false;
    g_poiStoryFieldsResolved = false;
    g_playerTextureFieldsResolved = false;
#endif
    g_self = self;
    struct ScanLogScope { ~ScanLogScope() noexcept { g_self = nullptr; } } scope;
    try
    {
#if defined(MODLOADER_CLIENT_BUILD)
        if (!self || !scanner || !scanner->Resolve ||
            !scanner->ReportFailure || !scanner->HasFailures)
        {
            LOG_ERROR("MiniMap: native fingerprint resolution failed: invalid required scanner callback arguments");
            MiniMapNative::Shutdown();
            // A malformed callback can only be reported when the loader supplied
            // its identity and reporting operation. No alternative failure API.
            if (self && scanner && scanner->ReportFailure)
                scanner->ReportFailure(self, "MiniMap::NativeDependencies", "invalid required scanner callback arguments");
            return;
        }
        MiniMap::Adapters::AlienX::RequiredScanner requiredScanner(*self, *scanner);
        MiniMap::Adapters::AlienX::ResolveNativeDependencies(requiredScanner);
        if (requiredScanner.HasFailures())
        {
            MiniMapNative::Shutdown();
            return;
        }

        PluginMap::MassEntityManagerField.offset = -1;
        PluginMap::MassEntityManagerField.resolvedSize = 0;
        if (!PluginMap::ResolveMassEntityManager(scanner, self))
        {
            MiniMapNative::Shutdown();
            return;
        }

        const int32_t managerOffset = PluginMap::MassEntityManagerField.offset;
        const uint32_t managerSize = PluginMap::MassEntityManagerField.resolvedSize;
        if (managerOffset != 0x38 || managerSize != 0x10)
        {
            requiredScanner.Fail(
                "MiniMap::MassEntityManagerField",
                "resolved EntityManager field does not match CL-127004 offset 0x38 and size 0x10");
            MiniMapNative::Shutdown();
            return;
        }

        if (!MiniMapNative::PublishMassEntityManagerField(managerOffset, managerSize))
        {
            requiredScanner.Fail(
                "MiniMap::MassEntityManagerField",
                "validated EntityManager field could not be published by NativeApi");
            MiniMapNative::Shutdown();
            return;
        }

        if (!ClientBuildingMassFieldContract::Resolve(scanner, self))
        {
            MiniMapNative::Shutdown();
            return;
        }
        g_buildingMassFieldsResolved = true;

        g_fowDataField.offset = -1;
        g_fowDataField.resolvedSize = 0;
        if (!PluginMap::ResolveField(
            scanner,
            self,
            g_fowDataField,
            kFowDataCandidates,
            static_cast<uint32_t>(
                sizeof(kFowDataCandidates) / sizeof(kFowDataCandidates[0]))))
        {
            MiniMapNative::Shutdown();
            return;
        }
        if (g_fowDataField.offset != 0x100 ||
            g_fowDataField.resolvedSize != 0x10)
        {
            requiredScanner.Fail(
                "UCrPlayerMapMenuDataComponent::FOWData",
                "resolved FOWData does not match CL-127004 offset 0x100 and extent 0x10");
            MiniMapNative::Shutdown();
            return;
        }
        g_fowDataFieldResolved = true;

        g_playerMapDataComponentField.offset = -1;
        g_playerMapDataComponentField.resolvedSize = 0;
        if (!PluginMap::ResolveField(
            scanner,
            self,
            g_playerMapDataComponentField))
        {
            MiniMapNative::Shutdown();
            return;
        }
        g_playerMapDataComponentFieldResolved = true;

        if (!ClientTerrainFieldContract::Resolve(scanner, self))
        {
            MiniMapNative::Shutdown();
            return;
        }
        g_terrainFieldsResolved = true;

        if (!ClientPoiStoryFieldContract::Resolve(scanner, self, requiredScanner))
        {
            MiniMapNative::Shutdown();
            return;
        }
        g_poiStoryFieldsResolved = true;

        if (!ClientPlayerTextureFieldContract::Resolve(scanner, self, requiredScanner))
        {
            MiniMapNative::Shutdown();
            return;
        }
        g_playerTextureFieldsResolved = true;
#else
        (void)scanner; // Existing server/generic callback performs no native scans.
#endif
    }
    catch (...)
    {
        LOG_ERROR("MiniMap: exception during native dependency initialization");
#if defined(MODLOADER_CLIENT_BUILD)
        MiniMapNative::Shutdown();
#endif
        try
        {
            if (self && scanner && scanner->ReportFailure)
                scanner->ReportFailure(self, "MiniMap::NativeDependencies", "exception during native dependency initialization");
        }
        catch (...) {} // Never cross the exported loader boundary.
    }
}

extern "C" __declspec(dllexport) bool PluginInit(IPluginSelf* self)
{
#if defined(MODLOADER_CLIENT_BUILD)
    if (!PluginSdkRuntime::Init(self ? self->hooks : nullptr))
    {
        MiniMapNative::Shutdown();
        if (self && self->logger && self->logger->Log)
            self->logger->Log(PluginLogLevel::Error, self,
                "MiniMap: SDK runtime initialization failed; loader game globals are unavailable");
        return false;
    }
    if (!ClientMassQueryStorageContract::IsCompatible(self))
    {
        MiniMapNative::Shutdown();
        return false;
    }
    if (!g_buildingMassFieldsResolved)
    {
        if (self && self->logger && self->logger->Error)
            self->logger->Error(self,
                "MiniMap: Client admission refused: required Mass fields were not validated");
        MiniMapNative::Shutdown();
        return false;
    }
    if (!g_fowDataFieldResolved)
    {
        if (self && self->logger && self->logger->Error)
            self->logger->Error(self,
                "MiniMap: Client admission refused: native FOWData layout was not validated");
        MiniMapNative::Shutdown();
        return false;
    }
    if (!g_playerMapDataComponentFieldResolved)
    {
        if (self && self->logger && self->logger->Error)
            self->logger->Error(self,
                "MiniMap: Client admission refused: player map-data component field was not validated");
        MiniMapNative::Shutdown();
        return false;
    }
    if (!g_terrainFieldsResolved)
    {
        if (self && self->logger && self->logger->Error)
            self->logger->Error(self,
                "MiniMap: Client admission refused: required Terrain fields were not validated");
        MiniMapNative::Shutdown();
        return false;
    }
    if (!g_poiStoryFieldsResolved)
    {
        if (self && self->logger && self->logger->Error)
            self->logger->Error(self,
                "MiniMap: Client admission refused: required POI and story fields were not validated");
        MiniMapNative::Shutdown();
        return false;
    }
    if (!g_playerTextureFieldsResolved)
    {
        if (self && self->logger && self->logger->Error)
            self->logger->Error(self,
                "MiniMap: Client admission refused: required player-tracking and texture fields were not validated");
        MiniMapNative::Shutdown();
        return false;
    }
    if (MiniMapNative::Get() == nullptr)
    {
        if (self && self->logger && self->logger->Error)
            self->logger->Error(self,
                "MiniMap: Client admission refused: native Mass EntityManager field was not validated");
        MiniMapNative::Shutdown();
        return false;
    }
    if (!ClientBuildingMassTypeContract::IsCompatible(self))
    {
        MiniMapNative::Shutdown();
        return false;
    }
#endif
    if (g_binding) return false; // Never replace an active composition/identity.
    g_baseline = {};
    g_self = self;
    g_binding.emplace(g_runtime);
    if (!g_binding->IsBound())
    {
        ReleaseRuntime();
        return false;
    }
    try
    {
#if defined(MODLOADER_CLIENT_BUILD)
        g_textures.Initialize(self); // No native texture acquisitions/releases.
#endif
        if (MiniMap::Initialize()) return true;
        // Existing initialization guards/explicit rollback finish while bound.
        ReleaseRuntime();
    }
    catch (...)
    {
        LOG_ERROR("MiniMap: exception during core initialization");
#if defined(MODLOADER_CLIENT_BUILD)
        if (!g_textures.CloseAcquisitions())
        {
            LOG_ERROR("MiniMap: reentrant texture operation during initialization cleanup; runtime retained");
            return false;
        }
#endif
        try
        {
            // Exceptional rollback uses the existing core shutdown order, with
            // the same Runtime/identity still alive for every cleanup consumer.
            MiniMap::Shutdown();
            ReleaseRuntime();
        }
        catch (...)
        {
            // An interrupted cleanup is not completion. Do not unbind services
            // beneath remaining consumers or retry ambiguous native operations.
            LOG_ERROR("MiniMap: initialization cleanup exception; runtime retained because core cleanup is incomplete");
        }
    }
    return false;
}

extern "C" __declspec(dllexport) void PluginShutdown()
{
#if defined(MODLOADER_CLIENT_BUILD)
    if (!g_textures.CloseAcquisitions())
    {
        LOG_ERROR("MiniMap: reentrant texture operation during shutdown; runtime retained");
        return;
    }
#endif
    try
    {
        MiniMap::Shutdown();
    }
    catch (...)
    {
        LOG_ERROR("MiniMap: shutdown exception; runtime retained because core cleanup is incomplete");
        return;
    }
    ReleaseRuntime();
}
