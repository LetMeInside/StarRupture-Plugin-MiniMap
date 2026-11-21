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
#include "Map/Map.h"
#include "Map/FogOfWar/FogOfWar.h"
#include "Map/Foundables/Foundables.h"
#include "Map/PointsOfInterest/PointsOfInterest.h"
#include "Map/Terrain/TerrainSource.h"
#include "Map/Buildings/BuildingDefinitionAcquisition.h"
#include "Map/Buildings/BuildingSplineSource.h"
#include "Services/Map/Buildings/Capture/BuildingSpriteRenderer.h"
#include "Services/RuntimeBinding.h"
#include "plugin.h"
#include "plugin_helpers.h"
#include "resource.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
    bool g_captureFieldsResolved = false;
    MiniMapMap::PlayerFieldOffsets g_playerFieldOffsets{};
    MiniMapPointsOfInterest::FieldOffsets g_poiFieldOffsets{};
    MiniMapFoundables::AcquisitionFieldOffsets g_foundablesFieldOffsets{};

    PluginFieldSpec g_fowDataField{
        "UCrPlayerMapMenuDataComponent::FOWData",
        "UCrPlayerMapMenuDataComponent",
        "FOWData",
        PLUGIN_FIELD_NATIVE,
        sizeof(SDK::TArray<uint8_t>),
        -1
    };

    PluginFieldSpec g_fowDataClassSize = SR_SIZE(UCrPlayerMapMenuDataComponent);

    constexpr PluginFieldCandidate kFowDataCandidates[] = {
        { 127004, 0x100, 0x10, 0, nullptr }
    };

    PluginFieldSpec g_playerMapDataComponentField{
        "ACrCharacterPlayerBase::PlayerMapMenuDataComponent",
        "ACrCharacterPlayerBase",
        "PlayerMapMenuDataComponent",
        PLUGIN_FIELD_FLAG_NONE,
        sizeof(decltype(SDK::ACrCharacterPlayerBase::PlayerMapMenuDataComponent)),
        -1
    };

    PluginFieldSpec g_playerCharacterClassSize = SR_SIZE(ACrCharacterPlayerBase);

    /** Resolves Terrain fields and validates runtime containing extents before publishing offsets. */
    class ClientTerrainFieldContract
    {
    public:
        /** Resolves Terrain metadata once during Client admission and publishes validated access offsets. */
        static bool Resolve(
            IPluginHookScanner* scanner,
            const IPluginSelf* self,
            MiniMap::Adapters::AlienX::RequiredScanner& requiredScanner)
        {
            static PluginFieldSpec fields[] = {
                { "UCrMapMenuDevSettings::TerrainData", "UCrMapMenuDevSettings", "TerrainData",
                    PLUGIN_FIELD_FLAG_NONE, sizeof(decltype(SDK::UCrMapMenuDevSettings::TerrainData)),
                    static_cast<int32_t>(offsetof(SDK::UCrMapMenuDevSettings, TerrainData)) },
                { "UCrMapMenuTerrainData::MapTerrainTopLeftPivotPoint", "UCrMapMenuTerrainData",
                    "MapTerrainTopLeftPivotPoint", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::UCrMapMenuTerrainData::MapTerrainTopLeftPivotPoint)),
                    static_cast<int32_t>(offsetof(SDK::UCrMapMenuTerrainData, MapTerrainTopLeftPivotPoint)) },
                { "UCrMapMenuTerrainData::MapTerrainSegmentSize", "UCrMapMenuTerrainData",
                    "MapTerrainSegmentSize", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::UCrMapMenuTerrainData::MapTerrainSegmentSize)),
                    static_cast<int32_t>(offsetof(SDK::UCrMapMenuTerrainData, MapTerrainSegmentSize)) },
                { "UCrMapMenuTerrainData::TerrainSegmentsData", "UCrMapMenuTerrainData",
                    "TerrainSegmentsData", PLUGIN_FIELD_FLAG_NONE,
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
                    "TerrainSegmentGridIndex", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::FCrTerrainSegmentData::TerrainSegmentGridIndex)),
                    static_cast<int32_t>(offsetof(SDK::FCrTerrainSegmentData, TerrainSegmentGridIndex)) },
                { "FCrTerrainSegmentData::TerrainSegmentTexture", "FCrTerrainSegmentData",
                    "TerrainSegmentTexture", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::FCrTerrainSegmentData::TerrainSegmentTexture)),
                    static_cast<int32_t>(offsetof(SDK::FCrTerrainSegmentData, TerrainSegmentTexture)) },
                { "FCrTerrainSegmentData::TerrainSegmentTextureRadiation2", "FCrTerrainSegmentData",
                    "TerrainSegmentTextureRadiation2", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::FCrTerrainSegmentData::TerrainSegmentTextureRadiation2)),
                    static_cast<int32_t>(offsetof(SDK::FCrTerrainSegmentData, TerrainSegmentTextureRadiation2)) },
                { "UWorld::GameState", "UWorld", "GameState", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::UWorld::GameState)),
                    static_cast<int32_t>(offsetof(SDK::UWorld, GameState)) },
                { "ACrGameStateBase::MapMenuDataReplicationHelper", "ACrGameStateBase",
                    "MapMenuDataReplicationHelper", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::ACrGameStateBase::MapMenuDataReplicationHelper)),
                    static_cast<int32_t>(offsetof(SDK::ACrGameStateBase, MapMenuDataReplicationHelper)) },
                { "ACrMapMenuDataReplicationHelper::CurrentRadiationLevelReplicated",
                    "ACrMapMenuDataReplicationHelper", "CurrentRadiationLevelReplicated",
                    PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::ACrMapMenuDataReplicationHelper::CurrentRadiationLevelReplicated)),
                    static_cast<int32_t>(offsetof(
                        SDK::ACrMapMenuDataReplicationHelper, CurrentRadiationLevelReplicated)) }
            };

            static PluginFieldSpec classSizes[] = {
                SR_SIZE(UCrMapMenuDevSettings),
                SR_SIZE(UCrMapMenuTerrainData),
                SR_SIZE(FCrTerrainSegmentData),
                SR_SIZE(UWorld),
                SR_SIZE(ACrGameStateBase),
                SR_SIZE(ACrMapMenuDataReplicationHelper)
            };

            bool allResolved = true;
            for (PluginFieldSpec& field : fields)
            {
                field.offset = -1;
                field.resolvedSize = 0;
                allResolved = PluginMap::ResolveField(scanner, self, field) && allResolved;
            }
            for (PluginFieldSpec& classSize : classSizes)
            {
                classSize.offset = -1;
                classSize.resolvedSize = 0;
                allResolved = PluginMap::ResolveField(scanner, self, classSize) && allResolved;
            }

            if (!allResolved)
            {
                return false;
            }

            const auto memberFits = [](
                const PluginFieldSpec& field,
                uint32_t containingSize,
                uint32_t expectedSize)
            {
                return field.offset >= 0 && field.resolvedSize == expectedSize &&
                    static_cast<uint64_t>(field.offset) + field.resolvedSize <= containingSize;
            };
            const bool objectFieldsFit =
                memberFits(fields[0], classSizes[0].resolvedSize, sizeof(SDK::TSoftObjectPtr<SDK::UCrMapMenuTerrainData>)) &&
                memberFits(fields[1], classSizes[1].resolvedSize, sizeof(SDK::FVector3f)) &&
                memberFits(fields[2], classSizes[1].resolvedSize, sizeof(SDK::FVector3f)) &&
                memberFits(fields[3], classSizes[1].resolvedSize, sizeof(SDK::TArray<SDK::FCrTerrainSegmentData>)) &&
                memberFits(fields[12], classSizes[3].resolvedSize, sizeof(SDK::AGameStateBase*)) &&
                memberFits(fields[13], classSizes[4].resolvedSize,
                    sizeof(SDK::ACrMapMenuDataReplicationHelper*)) &&
                memberFits(fields[14], classSizes[5].resolvedSize, sizeof(int32_t)) &&
                fields[0].offset % alignof(void*) == 0 &&
                fields[1].offset % alignof(SDK::FVector3f) == 0 &&
                fields[2].offset % alignof(SDK::FVector3f) == 0 &&
                fields[3].offset % alignof(SDK::TArray<SDK::FCrTerrainSegmentData>) == 0 &&
                fields[12].offset % alignof(SDK::AGameStateBase*) == 0 &&
                fields[13].offset % alignof(SDK::ACrMapMenuDataReplicationHelper*) == 0 &&
                fields[14].offset % alignof(int32_t) == 0;
            const uint32_t terrainStride = classSizes[2].resolvedSize;
            const bool segmentFieldsFit = terrainStride != 0 &&
                terrainStride % alignof(SDK::FSlateBrush) == 0 &&
                memberFits(fields[9], terrainStride, sizeof(SDK::FIntPoint)) &&
                memberFits(fields[10], terrainStride, sizeof(SDK::FSlateBrush)) &&
                memberFits(fields[11], terrainStride, sizeof(SDK::FSlateBrush)) &&
                fields[9].offset % alignof(SDK::FIntPoint) == 0 &&
                fields[10].offset % alignof(SDK::FSlateBrush) == 0 &&
                fields[11].offset % alignof(SDK::FSlateBrush) == 0;
            if (!objectFieldsFit || !segmentFieldsFit)
            {
                requiredScanner.Fail(
                    "MiniMap::TerrainFields",
                    "resolved Terrain field extent or runtime segment stride is invalid");
                return false;
            }

            MiniMapTerrainSource::FieldOffsets terrainOffsets{};
            terrainOffsets.DevSettingsTerrainData = fields[0].offset;
            terrainOffsets.TerrainDataPivot = fields[1].offset;
            terrainOffsets.TerrainDataSegmentSize = fields[2].offset;
            terrainOffsets.TerrainDataSegments = fields[3].offset;
            terrainOffsets.SegmentGridIndex = fields[9].offset;
            terrainOffsets.SegmentTexture = fields[10].offset;
            terrainOffsets.SegmentTextureRadiation2 = fields[11].offset;
            terrainOffsets.WorldGameState = fields[12].offset;
            terrainOffsets.GameStateReplicationHelper = fields[13].offset;
            terrainOffsets.RadiationLevel = fields[14].offset;
            terrainOffsets.TerrainSegmentStride = terrainStride;
            MiniMapTerrainSource::SetFieldOffsets(terrainOffsets);
            g_poiFieldOffsets.WorldGameState = fields[12].offset;
            g_foundablesFieldOffsets.WorldGameState = fields[12].offset;
            g_foundablesFieldOffsets.GameStateClassSize = classSizes[4].resolvedSize;
            return true;
        }
    };

    PluginBitField g_actorBeingDestroyedField{
        { "AActor::bActorIsBeingDestroyed", "AActor", "bActorIsBeingDestroyed",
            PLUGIN_FIELD_BITFIELD, 1, -1 }
    };

    PluginFieldSpec g_actorClassSize = SR_SIZE(AActor);

    PluginBitField g_textureSRGBField{
        { "UTexture::SRGB", "UTexture", "SRGB",
            PLUGIN_FIELD_BITFIELD, 1, -1 }
    };

    PluginFieldSpec g_textureClassSize = SR_SIZE(UTexture);

    /** Resolves reflected POI, story-record, category, and actor fields and validates required extents. */
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
            allResolved = ResolveReplicatorFields(scanner, self, requiredScanner) && allResolved;
            allResolved = ResolveStoryRecordFields(scanner, self) && allResolved;
            allResolved = ResolvePoiActorFields(scanner, self) && allResolved;
            allResolved = ResolveCategoryFields(scanner, self) && allResolved;
            allResolved = ResolveActorIdentityFields(scanner, self) && allResolved;
            allResolved = ResolveActorBeingDestroyed(scanner, self, requiredScanner) && allResolved;
            if (allResolved)
            {
                MiniMapPointsOfInterest::SetFieldOffsets(g_poiFieldOffsets);
                MiniMapFoundables::SetAcquisitionFieldOffsets(g_foundablesFieldOffsets);
            }

            return allResolved;
        }

    private:
        /** Resolves a fixed group of fields through the active loader scanner session. */
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

        /** Verifies a resolved array-element member lies inside its runtime-sized element. */
        static bool MemberFitsSize(const PluginFieldSpec& member, const PluginFieldSpec& size)
        {
            return member.offset >= 0 && member.resolvedSize != 0 && size.resolvedSize != 0 &&
                uint64_t(member.offset) + member.resolvedSize <= size.resolvedSize;
        }

        /** Resolves the game-state path to the abandoned-base replication data. */
        static bool ResolveReplicatorFields(
            IPluginHookScanner* scanner,
            const IPluginSelf* self,
            MiniMap::Adapters::AlienX::RequiredScanner& requiredScanner)
        {
            static PluginFieldSpec fields[] = {
                { "ACrGameStateBase::AbandonBaseInfoReplicator", "ACrGameStateBase",
                    "AbandonBaseInfoReplicator", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::ACrGameStateBase::AbandonBaseInfoReplicator)),
                    static_cast<int32_t>(offsetof(SDK::ACrGameStateBase, AbandonBaseInfoReplicator)) },
                { "ACrAbandonBaseInfoReplicator::AbandonBaseDataContainer",
                    "ACrAbandonBaseInfoReplicator", "AbandonBaseDataContainer", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::ACrAbandonBaseInfoReplicator::AbandonBaseDataContainer)),
                    static_cast<int32_t>(offsetof(SDK::ACrAbandonBaseInfoReplicator, AbandonBaseDataContainer)) },
                { "FCrAbandonBaseDataContainer::AbandonBaseData", "FCrAbandonBaseDataContainer",
                    "AbandonBaseData", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::FCrAbandonBaseDataContainer::AbandonBaseData)),
                    static_cast<int32_t>(offsetof(SDK::FCrAbandonBaseDataContainer, AbandonBaseData)) }
            };
            const bool resolved = ResolveFields(scanner, self, fields, std::size(fields));
            const uint64_t fieldEnd = static_cast<uint64_t>(fields[0].offset) + fields[0].resolvedSize;
            const bool gameStateFieldFits = resolved && fields[0].offset >= 0 &&
                fields[0].resolvedSize == sizeof(SDK::ACrAbandonBaseInfoReplicator*) &&
                fields[0].offset % alignof(SDK::ACrAbandonBaseInfoReplicator*) == 0 &&
                g_foundablesFieldOffsets.GameStateClassSize != 0 &&
                fieldEnd <= g_foundablesFieldOffsets.GameStateClassSize;
            if (!gameStateFieldFits)
            {
                requiredScanner.Fail(
                    "ACrGameStateBase::AbandonBaseInfoReplicator",
                    "resolved GameState member does not fit its runtime class extent");
                return false;
            }
            g_poiFieldOffsets.GameStateAbandonBaseInfoReplicator = fields[0].offset;
            g_poiFieldOffsets.ReplicatorAbandonBaseDataContainer = fields[1].offset;
            g_poiFieldOffsets.AbandonBaseContainerData = fields[2].offset;
            g_foundablesFieldOffsets.GameStateAbandonBaseInfoReplicator = fields[0].offset;
            g_foundablesFieldOffsets.ReplicatorAbandonBaseDataContainer = fields[1].offset;
            g_foundablesFieldOffsets.AbandonBaseContainerData = fields[2].offset;
            return resolved;
        }

        /** Resolves the consumed members of replicated abandoned-base story records. */
        static bool ResolveStoryRecordFields(IPluginHookScanner* scanner, const IPluginSelf* self)
        {
            static PluginFieldSpec fields[] = {
                { "FCrAbandonBaseData::AbandonBasePOIId", "FCrAbandonBaseData",
                    "AbandonBasePOIId", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::FCrAbandonBaseData::AbandonBasePOIId)),
                    static_cast<int32_t>(offsetof(SDK::FCrAbandonBaseData, AbandonBasePOIId)) },
                { "FCrAbandonBaseData::StoryItemsCollectedStatus", "FCrAbandonBaseData",
                    "StoryItemsCollectedStatus", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::FCrAbandonBaseData::StoryItemsCollectedStatus)),
                    static_cast<int32_t>(offsetof(SDK::FCrAbandonBaseData, StoryItemsCollectedStatus)) },
                { "FCrAbandonBaseDataCollectedStatus::Item", "FCrAbandonBaseDataCollectedStatus",
                    "Item", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::FCrAbandonBaseDataCollectedStatus::Item)),
                    static_cast<int32_t>(offsetof(SDK::FCrAbandonBaseDataCollectedStatus, Item)) },
                { "FCrAbandonBaseDataCollectedStatus::Status", "FCrAbandonBaseDataCollectedStatus",
                    "Status", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::FCrAbandonBaseDataCollectedStatus::Status)),
                    static_cast<int32_t>(offsetof(SDK::FCrAbandonBaseDataCollectedStatus, Status)) }
            };
            static PluginFieldSpec sizes[] = {
                SR_SIZE(FCrAbandonBaseData),
                SR_SIZE(FCrAbandonBaseDataCollectedStatus)
            };
            const bool fieldsResolved = ResolveFields(scanner, self, fields, std::size(fields));
            const bool sizesResolved = ResolveFields(scanner, self, sizes, std::size(sizes));
            if (!fieldsResolved || !sizesResolved ||
                !MemberFitsSize(fields[0], sizes[0]) || !MemberFitsSize(fields[1], sizes[0]) ||
                !MemberFitsSize(fields[2], sizes[1]) || !MemberFitsSize(fields[3], sizes[1]))
                return false;
            MiniMapFoundables::ArrayElementContracts contracts;
            contracts.AbandonBaseGuid = fields[0].offset;
            contracts.AbandonBaseGuidSize = fields[0].resolvedSize;
            contracts.AbandonBaseStatuses = fields[1].offset;
            contracts.AbandonBaseSize = sizes[0].resolvedSize;
            contracts.CollectedStatusGuid = fields[2].offset;
            contracts.CollectedStatusValue = fields[3].offset;
            contracts.CollectedStatusSize = sizes[1].resolvedSize;
            MiniMapFoundables::SetStoryArrayContracts(contracts);
            return true;
        }

        /** Resolves the directly consumed POI marker fields. */
        static bool ResolvePoiActorFields(IPluginHookScanner* scanner, const IPluginSelf* self)
        {
            static PluginFieldSpec fields[] = {
                { "ACrPointOfInterestMarkerActor::PointOfInterestState", "ACrPointOfInterestMarkerActor",
                    "PointOfInterestState", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::ACrPointOfInterestMarkerActor::PointOfInterestState)),
                    static_cast<int32_t>(offsetof(SDK::ACrPointOfInterestMarkerActor, PointOfInterestState)) },
                { "ACrPointOfInterestMarkerActor::PointOfInterestType", "ACrPointOfInterestMarkerActor",
                    "PointOfInterestType", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::ACrPointOfInterestMarkerActor::PointOfInterestType)),
                    static_cast<int32_t>(offsetof(SDK::ACrPointOfInterestMarkerActor, PointOfInterestType)) },
                { "ACrPointOfInterestMarkerActor::Guid", "ACrPointOfInterestMarkerActor", "Guid",
                    PLUGIN_FIELD_FLAG_NONE, sizeof(decltype(SDK::ACrPointOfInterestMarkerActor::Guid)),
                    static_cast<int32_t>(offsetof(SDK::ACrPointOfInterestMarkerActor, Guid)) },
                { "ACrPointOfInterestMarkerActor::AbandonBaseStoryItemsAssigned",
                    "ACrPointOfInterestMarkerActor", "AbandonBaseStoryItemsAssigned", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::ACrPointOfInterestMarkerActor::AbandonBaseStoryItemsAssigned)),
                    static_cast<int32_t>(offsetof(
                        SDK::ACrPointOfInterestMarkerActor, AbandonBaseStoryItemsAssigned)) }
            };
            const bool resolved = ResolveFields(scanner, self, fields, std::size(fields));
            g_poiFieldOffsets.MarkerState = fields[0].offset;
            g_poiFieldOffsets.MarkerType = fields[1].offset;
            g_poiFieldOffsets.MarkerGuid = fields[2].offset;
            g_foundablesFieldOffsets.PoiMarkerType = fields[1].offset;
            g_foundablesFieldOffsets.PoiMarkerGuid = fields[2].offset;
            g_foundablesFieldOffsets.PoiStoryItemsAssigned = fields[3].offset;
            return resolved;
        }

        /** Resolves the consumed category metadata, icon variants, and story actor GUIDs. */
        static bool ResolveCategoryFields(IPluginHookScanner* scanner, const IPluginSelf* self)
        {
            static PluginFieldSpec fields[] = {
                { "ACrDatapadBase::Guid", "ACrDatapadBase", "Guid", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::ACrDatapadBase::Guid)),
                    static_cast<int32_t>(offsetof(SDK::ACrDatapadBase, Guid)) },
                { "ACrAudiologBase::Guid", "ACrAudiologBase", "Guid", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::ACrAudiologBase::Guid)),
                    static_cast<int32_t>(offsetof(SDK::ACrAudiologBase, Guid)) },
                { "UCrMapMenuCategoryData::Icon", "UCrMapMenuCategoryData", "Icon",
                    PLUGIN_FIELD_FLAG_NONE, sizeof(decltype(SDK::UCrMapMenuCategoryData::Icon)),
                    static_cast<int32_t>(offsetof(SDK::UCrMapMenuCategoryData, Icon)) },
                { "UCrMapMenuCategoryData::CategoryBlocked", "UCrMapMenuCategoryData",
                    "CategoryBlocked", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::UCrMapMenuCategoryData::CategoryBlocked)),
                    static_cast<int32_t>(offsetof(SDK::UCrMapMenuCategoryData, CategoryBlocked)) },
                { "UCrMapMenuCategoryData::LegendFilter", "UCrMapMenuCategoryData", "LegendFilter",
                    PLUGIN_FIELD_FLAG_NONE, sizeof(decltype(SDK::UCrMapMenuCategoryData::LegendFilter)),
                    static_cast<int32_t>(offsetof(SDK::UCrMapMenuCategoryData, LegendFilter)) },
                { "UCrMapMenuCategoryData::CanBeHiddenByFogOfWar", "UCrMapMenuCategoryData",
                    "CanBeHiddenByFogOfWar", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::UCrMapMenuCategoryData::CanBeHiddenByFogOfWar)),
                    static_cast<int32_t>(offsetof(SDK::UCrMapMenuCategoryData, CanBeHiddenByFogOfWar)) },
                { "UCrMapMenuPOIData::UnknownIcon", "UCrMapMenuPOIData", "UnknownIcon",
                    PLUGIN_FIELD_FLAG_NONE, sizeof(decltype(SDK::UCrMapMenuPOIData::UnknownIcon)),
                    static_cast<int32_t>(offsetof(SDK::UCrMapMenuPOIData, UnknownIcon)) },
                { "UCrMapMenuPOIData::AbandonBaseCompletedIcon", "UCrMapMenuPOIData",
                    "AbandonBaseCompletedIcon", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::UCrMapMenuPOIData::AbandonBaseCompletedIcon)),
                    static_cast<int32_t>(offsetof(SDK::UCrMapMenuPOIData, AbandonBaseCompletedIcon)) }
            };
            const bool resolved = ResolveFields(scanner, self, fields, std::size(fields));
            g_foundablesFieldOffsets.CategoryIcon = fields[2].offset;
            g_foundablesFieldOffsets.CategoryBlocked = fields[3].offset;
            g_foundablesFieldOffsets.CategoryLegendFilter = fields[4].offset;
            g_foundablesFieldOffsets.DatapadGuid = fields[0].offset;
            g_foundablesFieldOffsets.AudiologGuid = fields[1].offset;
            g_poiFieldOffsets.CategoryIcon = fields[2].offset;
            g_poiFieldOffsets.CategoryLegendFilter = fields[4].offset;
            g_poiFieldOffsets.CategoryCanBeHiddenByFog = fields[5].offset;
            g_poiFieldOffsets.PoiUnknownIcon = fields[6].offset;
            g_poiFieldOffsets.PoiAbandonBaseCompletedIcon = fields[7].offset;
            return resolved;
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
                    static_cast<int32_t>(offsetof(SDK::UStruct, SuperStruct)) },
                { "UObject::InternalIndex", "UObject", "InternalIndex",
                    PLUGIN_FIELD_NATIVE | PLUGIN_FIELD_CHECK,
                    sizeof(decltype(SDK::UObject::Index)),
                    static_cast<int32_t>(offsetof(SDK::UObject, Index)) }
            };
            if (!ResolveFields(scanner, self, fields, std::size(fields)))
                return false;
            g_playerFieldOffsets.ActorRootComponent = fields[0].offset;
            g_playerFieldOffsets.LevelOwningWorld = fields[1].offset;
            g_playerFieldOffsets.ObjectOuter = fields[3].offset;
            g_playerFieldOffsets.ObjectIndex = fields[5].offset;
            g_poiFieldOffsets.ActorRootComponent = fields[0].offset;
            g_foundablesFieldOffsets.ActorRootComponent = fields[0].offset;
            g_foundablesFieldOffsets.LevelOwningWorld = fields[1].offset;
            g_foundablesFieldOffsets.ObjectFlags = fields[2].offset;
            g_foundablesFieldOffsets.ObjectOuter = fields[3].offset;
            return true;
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
            g_actorClassSize.offset = -1;
            g_actorClassSize.resolvedSize = 0;
            if (!PluginMap::ResolveField(scanner, self, g_actorBeingDestroyedField))
            {
                return false;
            }
            if (!PluginMap::ResolveField(scanner, self, g_actorClassSize))
            {
                return false;
            }
            const uint64_t fieldEnd = static_cast<uint64_t>(g_actorBeingDestroyedField.offset) +
                g_actorBeingDestroyedField.resolvedSize;
            if (g_actorBeingDestroyedField.offset < 0 ||
                g_actorBeingDestroyedField.resolvedSize != sizeof(uint8_t) ||
                g_actorBeingDestroyedField.boolMask == 0 ||
                (g_actorBeingDestroyedField.boolMask & (g_actorBeingDestroyedField.boolMask - 1)) != 0 ||
                fieldEnd > g_actorClassSize.resolvedSize)
            {
                requiredScanner.Fail(
                    "AActor::bActorIsBeingDestroyed",
                    "resolved destruction bit is missing or outside the running AActor layout");
                return false;
            }
            g_playerFieldOffsets.ActorBeingDestroyed = g_actorBeingDestroyedField.offset;
            g_playerFieldOffsets.ActorBeingDestroyedMask = g_actorBeingDestroyedField.boolMask;
            g_foundablesFieldOffsets.ActorBeingDestroyed = g_actorBeingDestroyedField.offset;
            g_foundablesFieldOffsets.ActorBeingDestroyedMask = g_actorBeingDestroyedField.boolMask;
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
            bool allResolved = ResolvePlayerFields(scanner, self, requiredScanner);
            const bool srgbResolved = ResolveTextureSRGB(scanner, self, requiredScanner);
            return allResolved && srgbResolved;
        }

    private:
        /** Resolves every reflected player/controller property consumed by Map.cpp. */
        static bool ResolvePlayerFields(
            IPluginHookScanner* scanner,
            const IPluginSelf* self,
            MiniMap::Adapters::AlienX::RequiredScanner& requiredScanner)
        {
            static PluginFieldSpec fields[] = {
                { "ACrPlayerControllerBase::PlayerMainCharacter", "ACrPlayerControllerBase",
                    "PlayerMainCharacter", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::ACrPlayerControllerBase::PlayerMainCharacter)),
                    -1 },
                { "ACrCharacterPlayerBase::bDead", "ACrCharacterPlayerBase", "bDead",
                    PLUGIN_FIELD_FLAG_NONE, sizeof(decltype(SDK::ACrCharacterPlayerBase::bDead)), -1 },
                { "ACrCharacterPlayerBase::BuildingDrone", "ACrCharacterPlayerBase", "BuildingDrone",
                    PLUGIN_FIELD_FLAG_NONE, sizeof(decltype(SDK::ACrCharacterPlayerBase::BuildingDrone)), -1 },
                { "ACrCharacterDroneBase::OwnerPlayer", "ACrCharacterDroneBase", "OwnerPlayer",
                    PLUGIN_FIELD_FLAG_NONE, sizeof(decltype(SDK::ACrCharacterDroneBase::OwnerPlayer)), -1 },
                { "APlayerController::bIsLocalPlayerController", "APlayerController",
                    "bIsLocalPlayerController", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::APlayerController::bIsLocalPlayerController)), -1 },
                { "AController::Pawn", "AController", "Pawn", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::AController::Pawn)), -1 },
                { "APawn::Controller", "APawn", "Controller", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::APawn::Controller)), -1 },
                { "ACrCharacterPlayerBase::Status", "ACrCharacterPlayerBase", "Status",
                    PLUGIN_FIELD_FLAG_NONE, sizeof(decltype(SDK::ACrCharacterPlayerBase::Status)), -1 }
            };

            static PluginFieldSpec classSizes[] = {
                SR_SIZE(ACrPlayerControllerBase),
                SR_SIZE(ACrCharacterPlayerBase),
                SR_SIZE(ACrCharacterDroneBase),
                SR_SIZE(APlayerController),
                SR_SIZE(AController),
                SR_SIZE(APawn)
            };

            constexpr size_t fieldOwners[] = { 0, 1, 1, 2, 3, 4, 5, 1 };

            bool allResolved = true;
            for (size_t index = 0; index < std::size(fields); ++index)
            {
                fields[index].offset = -1;
                fields[index].resolvedSize = 0;
                allResolved = PluginMap::ResolveField(scanner, self, fields[index]) && allResolved;
            }
            for (size_t index = 0; index < std::size(classSizes); ++index)
            {
                if (index == 1)
                {
                    classSizes[index] = g_playerCharacterClassSize;
                    continue;
                }
                PluginFieldSpec& classSize = classSizes[index];
                classSize.offset = -1;
                classSize.resolvedSize = 0;
                allResolved = PluginMap::ResolveField(scanner, self, classSize) && allResolved;
            }
            if (!allResolved)
                return false;

            constexpr size_t expectedAlignments[] = {
                alignof(void*), 1, alignof(void*), alignof(void*), 1, alignof(void*),
                alignof(void*), alignof(SDK::EPlayerCharacterStatus)
            };
            bool fieldsFit = true;
            for (size_t index = 0; index < std::size(fields); ++index)
            {
                const PluginFieldSpec& field = fields[index];
                const uint32_t ownerSize = classSizes[fieldOwners[index]].resolvedSize;
                const uint64_t fieldEnd = static_cast<uint64_t>(field.offset) + field.resolvedSize;
                fieldsFit = field.offset >= 0 && field.resolvedSize == field.size &&
                    fieldEnd <= ownerSize && field.offset % expectedAlignments[index] == 0 && fieldsFit;
            }
            if (!fieldsFit)
            {
                requiredScanner.Fail(
                    "MiniMap::PlayerTrackingFields",
                    "resolved player tracking field extent or alignment is invalid");
                return false;
            }

            g_playerFieldOffsets.PlayerMainCharacter = fields[0].offset;
            g_playerFieldOffsets.CharacterDead = fields[1].offset;
            g_playerFieldOffsets.BuildingDrone = fields[2].offset;
            g_playerFieldOffsets.DroneOwnerPlayer = fields[3].offset;
            g_playerFieldOffsets.IsLocalPlayerController = fields[4].offset;
            g_playerFieldOffsets.ControllerPawn = fields[5].offset;
            g_playerFieldOffsets.PawnController = fields[6].offset;
            g_playerFieldOffsets.CharacterStatus = fields[7].offset;
            MiniMapMap::PlayerFieldAccessors::SetOffsets(g_playerFieldOffsets);
            return true;
        }

        /** Resolves and validates the mapped UTexture bitfield before publishing it. */
        static bool ResolveTextureSRGB(
            IPluginHookScanner* scanner,
            const IPluginSelf* self,
            MiniMap::Adapters::AlienX::RequiredScanner& requiredScanner)
        {
            g_textureSRGBField.offset = -1;
            g_textureSRGBField.resolvedSize = 0;
            g_textureSRGBField.boolMask = 0;
            g_textureClassSize.offset = -1;
            g_textureClassSize.resolvedSize = 0;
            const bool srgbResolved = PluginMap::ResolveField(scanner, self, g_textureSRGBField);
            const bool textureSizeResolved = PluginMap::ResolveField(scanner, self, g_textureClassSize);
            if (!srgbResolved || !textureSizeResolved)
            {
                return false;
            }

            const uint64_t fieldEnd = static_cast<uint64_t>(g_textureSRGBField.offset) +
                g_textureSRGBField.resolvedSize;
            if (g_textureSRGBField.offset < 0 ||
                g_textureSRGBField.resolvedSize != sizeof(uint8_t) || g_textureSRGBField.boolMask == 0 ||
                (g_textureSRGBField.boolMask &
                    static_cast<uint8_t>(g_textureSRGBField.boolMask - 1)) != 0 ||
                g_textureClassSize.resolvedSize == 0 || fieldEnd > g_textureClassSize.resolvedSize)
            {
                requiredScanner.Fail(
                    "UTexture::SRGB",
                    "resolved SRGB bit is missing or outside the running UTexture layout");
                return false;
            }
            if (!MiniMapNative::PublishTextureSRGBField(
                g_textureSRGBField.offset,
                g_textureSRGBField.resolvedSize,
                g_textureSRGBField.boolMask))
            {
                requiredScanner.Fail(
                    "UTexture::SRGB",
                    "resolved SRGB bit could not be published to the native API");
                return false;
            }
            return true;
        }
    };

    /** Resolves and validates the reflected fields written by sprite capture. */
    class ClientCaptureFieldContract
    {
        using CaptureOffsets = Services::Map::Buildings::Capture::CaptureFieldOffsets;

        struct OwnerSize
        {
            const char* Name;
            int32_t Size;
        };

        /** Resolves runtime owner extents used to bound every capture field. */
        static bool ResolveOwnerSizes(
            IPluginHookScanner* scanner,
            const IPluginSelf* self,
            MiniMap::Adapters::AlienX::RequiredScanner& requiredScanner,
            std::array<OwnerSize, 10>& owners)
        {
            static PluginFieldSpec fields[] = {
                SR_SIZE(UActorComponent), SR_SIZE(USceneComponent), SR_SIZE(ULightComponent),
                SR_SIZE(ULightComponentBase), SR_SIZE(USceneCaptureComponent),
                SR_SIZE(USceneCaptureComponent2D), SR_SIZE(UTextureRenderTarget),
                SR_SIZE(UTextureRenderTarget2D), SR_SIZE(UDirectionalLightComponent),
                SR_SIZE(FTickFunction)
            };
            static const char* names[] = {
                "UActorComponent", "USceneComponent", "ULightComponent", "ULightComponentBase",
                "USceneCaptureComponent", "USceneCaptureComponent2D", "UTextureRenderTarget",
                "UTextureRenderTarget2D", "UDirectionalLightComponent", "FTickFunction"
            };
            bool resolved = true;
            for (size_t index = 0; index < std::size(fields); ++index)
            {
                fields[index].offset = -1;
                fields[index].resolvedSize = 0;
                if (!PluginMap::ResolveField(scanner, self, fields[index]) || fields[index].resolvedSize == 0 ||
                    fields[index].resolvedSize > 0x7fffffffU)
                {
                    requiredScanner.Fail(names[index], "runtime capture owner size is missing or invalid");
                    resolved = false;
                    continue;
                }
                owners[index] = { names[index], static_cast<int32_t>(fields[index].resolvedSize) };
            }
            return resolved;
        }

        /** Returns the runtime reflected extent for one declaring owner. */
        static int32_t FindOwnerSize(const std::array<OwnerSize, 10>& owners, const char* name)
        {
            for (const OwnerSize& owner : owners)
            {
                if (std::strcmp(owner.Name, name) == 0)
                {
                    return owner.Size;
                }
            }
            return 0;
        }

        /** Resolves one required set of ordinary reflected capture properties. */
        static bool ResolvePropertySet(
            IPluginHookScanner* scanner,
            const IPluginSelf* self,
            MiniMap::Adapters::AlienX::RequiredScanner& requiredScanner,
            const std::array<OwnerSize, 10>& owners,
            CaptureOffsets& offsets,
            PluginFieldSpec* fields,
            size_t fieldCount,
            size_t firstProperty)
        {
            bool allResolved = true;
            for (size_t index = 0; index < fieldCount; ++index)
            {
                fields[index].offset = -1;
                fields[index].resolvedSize = 0;
                const bool resolved = PluginMap::ResolveField(scanner, self, fields[index]);
                const int32_t ownerSize = FindOwnerSize(owners, fields[index].className);
                uint32_t alignment = fields[index].size >= 4 ? alignof(uint32_t) : 1;
                const size_t propertyIndex = firstProperty + index;
                if (propertyIndex == static_cast<size_t>(CaptureOffsets::Property::PrimaryComponentTick))
                {
                    alignment = alignof(SDK::FActorComponentTickFunction);
                }
                else if (propertyIndex == static_cast<size_t>(CaptureOffsets::Property::TextureTarget))
                {
                    alignment = alignof(SDK::UTextureRenderTarget2D*);
                }
                if (!resolved || ownerSize <= 0 || fields[index].offset < 0 ||
                    fields[index].resolvedSize != fields[index].size ||
                    static_cast<uint64_t>(fields[index].offset) + fields[index].resolvedSize >
                        static_cast<uint32_t>(ownerSize) ||
                    (alignment > 1 && fields[index].offset % static_cast<int32_t>(alignment) != 0))
                {
                    requiredScanner.Fail(fields[index].key,
                        "capture property is missing, mis-sized, misaligned, or outside its runtime owner");
                    allResolved = false;
                    continue;
                }
                offsets.Properties[firstProperty + index] = fields[index].offset;
            }
            return allResolved;
        }

        /** Resolves component and base-capture properties using their declaring owners. */
        static bool ResolveBaseProperties(IPluginHookScanner* scanner, const IPluginSelf* self,
            MiniMap::Adapters::AlienX::RequiredScanner& requiredScanner,
            const std::array<OwnerSize, 10>& owners,
            CaptureOffsets& offsets)
        {
            static PluginFieldSpec fields[] = {
                { "UActorComponent::PrimaryComponentTick", "UActorComponent", "PrimaryComponentTick",
                    PLUGIN_FIELD_FLAG_NONE, sizeof(SDK::FActorComponentTickFunction), 0x38 },
                { "USceneComponent::Mobility", "USceneComponent", "Mobility",
                    PLUGIN_FIELD_FLAG_NONE, sizeof(SDK::EComponentMobility), 0x1A3 },
                { "ULightComponentBase::Intensity", "ULightComponentBase", "Intensity",
                    PLUGIN_FIELD_FLAG_NONE, sizeof(float), 0x274 },
                { "ULightComponentBase::LightColor", "ULightComponentBase", "LightColor",
                    PLUGIN_FIELD_FLAG_NONE, sizeof(SDK::FColor), 0x278 },
                { "USceneCaptureComponent::PrimitiveRenderMode", "USceneCaptureComponent", "PrimitiveRenderMode",
                    PLUGIN_FIELD_FLAG_NONE, sizeof(SDK::ESceneCapturePrimitiveRenderMode), 0x250 },
                { "USceneCaptureComponent::CaptureSource", "USceneCaptureComponent", "CaptureSource",
                    PLUGIN_FIELD_FLAG_NONE, sizeof(SDK::ESceneCaptureSource), 0x251 },
                { "USceneCaptureComponent::bAlwaysPersistRenderingState", "USceneCaptureComponent",
                    "bAlwaysPersistRenderingState", PLUGIN_FIELD_FLAG_NONE, sizeof(bool), 0x253 },
                { "USceneCaptureComponent::bUseRayTracingIfEnabled", "USceneCaptureComponent",
                    "bUseRayTracingIfEnabled", PLUGIN_FIELD_FLAG_NONE, sizeof(bool), 0x2A4 }
            };
            return ResolvePropertySet(scanner, self, requiredScanner, owners, offsets,
                fields, std::size(fields), 0);
        }

        /** Resolves 2D capture properties using the declaring 2D component type. */
        static bool ResolveCapture2DProperties(IPluginHookScanner* scanner, const IPluginSelf* self,
            MiniMap::Adapters::AlienX::RequiredScanner& requiredScanner,
            const std::array<OwnerSize, 10>& owners,
            CaptureOffsets& offsets)
        {
            static PluginFieldSpec fields[] = {
                { "USceneCaptureComponent2D::ProjectionType", "USceneCaptureComponent2D", "ProjectionType",
                    PLUGIN_FIELD_FLAG_NONE, sizeof(SDK::ECameraProjectionMode), 0x338 },
                { "USceneCaptureComponent2D::OrthoWidth", "USceneCaptureComponent2D", "OrthoWidth",
                    PLUGIN_FIELD_FLAG_NONE, sizeof(float), 0x348 },
                { "USceneCaptureComponent2D::bAutoCalculateOrthoPlanes", "USceneCaptureComponent2D",
                    "bAutoCalculateOrthoPlanes", PLUGIN_FIELD_FLAG_NONE, sizeof(bool), 0x34C },
                { "USceneCaptureComponent2D::bUpdateOrthoPlanes", "USceneCaptureComponent2D",
                    "bUpdateOrthoPlanes", PLUGIN_FIELD_FLAG_NONE, sizeof(bool), 0x354 },
                { "USceneCaptureComponent2D::bUseCameraHeightAsViewTarget", "USceneCaptureComponent2D",
                    "bUseCameraHeightAsViewTarget", PLUGIN_FIELD_FLAG_NONE, sizeof(bool), 0x355 },
                { "USceneCaptureComponent2D::TextureTarget", "USceneCaptureComponent2D", "TextureTarget",
                    PLUGIN_FIELD_FLAG_NONE, sizeof(void*), 0x360 },
                { "USceneCaptureComponent2D::CompositeMode", "USceneCaptureComponent2D", "CompositeMode",
                    PLUGIN_FIELD_FLAG_NONE, sizeof(SDK::ESceneCaptureCompositeMode), 0x368 },
                { "USceneCaptureComponent2D::PostProcessBlendWeight", "USceneCaptureComponent2D",
                    "PostProcessBlendWeight", PLUGIN_FIELD_FLAG_NONE, sizeof(float), 0xAD0 },
                { "USceneCaptureComponent2D::bUseCustomProjectionMatrix", "USceneCaptureComponent2D",
                    "bUseCustomProjectionMatrix", PLUGIN_FIELD_FLAG_NONE, sizeof(bool), 0xADC },
                { "USceneCaptureComponent2D::bEnableOrthographicTiling", "USceneCaptureComponent2D",
                    "bEnableOrthographicTiling", PLUGIN_FIELD_FLAG_NONE, sizeof(bool), 0xB61 },
                { "USceneCaptureComponent2D::bEnableClipPlane", "USceneCaptureComponent2D",
                    "bEnableClipPlane", PLUGIN_FIELD_FLAG_NONE, sizeof(bool), 0xB6C },
                { "USceneCaptureComponent2D::bRenderInMainRenderer", "USceneCaptureComponent2D",
                    "bRenderInMainRenderer", PLUGIN_FIELD_FLAG_NONE, sizeof(bool), 0xBA0 }
            };
            return ResolvePropertySet(scanner, self, requiredScanner, owners, offsets,
                fields, std::size(fields), 8);
        }

        /** Resolves directly written base and 2D render-target properties. */
        static bool ResolveRenderTargetProperties(IPluginHookScanner* scanner, const IPluginSelf* self,
            MiniMap::Adapters::AlienX::RequiredScanner& requiredScanner,
            const std::array<OwnerSize, 10>& owners,
            CaptureOffsets& offsets)
        {
            static PluginFieldSpec fields[] = {
                { "UTextureRenderTarget::TargetGamma", "UTextureRenderTarget", "TargetGamma",
                    PLUGIN_FIELD_FLAG_NONE, sizeof(float), 0x140 },
                { "UTextureRenderTarget2D::ClearColor", "UTextureRenderTarget2D", "ClearColor",
                    PLUGIN_FIELD_FLAG_NONE, sizeof(SDK::FLinearColor), 0x150 }
            };
            return ResolvePropertySet(scanner, self, requiredScanner, owners, offsets,
                fields, std::size(fields), 20);
        }

        /** Resolves directly written directional-light properties at their compiled offsets. */
        static bool ResolveDirectionalLightProperties(IPluginHookScanner* scanner, const IPluginSelf* self,
            MiniMap::Adapters::AlienX::RequiredScanner& requiredScanner,
            const std::array<OwnerSize, 10>& owners,
            CaptureOffsets& offsets)
        {
            static PluginFieldSpec fields[] = {
                { "UDirectionalLightComponent::ForwardShadingPriority", "UDirectionalLightComponent",
                    "ForwardShadingPriority", PLUGIN_FIELD_FLAG_NONE, sizeof(int32_t), 0x3FC },
                { "UDirectionalLightComponent::LightSourceAngle", "UDirectionalLightComponent",
                    "LightSourceAngle", PLUGIN_FIELD_FLAG_NONE, sizeof(float), 0x400 }
            };
            return ResolvePropertySet(scanner, self, requiredScanner, owners, offsets,
                fields, std::size(fields), 22);
        }

        /** Resolves every ordinary capture property set. */
        static bool ResolveProperties(IPluginHookScanner* scanner, const IPluginSelf* self,
            MiniMap::Adapters::AlienX::RequiredScanner& requiredScanner,
            const std::array<OwnerSize, 10>& owners,
            CaptureOffsets& offsets)
        {
            const bool baseResolved = ResolveBaseProperties(scanner, self, requiredScanner, owners, offsets);
            const bool capture2DResolved = ResolveCapture2DProperties(scanner, self, requiredScanner, owners, offsets);
            const bool targetResolved = ResolveRenderTargetProperties(scanner, self, requiredScanner, owners, offsets);
            const bool lightResolved = ResolveDirectionalLightProperties(scanner, self, requiredScanner, owners, offsets);
            return baseResolved && capture2DResolved && targetResolved && lightResolved;
        }

        /** Resolves one bitfield set and validates its runtime extent and mask. */
        static bool ResolveBitFieldSet(
            IPluginHookScanner* scanner,
            const IPluginSelf* self,
            MiniMap::Adapters::AlienX::RequiredScanner& requiredScanner,
            const std::array<OwnerSize, 10>& owners,
            CaptureOffsets& offsets,
            PluginBitField* fields,
            size_t fieldCount,
            size_t firstBitField)
        {
            bool allResolved = true;
            for (size_t index = 0; index < fieldCount; ++index)
            {
                PluginBitField& field = fields[index];
                field.offset = -1;
                field.resolvedSize = 0;
                field.boolMask = 0;
                const bool resolved = PluginMap::ResolveField(scanner, self, field);
                const int32_t ownerSize = FindOwnerSize(owners, field.className);
                if (!resolved || ownerSize <= 0 || field.offset < 0 || field.resolvedSize != 1 ||
                    field.boolMask == 0 || (field.boolMask & (field.boolMask - 1)) != 0 ||
                    static_cast<uint64_t>(field.offset) + 1 > static_cast<uint32_t>(ownerSize))
                {
                    requiredScanner.Fail(field.key,
                        "capture bitfield is missing, invalid, or outside its runtime owner");
                    allResolved = false;
                    continue;
                }
                offsets.BitFields[firstBitField + index] = { field.offset, field.boolMask };
            }
            return allResolved;
        }

        /** Resolves tick and actor-component packed fields. */
        static bool ResolveComponentBitFields(
            IPluginHookScanner* scanner,
            const IPluginSelf* self,
            MiniMap::Adapters::AlienX::RequiredScanner& requiredScanner,
            const std::array<OwnerSize, 10>& owners,
            CaptureOffsets& offsets)
        {
            static PluginBitField fields[] = {
                { { "FTickFunction::bCanEverTick", "FTickFunction", "bCanEverTick",
                    PLUGIN_FIELD_BITFIELD, 1, -1 } },
                { { "UActorComponent::bReplicates", "UActorComponent", "bReplicates",
                    PLUGIN_FIELD_BITFIELD, 1, -1 } },
                { { "UActorComponent::bAutoActivate", "UActorComponent", "bAutoActivate",
                    PLUGIN_FIELD_BITFIELD, 1, -1 } },
                { { "UActorComponent::bCanEverAffectNavigation", "UActorComponent",
                    "bCanEverAffectNavigation", PLUGIN_FIELD_BITFIELD, 1, -1 } },
                { { "ULightComponent::bUseTemperature", "ULightComponent", "bUseTemperature",
                    PLUGIN_FIELD_BITFIELD, 1, -1 } },
                { { "ULightComponentBase::bAffectsWorld", "ULightComponentBase", "bAffectsWorld",
                    PLUGIN_FIELD_BITFIELD, 1, -1 } },
                { { "ULightComponentBase::CastShadows", "ULightComponentBase", "CastShadows",
                    PLUGIN_FIELD_BITFIELD, 1, -1 } },
                { { "ULightComponentBase::CastStaticShadows", "ULightComponentBase", "CastStaticShadows",
                    PLUGIN_FIELD_BITFIELD, 1, -1 } },
                { { "ULightComponentBase::CastDynamicShadows", "ULightComponentBase", "CastDynamicShadows",
                    PLUGIN_FIELD_BITFIELD, 1, -1 } },
                { { "ULightComponentBase::bAffectTranslucentLighting", "ULightComponentBase",
                    "bAffectTranslucentLighting", PLUGIN_FIELD_BITFIELD, 1, -1 } }
            };
            return ResolveBitFieldSet(scanner, self, requiredScanner, owners, offsets,
                fields, std::size(fields), 0);
        }

        /** Resolves capture-component packed fields. */
        static bool ResolveCaptureBitFields(
            IPluginHookScanner* scanner,
            const IPluginSelf* self,
            MiniMap::Adapters::AlienX::RequiredScanner& requiredScanner,
            const std::array<OwnerSize, 10>& owners,
            CaptureOffsets& offsets)
        {
            static PluginBitField fields[] = {
                { { "USceneCaptureComponent::bCaptureEveryFrame", "USceneCaptureComponent",
                    "bCaptureEveryFrame", PLUGIN_FIELD_BITFIELD, 1, -1 } },
                { { "USceneCaptureComponent::bCaptureOnMovement", "USceneCaptureComponent",
                    "bCaptureOnMovement", PLUGIN_FIELD_BITFIELD, 1, -1 } },
                { { "USceneCaptureComponent2D::bMainViewFamily", "USceneCaptureComponent2D",
                    "bMainViewFamily", PLUGIN_FIELD_BITFIELD, 1, -1 } },
                { { "USceneCaptureComponent2D::bMainViewResolution", "USceneCaptureComponent2D",
                    "bMainViewResolution", PLUGIN_FIELD_BITFIELD, 1, -1 } },
                { { "USceneCaptureComponent2D::bMainViewCamera", "USceneCaptureComponent2D",
                    "bMainViewCamera", PLUGIN_FIELD_BITFIELD, 1, -1 } },
                { { "USceneCaptureComponent2D::bInheritMainViewCameraPostProcessSettings",
                    "USceneCaptureComponent2D", "bInheritMainViewCameraPostProcessSettings",
                    PLUGIN_FIELD_BITFIELD, 1, -1 } }
            };
            return ResolveBitFieldSet(scanner, self, requiredScanner, owners, offsets,
                fields, std::size(fields), 10);
        }

        /** Resolves render-target packed fields. */
        static bool ResolveTargetBitFields(
            IPluginHookScanner* scanner,
            const IPluginSelf* self,
            MiniMap::Adapters::AlienX::RequiredScanner& requiredScanner,
            const std::array<OwnerSize, 10>& owners,
            CaptureOffsets& offsets)
        {
            static PluginBitField fields[] = {
                { { "UTextureRenderTarget2D::bSupportsUAV", "UTextureRenderTarget2D", "bSupportsUAV",
                    PLUGIN_FIELD_BITFIELD, 1, -1 } },
                { { "UTextureRenderTarget2D::bAutoGenerateMips", "UTextureRenderTarget2D",
                    "bAutoGenerateMips", PLUGIN_FIELD_BITFIELD, 1, -1 } }
            };
            return ResolveBitFieldSet(scanner, self, requiredScanner, owners, offsets,
                fields, std::size(fields), 16);
        }

        /** Resolves directly written directional-light bitfields and their masks. */
        static bool ResolveDirectionalLightBitFields(
            IPluginHookScanner* scanner,
            const IPluginSelf* self,
            MiniMap::Adapters::AlienX::RequiredScanner& requiredScanner,
            const std::array<OwnerSize, 10>& owners,
            CaptureOffsets& offsets)
        {
            static PluginBitField fields[] = {
                { { "UDirectionalLightComponent::bAtmosphereSunLight", "UDirectionalLightComponent",
                    "bAtmosphereSunLight", PLUGIN_FIELD_BITFIELD, 1, -1 } },
                { { "UDirectionalLightComponent::bPerPixelAtmosphereTransmittance",
                    "UDirectionalLightComponent", "bPerPixelAtmosphereTransmittance",
                    PLUGIN_FIELD_BITFIELD, 1, -1 } },
                { { "UDirectionalLightComponent::bCastShadowsOnClouds", "UDirectionalLightComponent",
                    "bCastShadowsOnClouds", PLUGIN_FIELD_BITFIELD, 1, -1 } },
                { { "UDirectionalLightComponent::bCastShadowsOnAtmosphere", "UDirectionalLightComponent",
                    "bCastShadowsOnAtmosphere", PLUGIN_FIELD_BITFIELD, 1, -1 } },
                { { "UDirectionalLightComponent::bCastCloudShadows", "UDirectionalLightComponent",
                    "bCastCloudShadows", PLUGIN_FIELD_BITFIELD, 1, -1 } }
            };
            return ResolveBitFieldSet(scanner, self, requiredScanner, owners, offsets,
                fields, std::size(fields), 18);
        }

        /** Requires the live SetShowFlagSettings parameter block and array argument to match the SDK ABI. */
        static bool ResolveShowFlagParameters(
            IPluginHookScanner* scanner,
            const IPluginSelf* self,
            MiniMap::Adapters::AlienX::RequiredScanner& requiredScanner)
        {
            static PluginFieldSpec blockSize{
                "SceneCaptureComponent_SetShowFlagSettings", "SceneCaptureComponent_SetShowFlagSettings",
                nullptr, PLUGIN_FIELD_LATE, sizeof(SDK::Params::SceneCaptureComponent_SetShowFlagSettings), -1 };
            static PluginFieldSpec showFlags{
                "SceneCaptureComponent_SetShowFlagSettings::InShowFlagSettings",
                "SceneCaptureComponent_SetShowFlagSettings", "InShowFlagSettings",
                PLUGIN_FIELD_ANY_SIZE | PLUGIN_FIELD_LATE, 0,
                static_cast<int32_t>(offsetof(
                    SDK::Params::SceneCaptureComponent_SetShowFlagSettings, InShowFlagSettings)) };
            blockSize.offset = -1;
            blockSize.resolvedSize = 0;
            showFlags.offset = -1;
            showFlags.resolvedSize = 0;
            const bool blockResolved = PluginMap::ResolveField(scanner, self, blockSize);
            const bool fieldResolved = PluginMap::ResolveField(scanner, self, showFlags);
            if (!blockResolved || !fieldResolved || blockSize.resolvedSize != 0x10 ||
                showFlags.offset != 0 || showFlags.resolvedSize != 0x10)
            {
                requiredScanner.Fail("SceneCaptureComponent_SetShowFlagSettings",
                    "live parameter block or show-flag array does not match the 0x10-byte SDK parameter ABI");
                return false;
            }
            return true;
        }

        /** Resolves all captured bitfield groups. */
        static bool ResolveBitFields(
            IPluginHookScanner* scanner,
            const IPluginSelf* self,
            MiniMap::Adapters::AlienX::RequiredScanner& requiredScanner,
            const std::array<OwnerSize, 10>& owners,
            CaptureOffsets& offsets)
        {
            const bool componentResolved = ResolveComponentBitFields(scanner, self, requiredScanner, owners, offsets);
            const bool captureResolved = ResolveCaptureBitFields(scanner, self, requiredScanner, owners, offsets);
            const bool targetResolved = ResolveTargetBitFields(scanner, self, requiredScanner, owners, offsets);
            const bool lightResolved = ResolveDirectionalLightBitFields(scanner, self, requiredScanner, owners, offsets);
            return componentResolved && captureResolved && targetResolved && lightResolved;
        }

    public:
        /** Resolves all directly consumed capture properties in the active Client scanner session. */
        static bool Resolve(
            IPluginHookScanner* scanner,
            const IPluginSelf* self,
            MiniMap::Adapters::AlienX::RequiredScanner& requiredScanner)
        {
            CaptureOffsets offsets{};
            std::array<OwnerSize, 10> owners{};
            const bool ownerSizesResolved = ResolveOwnerSizes(scanner, self, requiredScanner, owners);
            if (!ownerSizesResolved)
            {
                return false;
            }
            const bool propertiesResolved = ResolveProperties(
                scanner, self, requiredScanner, owners, offsets);
            const bool bitFieldsResolved = ResolveBitFields(
                scanner, self, requiredScanner, owners, offsets);
            const bool parametersResolved = ResolveShowFlagParameters(scanner, self, requiredScanner);
            if (!propertiesResolved || !bitFieldsResolved || !parametersResolved)
            {
                return false;
            }
            const auto tickBit = offsets.BitFields[static_cast<size_t>(CaptureOffsets::BitField::TickCanEverTick)];
            const int32_t runtimeTickSize = FindOwnerSize(owners, "FTickFunction");
            const int32_t runtimeActorTickSize = static_cast<int32_t>(sizeof(SDK::FActorComponentTickFunction));
            if (tickBit.Offset < 0 || static_cast<uint64_t>(tickBit.Offset) + 1 >
                    static_cast<uint32_t>(runtimeTickSize) ||
                static_cast<uint64_t>(tickBit.Offset) + 1 > static_cast<uint32_t>(runtimeActorTickSize))
            {
                requiredScanner.Fail("FTickFunction::bCanEverTick",
                    "nested tick bitfield is outside the validated runtime tick extent");
                return false;
            }
            offsets.IsValid = true;
            return Services::Map::Buildings::Capture::BuildingSpriteRenderer::
                PublishCaptureFieldOffsets(offsets);
        }
    };


    PluginFieldSpec g_buildingDefinitionFields[] = {
                { "UAuActorPlacementData::BuildingID", "UAuActorPlacementData", "BuildingID",
                    PLUGIN_FIELD_FLAG_NONE, sizeof(decltype(SDK::UAuActorPlacementData::BuildingID)), -1 },
                { "UAuActorPlacementData::ActorClass", "UAuActorPlacementData", "ActorClass",
                    PLUGIN_FIELD_FLAG_NONE, sizeof(decltype(SDK::UAuActorPlacementData::ActorClass)), -1 },
                { "UAuActorPlacementData::EntityType", "UAuActorPlacementData", "EntityType",
                    PLUGIN_FIELD_FLAG_NONE, sizeof(SDK::FAuAPMassSpawnedEntityType), -1 },
                { "FAuAPMassSpawnedEntityType::EntityConfig", "FAuAPMassSpawnedEntityType", "EntityConfig",
                    PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::FAuAPMassSpawnedEntityType::EntityConfig)), -1 },
                { "FAuAPMassSpawnedEntityType::EntityConfigPtr", "FAuAPMassSpawnedEntityType", "EntityConfigPtr",
                    PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::FAuAPMassSpawnedEntityType::EntityConfigPtr)), -1 },
                { "UCrBuildingData::Tags", "UCrBuildingData", "Tags", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::UCrBuildingData::Tags)), -1 },
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
                    "HighResTemplateActor", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::UCrMassRepVisCosmeticsTrait::HighResTemplateActor)), -1 },
                { "UCrMassRepVisCosmeticsTrait::LowResTemplateActor", "UCrMassRepVisCosmeticsTrait",
                    "LowResTemplateActor", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::UCrMassRepVisCosmeticsTrait::LowResTemplateActor)), -1 },
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
                    PLUGIN_FIELD_FLAG_NONE, sizeof(decltype(SDK::ACrModularRampWalkway::SplineMesh)), -1 },
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
        /** Verifies a resolved member lies inside its runtime-sized array element. */
        static bool MemberFitsSize(const PluginFieldSpec& member, const PluginFieldSpec& size)
        {
            return member.offset >= 0 && member.resolvedSize != 0 && size.resolvedSize != 0 &&
                uint64_t(member.offset) + member.resolvedSize <= size.resolvedSize;
        }

        /** Resolves a field group while the loader scanner session is active. */
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

        /** Validates one resolved field's width, extent, and typed-access alignment. */
        static bool FieldFitsSize(
            const PluginFieldSpec& field,
            const PluginFieldSpec& ownerSize,
            std::size_t expectedWidth,
            std::size_t requiredAlignment)
        {
            if (field.offset < 0 || field.resolvedSize != expectedWidth ||
                ownerSize.resolvedSize == 0 || requiredAlignment == 0)
                return false;
            const uint64_t fieldEnd = static_cast<uint64_t>(field.offset) + field.resolvedSize;
            return fieldEnd <= ownerSize.resolvedSize &&
                static_cast<std::size_t>(field.offset) % requiredAlignment == 0;
        }

        /** Validates the direct spline-data and point-array fields against their runtime owners. */
        static bool SplineSourceFieldsFit(
            const PluginFieldSpec* fields,
            const PluginFieldSpec* sizes)
        {
            return FieldFitsSize(fields[0], sizes[1], sizeof(SDK::TArray<SDK::FAuAPSplinePointData>),
                    alignof(SDK::TArray<SDK::FAuAPSplinePointData>)) &&
                FieldFitsSize(fields[1], sizes[1], sizeof(SDK::FVector), alignof(SDK::FVector)) &&
                FieldFitsSize(fields[2], sizes[1], sizeof(SDK::FRotator), alignof(SDK::FRotator)) &&
                FieldFitsSize(fields[3], sizes[1], sizeof(float), alignof(float)) &&
                MemberFitsSize(fields[4], sizes[0]) && MemberFitsSize(fields[5], sizes[0]) &&
                MemberFitsSize(fields[6], sizes[0]);
        }

        /** Validates Mass fragment and shared-parameter fields against their runtime owners. */
        static bool SplineMassFieldsFit(
            const PluginFieldSpec* fields,
            const PluginFieldSpec* sizes)
        {
            return FieldFitsSize(fields[0], sizes[0], sizeof(SDK::FAuAPPayloadDataHandle),
                    alignof(SDK::FAuAPPayloadDataHandle)) &&
                FieldFitsSize(fields[1], sizes[1], sizeof(SDK::FAuAPSplineData),
                    alignof(SDK::FAuAPSplineData)) &&
                FieldFitsSize(fields[2], sizes[1], sizeof(SDK::FSplineCurves),
                    alignof(SDK::FSplineCurves)) &&
                FieldFitsSize(fields[3], sizes[1], sizeof(bool), alignof(bool)) &&
                FieldFitsSize(fields[4], sizes[2], sizeof(SDK::UAuActorPlacementData*),
                    alignof(SDK::UAuActorPlacementData*));
        }

        /** Resolves runtime fields within spline data and the existing point-array contract. */
        static bool ResolveSplineDataFields(
            IPluginHookScanner* scanner,
            const IPluginSelf* self,
            MiniMap::BuildingAcquisition::RuntimeFieldContract& contract,
            int32_t& pointIndexOffset,
            int32_t& pointPositionOffset,
            int32_t& pointTangentOffset,
            uint32_t& pointStride)
        {
            static PluginFieldSpec fields[] = {
                { "FAuAPSplineData::SplineData", "FAuAPSplineData", "SplineData", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::FAuAPSplineData::SplineData)),
                    -1 },
                { "FAuAPSplineData::SplineLocationWS", "FAuAPSplineData", "SplineLocationWS",
                    PLUGIN_FIELD_FLAG_NONE, sizeof(decltype(SDK::FAuAPSplineData::SplineLocationWS)), -1 },
                { "FAuAPSplineData::SplineRotationWS", "FAuAPSplineData", "SplineRotationWS",
                    PLUGIN_FIELD_FLAG_NONE, sizeof(decltype(SDK::FAuAPSplineData::SplineRotationWS)), -1 },
                { "FAuAPSplineData::SplineRealLength", "FAuAPSplineData", "SplineRealLength",
                    PLUGIN_FIELD_FLAG_NONE, sizeof(decltype(SDK::FAuAPSplineData::SplineRealLength)), -1 },
                { "FAuAPSplinePointData::Index", "FAuAPSplinePointData", "Index", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::FAuAPSplinePointData::Index)),
                    static_cast<int32_t>(offsetof(SDK::FAuAPSplinePointData, Index)) },
                { "FAuAPSplinePointData::Position", "FAuAPSplinePointData", "Position", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::FAuAPSplinePointData::Position)),
                    static_cast<int32_t>(offsetof(SDK::FAuAPSplinePointData, Position)) },
                { "FAuAPSplinePointData::Tangent", "FAuAPSplinePointData", "Tangent", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::FAuAPSplinePointData::Tangent)),
                    static_cast<int32_t>(offsetof(SDK::FAuAPSplinePointData, Tangent)) },
                { "FAuAPSplineActorPayloadData::Data", "FAuAPSplineActorPayloadData", "Data",
                    PLUGIN_FIELD_CHECK, sizeof(decltype(SDK::FAuAPSplineActorPayloadData::Data)),
                    static_cast<int32_t>(offsetof(SDK::FAuAPSplineActorPayloadData, Data)) }
            };
            static PluginFieldSpec sizes[] = {
                SR_SIZE(FAuAPSplinePointData),
                SR_SIZE(FAuAPSplineData)
            };
            if (!ResolveFields(scanner, self, fields, sizeof(fields) / sizeof(fields[0])) ||
                !ResolveFields(scanner, self, sizes, sizeof(sizes) / sizeof(sizes[0])))
                return false;

            if (!SplineSourceFieldsFit(fields, sizes))
                return false;

            contract.SplineDataPoints = fields[0].offset;
            contract.SplineDataLocation = fields[1].offset;
            contract.SplineDataRotation = fields[2].offset;
            contract.SplineDataRealLength = fields[3].offset;
            pointIndexOffset = fields[4].offset;
            pointPositionOffset = fields[5].offset;
            pointTangentOffset = fields[6].offset;
            pointStride = sizes[0].resolvedSize;
            return true;
        }

        /** Resolves runtime fields within Mass fragments and their shared parameter. */
        static bool ResolveSplineMassFields(
            IPluginHookScanner* scanner,
            const IPluginSelf* self,
            MiniMap::BuildingAcquisition::RuntimeFieldContract& contract)
        {
            static PluginFieldSpec fields[] = {
                { "FAuAPMassFragment::Payload", "FAuAPMassFragment", "Payload", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(SDK::FAuAPPayloadDataHandle), -1 },
                { "FAuSplineConnectionFragment::Data", "FAuSplineConnectionFragment", "Data",
                    PLUGIN_FIELD_FLAG_NONE, sizeof(decltype(SDK::FAuSplineConnectionFragment::Data)), -1 },
                { "FAuSplineConnectionFragment::SplineCurves", "FAuSplineConnectionFragment", "SplineCurves",
                    PLUGIN_FIELD_FLAG_NONE, sizeof(decltype(SDK::FAuSplineConnectionFragment::SplineCurves)), -1 },
                { "FAuSplineConnectionFragment::bSplineCurvesInitialized", "FAuSplineConnectionFragment",
                    "bSplineCurvesInitialized", PLUGIN_FIELD_FLAG_NONE, sizeof(bool), -1 },
                { "FCrBuildingParameters::PlacementData", "FCrBuildingParameters", "PlacementData",
                    PLUGIN_FIELD_FLAG_NONE, sizeof(decltype(SDK::FCrBuildingParameters::PlacementData)), -1 }
            };
            static PluginFieldSpec sizes[] = {
                SR_SIZE(FAuAPMassFragment),
                SR_SIZE(FAuSplineConnectionFragment),
                SR_SIZE(FCrBuildingParameters)
            };
            if (!ResolveFields(scanner, self, fields, sizeof(fields) / sizeof(fields[0])) ||
                !ResolveFields(scanner, self, sizes, sizeof(sizes) / sizeof(sizes[0])))
                return false;

            if (!SplineMassFieldsFit(fields, sizes))
                return false;

            contract.ActorPlacementPayload = fields[0].offset;
            contract.SplineFragmentData = fields[1].offset;
            contract.SplineFragmentCurves = fields[2].offset;
            contract.SplineFragmentCurvesInitialized = fields[3].offset;
            contract.BuildingParametersPlacementData = fields[4].offset;
            return true;
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

        /** Resolves StarRupture spline fields and retains validated engine curve layouts. */
        static bool ResolveSplineFields(IPluginHookScanner* scanner, const IPluginSelf* self)
        {
            MiniMap::BuildingAcquisition::RuntimeFieldContract contract{};
            int32_t pointIndexOffset = -1;
            int32_t pointPositionOffset = -1;
            int32_t pointTangentOffset = -1;
            uint32_t pointStride = 0;
            const bool resolved = ResolveSplineDataFields(scanner, self, contract,
                    pointIndexOffset, pointPositionOffset, pointTangentOffset, pointStride) &&
                ResolveSplineMassFields(scanner, self, contract) &&
                ResolveSplineCurveContainerFields(scanner, self) &&
                ResolveSplineChannelFields(scanner, self) &&
                ResolveVectorCurvePointFields(scanner, self) &&
                ResolveQuatCurvePointFields(scanner, self) &&
                ResolveFloatCurvePointFields(scanner, self);
            if (!resolved ||
                !MiniMap::BuildingAcquisition::BuildingSplineSource::PublishRuntimeFieldContract(contract))
            {
                MiniMap::BuildingAcquisition::BuildingSplineSource::ResetRuntimeFieldContract();
                MiniMap::BuildingAcquisition::BuildingSplineSource::SetPointArrayContract(-1, -1, -1, 0);
                return false;
            }
            MiniMap::BuildingAcquisition::BuildingSplineSource::SetPointArrayContract(
                pointIndexOffset, pointPositionOffset, pointTangentOffset, pointStride);
            return true;
        }

        /** Checks resolved width, containing extent, and alignment for a typed field access. */
        static bool FieldFitsRuntimeType(
            const PluginFieldSpec& field,
            uint32_t containingSize,
            uint32_t expectedSize,
            std::size_t alignment)
        {
            if (field.offset < 0 || field.resolvedSize != expectedSize || containingSize == 0)
                return false;
            const uint64_t fieldEnd = static_cast<uint64_t>(field.offset) + field.resolvedSize;
            return fieldEnd <= containingSize && alignment != 0 &&
                static_cast<std::size_t>(field.offset) % alignment == 0;
        }

        /** Validates placement and nested entity-config fields against runtime sizes. */
        static bool ValidatePlacementDefinitionFields(
            const PluginFieldSpec* fields,
            const PluginFieldSpec* classSizes)
        {
            const bool placementFieldsFit =
                FieldFitsRuntimeType(fields[0], classSizes[0].resolvedSize,
                    sizeof(SDK::ECrBuildingID), alignof(SDK::ECrBuildingID)) &&
                FieldFitsRuntimeType(fields[1], classSizes[0].resolvedSize,
                    sizeof(SDK::TSoftClassPtr<SDK::UClass>), alignof(SDK::TSoftClassPtr<SDK::UClass>)) &&
                FieldFitsRuntimeType(fields[2], classSizes[0].resolvedSize,
                    classSizes[1].resolvedSize, alignof(SDK::FAuAPMassSpawnedEntityType));
            const bool nestedTypeHasNativeSize = classSizes[1].resolvedSize ==
                sizeof(SDK::FAuAPMassSpawnedEntityType);
            const bool nestedFieldsFit =
                FieldFitsRuntimeType(fields[3], classSizes[1].resolvedSize,
                    sizeof(SDK::TSoftObjectPtr<SDK::UMassEntityConfigAsset>),
                    alignof(SDK::TSoftObjectPtr<SDK::UMassEntityConfigAsset>)) &&
                FieldFitsRuntimeType(fields[4], classSizes[1].resolvedSize,
                    sizeof(SDK::UMassEntityConfigAsset*), alignof(SDK::UMassEntityConfigAsset*));
            const uint64_t configEnd = static_cast<uint64_t>(fields[3].offset) + fields[3].resolvedSize;
            const uint64_t configPointerEnd =
                static_cast<uint64_t>(fields[4].offset) + fields[4].resolvedSize;
            const bool nestedFieldsDoNotOverlap =
                configEnd <= static_cast<uint64_t>(fields[4].offset) ||
                configPointerEnd <= static_cast<uint64_t>(fields[3].offset);
            return placementFieldsFit && nestedTypeHasNativeSize && nestedFieldsFit &&
                nestedFieldsDoNotOverlap;
        }

        /** Validates game-specific tags, trait references, and walkway mesh fields. */
        static bool ValidateAuxiliaryDefinitionFields(
            const PluginFieldSpec* fields,
            const PluginFieldSpec* classSizes)
        {
            return FieldFitsRuntimeType(fields[5], classSizes[2].resolvedSize,
                    sizeof(SDK::TArray<SDK::FName>), alignof(SDK::TArray<SDK::FName>)) &&
                FieldFitsRuntimeType(fields[9], classSizes[3].resolvedSize,
                    sizeof(SDK::UClass*), alignof(SDK::UClass*)) &&
                FieldFitsRuntimeType(fields[10], classSizes[3].resolvedSize,
                    sizeof(SDK::UClass*), alignof(SDK::UClass*)) &&
                FieldFitsRuntimeType(fields[14], classSizes[4].resolvedSize,
                    sizeof(SDK::UStaticMesh*), alignof(SDK::UStaticMesh*));
        }

        /** Publishes the complete validated StarRupture definition offset set. */
        static bool PublishBuildingDefinitionOffsets(const PluginFieldSpec* fields)
        {
            const MiniMap::BuildingAcquisition::RuntimeFieldOffsets offsets{
                fields[0].offset, fields[1].offset, fields[2].offset, fields[3].offset,
                fields[4].offset, fields[5].offset, fields[9].offset, fields[10].offset,
                fields[14].offset};
            return MiniMap::BuildingAcquisition::BuildingDefinitionAcquisition::
                PublishRuntimeFieldOffsets(offsets);
        }

        /** Resolves required building fields and validates their runtime typed-access contract. */
        static bool ResolveBuildingDefinitionFields(
            IPluginHookScanner* scanner,
            const IPluginSelf* self,
            MiniMap::Adapters::AlienX::RequiredScanner& requiredScanner)
        {
            constexpr std::size_t kFVectorXFieldIndex = 32;
            constexpr std::size_t kFVectorYFieldIndex = 33;
            constexpr std::size_t kFVectorZFieldIndex = 34;
            static PluginFieldSpec classSizes[] = {
                SR_SIZE(UAuActorPlacementData), SR_SIZE(FAuAPMassSpawnedEntityType),
                SR_SIZE(UCrBuildingData), SR_SIZE(UCrMassRepVisCosmeticsTrait),
                SR_SIZE(ACrModularRampWalkway)};
            bool allResolved = true;
            for (PluginFieldSpec& field : g_buildingDefinitionFields)
            {
                field.offset = -1;
                field.resolvedSize = 0;
                allResolved = PluginMap::ResolveField(scanner, self, field) && allResolved;
            }
            for (PluginFieldSpec& classSize : classSizes)
            {
                classSize.offset = -1;
                classSize.resolvedSize = 0;
                allResolved = PluginMap::ResolveField(scanner, self, classSize) && allResolved;
            }
            g_foundablesFieldOffsets.VectorX = g_buildingDefinitionFields[kFVectorXFieldIndex].offset;
            g_foundablesFieldOffsets.VectorY = g_buildingDefinitionFields[kFVectorYFieldIndex].offset;
            g_foundablesFieldOffsets.VectorZ = g_buildingDefinitionFields[kFVectorZFieldIndex].offset;
            if (!allResolved || !ValidatePlacementDefinitionFields(g_buildingDefinitionFields, classSizes) ||
                !ValidateAuxiliaryDefinitionFields(g_buildingDefinitionFields, classSizes) ||
                !PublishBuildingDefinitionOffsets(g_buildingDefinitionFields))
            {
                requiredScanner.Fail(
                    "MiniMap::BuildingDefinitionFields",
                    "resolved StarRupture definition fields are missing or outside their runtime type extents");
                return false;
            }
            return true;
        }

        /** Resolves Foundables fields and validates the storage-item extent contract. */
        static bool ResolveFoundablesFields(IPluginHookScanner* scanner, const IPluginSelf* self)
        {
            static PluginFieldSpec fields[] = {
                { "FCrInventoryFragment::InputInventoryContainer", "FCrInventoryFragment",
                    "InputInventoryContainer", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(SDK::FCrItemsStorageContainer),
                    static_cast<int32_t>(offsetof(SDK::FCrInventoryFragment, InputInventoryContainer)) },
                { "FCrInventoryFragment::MainInventoryContainer", "FCrInventoryFragment",
                    "MainInventoryContainer", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(SDK::FCrItemsStorageContainer),
                    static_cast<int32_t>(offsetof(SDK::FCrInventoryFragment, MainInventoryContainer)) },
                { "FCrItemsStorageContainer::Items", "FCrItemsStorageContainer", "Items",
                    PLUGIN_FIELD_FLAG_NONE, sizeof(decltype(SDK::FCrItemsStorageContainer::Items)),
                    static_cast<int32_t>(offsetof(SDK::FCrItemsStorageContainer, Items)) },
                { "FCrStorageItem::Item", "FCrStorageItem", "Item", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(SDK::FAuSimpleItem),
                    static_cast<int32_t>(offsetof(SDK::FCrStorageItem, Item)) },
                { "FAuSimpleItem::ItemId", "FAuSimpleItem", "ItemId", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(SDK::FAuItemId),
                    static_cast<int32_t>(offsetof(SDK::FAuSimpleItem, ItemId)) },
                { "FAuSimpleItem::ItemDataBase", "FAuSimpleItem", "ItemDataBase", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(SDK::UAuItemDataBase*),
                    static_cast<int32_t>(offsetof(SDK::FAuSimpleItem, ItemDataBase)) },
                { "FAuSimpleItem::Count", "FAuSimpleItem", "Count", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::FAuSimpleItem::Count)),
                    static_cast<int32_t>(offsetof(SDK::FAuSimpleItem, Count)) },
                { "FAuItemId::Handle", "FAuItemId", "Handle", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(SDK::FGuid), static_cast<int32_t>(offsetof(SDK::FAuItemId, Handle)) },
                { "UAuItemDataBase::UIItemType", "UAuItemDataBase", "UIItemType", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(SDK::EUIItemType),
                    static_cast<int32_t>(offsetof(SDK::UAuItemDataBase, UIItemType)) },
                { "FCrMassFoundableParameters::FoundableType", "FCrMassFoundableParameters",
                    "FoundableType", PLUGIN_FIELD_FLAG_NONE,
                    sizeof(decltype(SDK::FCrMassFoundableParameters::FoundableType)),
                    static_cast<int32_t>(offsetof(SDK::FCrMassFoundableParameters, FoundableType)) }
            };
            static PluginFieldSpec sizes[] = {
                SR_SIZE(FCrStorageItem),
                SR_SIZE_CHECK(FAuSimpleItem)
            };

            bool allResolved = true;
            for (PluginFieldSpec& field : fields)
            {
                field.offset = -1;
                field.resolvedSize = 0;
                allResolved = PluginMap::ResolveField(scanner, self, field) && allResolved;
            }
            bool allSizesResolved = true;
            for (PluginFieldSpec& size : sizes)
            {
                size.offset = -1;
                size.resolvedSize = 0;
                allSizesResolved = PluginMap::ResolveField(scanner, self, size) && allSizesResolved;
            }
            if (!allResolved || !allSizesResolved || !MemberFitsSize(fields[3], sizes[0]))
                return false;

            g_foundablesFieldOffsets.InventoryInputContainer = fields[0].offset;
            g_foundablesFieldOffsets.InventoryMainContainer = fields[1].offset;
            g_foundablesFieldOffsets.StorageContainerItems = fields[2].offset;
            g_foundablesFieldOffsets.SimpleItemId = fields[4].offset;
            g_foundablesFieldOffsets.SimpleItemDataBase = fields[5].offset;
            g_foundablesFieldOffsets.SimpleItemCount = fields[6].offset;
            g_foundablesFieldOffsets.ItemIdHandle = fields[7].offset;
            g_foundablesFieldOffsets.ItemDataBaseUIItemType = fields[8].offset;
            g_foundablesFieldOffsets.FoundableType = fields[9].offset;
            MiniMapFoundables::SetStorageItemContract(fields[3].offset, sizes[0].resolvedSize);
            return true;
        }

    public:
        /** Resolves required Mass fields and validates the runtime State extent. */
        static bool Resolve(
            IPluginHookScanner* scanner,
            const IPluginSelf* self,
            MiniMap::Adapters::AlienX::RequiredScanner& requiredScanner)
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
                    PLUGIN_FIELD_NATIVE,
                    sizeof(SDK::EMassEnemyInitializationState), -1 }
            };

            bool allResolved = true;
            for (PluginFieldSpec& field : fields)
            {
                field.offset = -1;
                field.resolvedSize = 0;
                allResolved = PluginMap::ResolveField(scanner, self, field) && allResolved;
            }
            if (!allResolved)
            {
                return false;
            }

            const PluginFieldSpec& enemyStateField = fields[6];
            if (enemyStateField.offset < 0 ||
                enemyStateField.resolvedSize != sizeof(SDK::EMassEnemyInitializationState) ||
                static_cast<uint64_t>(enemyStateField.offset) + enemyStateField.resolvedSize >
                    sizeof(SDK::FMassEnemyStateFragment))
            {
                requiredScanner.Fail(
                    "FMassEnemyStateFragment::State",
                    "resolved State field does not fit the validated Client fragment layout");
                return false;
            }
            if (!MiniMapNative::PublishEnemyStateField(
                enemyStateField.offset, enemyStateField.resolvedSize))
            {
                requiredScanner.Fail(
                    "FMassEnemyStateFragment::State",
                    "resolved State field could not be published to the native API");
                return false;
            }
            g_foundablesFieldOffsets.TransformFragmentTransform = fields[2].offset;
            g_foundablesFieldOffsets.TransformTranslation = fields[4].offset;
            const bool foundablesResolved = ResolveFoundablesFields(scanner, self);
            const bool definitionsResolved =
                ResolveBuildingDefinitionFields(scanner, self, requiredScanner);
            const bool splineFieldsResolved = ResolveSplineFields(scanner, self);
            const bool compatible =
                allResolved && foundablesResolved && definitionsResolved && splineFieldsResolved;
            if (!compatible)
            {
                MiniMap::BuildingAcquisition::BuildingSplineSource::ResetRuntimeFieldContract();
                MiniMap::BuildingAcquisition::BuildingSplineSource::SetPointArrayContract(-1, -1, -1, 0);
            }
            return compatible;
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

        /** Requires a reflected value's base alignment without constraining its runtime size. */
        static bool CheckTypeAlignment(
            const IPluginSelf* self,
            const char* objectPath,
            const char* displayName,
            std::size_t expectedAlignment)
        {
            const SDK::UScriptStruct* type = SDK::UObject::FindObject<SDK::UScriptStruct>(
                objectPath, SDK::EClassCastFlags::ScriptStruct);
            if (type != nullptr && type->MinAlignment == static_cast<std::int32_t>(expectedAlignment))
                return true;
            if (self && self->logger && self->logger->Error)
                self->logger->Error(self,
                    "MiniMap: Client admission refused: %s SDK align=0x%X, running align=0x%X",
                    displayName, static_cast<unsigned>(expectedAlignment),
                    type ? static_cast<unsigned>(type->MinAlignment) : 0);
            return false;
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
            return CheckType(self, "ScriptStruct AuActorPlacement.AuAPSplinePointData", "FAuAPSplinePointData",
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

        /** Requires exact intrinsic vector, point, and brush layouts used by Terrain. */
        static bool CheckTerrainTypes(const IPluginSelf* self)
        {
            return CheckType(self, "ScriptStruct CoreUObject.Vector3f", "FVector3f",
                    sizeof(SDK::FVector3f), alignof(SDK::FVector3f)) &&
                CheckType(self, "ScriptStruct CoreUObject.IntPoint", "FIntPoint",
                    sizeof(SDK::FIntPoint), alignof(SDK::FIntPoint)) &&
                CheckType(self, "ScriptStruct SlateCore.SlateBrush", "FSlateBrush",
                    sizeof(SDK::FSlateBrush), alignof(SDK::FSlateBrush));
        }

        /** Requires the compiled outer container that still holds the abandoned-base array. */
        static bool CheckPoiStoryTypes(const IPluginSelf* self)
        {
            return CheckType(self, "ScriptStruct Chimera.CrAbandonBaseDataContainer",
                    "FCrAbandonBaseDataContainer", sizeof(SDK::FCrAbandonBaseDataContainer),
                    alignof(SDK::FCrAbandonBaseDataContainer));
        }

        /** Requires resident layouts for capture settings passed through the typed SDK path. */
        static bool CheckCaptureTypes(const IPluginSelf* self)
        {
            return CheckType(self, "ScriptStruct CoreUObject.LinearColor", "FLinearColor",
                    sizeof(SDK::FLinearColor), alignof(SDK::FLinearColor)) &&
                CheckType(self, "ScriptStruct CoreUObject.Color", "FColor",
                    sizeof(SDK::FColor), alignof(SDK::FColor)) &&
                CheckType(self, "ScriptStruct Engine.EngineShowFlagsSetting", "FEngineShowFlagsSetting",
                    sizeof(SDK::FEngineShowFlagsSetting), alignof(SDK::FEngineShowFlagsSetting)) &&
                CheckType(self, "ScriptStruct Engine.TickFunction", "FTickFunction",
                    sizeof(SDK::FTickFunction), alignof(SDK::FTickFunction)) &&
                CheckType(self, "ScriptStruct Engine.ActorComponentTickFunction", "FActorComponentTickFunction",
                    sizeof(SDK::FActorComponentTickFunction), alignof(SDK::FActorComponentTickFunction));
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
                    CheckTypeAlignment(self, "ScriptStruct Chimera.CrBuildingParameters",
                        "FCrBuildingParameters", alignof(SDK::FCrBuildingParameters)) &&
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
                    CheckSplinePayloadType(self) &&
                    CheckCaptureTypes(self);
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
        MINIMAP_VERSION_STRING,
        "Kian369",
        "A configurable on-screen minimap for StarRupture",
        PLUGIN_INTERFACE_VERSION,
        MINIMAP_EXPORTS_TARGET,
        PLUGIN_GAME_BUILT_CL,
#if defined(MODLOADER_CLIENT_BUILD)
        PLUGIN_MAP_MIGRATED |
        // Cross-plugin ABI tables remain callable until process shutdown only.
        PluginLoadSetting(PluginReload::RESTART) |
            PluginUnloadSetting(PluginReload::RESTART)
#else
        PluginLoadSetting(PluginReload::MENU) |
            PluginUnloadSetting(PluginReload::MENU)
#endif
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
        Services::Map::Buildings::Capture::BuildingSpriteRenderer::ResetCaptureFieldOffsets();
        g_buildingMassFieldsResolved = false;
        MiniMap::BuildingAcquisition::BuildingSplineSource::ResetRuntimeFieldContract();
        MiniMap::BuildingAcquisition::BuildingSplineSource::SetPointArrayContract(-1, -1, -1, 0);
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
    MiniMap::BuildingAcquisition::BuildingSplineSource::ResetRuntimeFieldContract();
    g_fowDataFieldResolved = false;
    g_playerMapDataComponentFieldResolved = false;
    g_terrainFieldsResolved = false;
    g_poiStoryFieldsResolved = false;
    g_playerTextureFieldsResolved = false;
    g_captureFieldsResolved = false;
    MiniMap::BuildingAcquisition::BuildingDefinitionAcquisition::ResetRuntimeFieldOffsets();
    Services::Map::Buildings::Capture::BuildingSpriteRenderer::ResetCaptureFieldOffsets();
    g_fowDataClassSize.offset = -1;
    g_fowDataClassSize.resolvedSize = 0;
    g_playerCharacterClassSize.offset = -1;
    g_playerCharacterClassSize.resolvedSize = 0;
    g_actorClassSize.offset = -1;
    g_actorClassSize.resolvedSize = 0;
    g_textureClassSize.offset = -1;
    g_textureClassSize.resolvedSize = 0;
    MiniMapFogOfWar::SetFowDataOffset(-1);
    g_playerFieldOffsets = {};
    g_poiFieldOffsets = {};
    g_foundablesFieldOffsets = {};
    MiniMapTerrainSource::SetFieldOffsets({});
    MiniMapMap::PlayerFieldAccessors::SetOffsets({});
    MiniMapPointsOfInterest::SetFieldOffsets({});
    MiniMapFoundables::SetAcquisitionFieldOffsets({});
    MiniMap::BuildingAcquisition::BuildingSplineSource::SetPointArrayContract(-1, -1, -1, 0);
    MiniMapFoundables::SetStorageItemContract(-1, 0);
    MiniMapFoundables::SetStoryArrayContracts({});
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

        if (!ClientBuildingMassFieldContract::Resolve(scanner, self, requiredScanner))
        {
            MiniMapNative::Shutdown();
            return;
        }
        g_buildingMassFieldsResolved = true;

        g_fowDataField.offset = -1;
        g_fowDataField.resolvedSize = 0;
        g_fowDataClassSize.offset = -1;
        g_fowDataClassSize.resolvedSize = 0;
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
        if (!PluginMap::ResolveField(scanner, self, g_fowDataClassSize))
        {
            MiniMapNative::Shutdown();
            return;
        }
        const uint64_t fowDataEnd = static_cast<uint64_t>(g_fowDataField.offset) +
            g_fowDataField.resolvedSize;
        if (g_fowDataField.offset < 0 ||
            g_fowDataField.resolvedSize != sizeof(SDK::TArray<uint8_t>) ||
            fowDataEnd > g_fowDataClassSize.resolvedSize)
        {
            requiredScanner.Fail(
                "UCrPlayerMapMenuDataComponent::FOWData",
                "resolved FOWData is missing or outside the running component layout");
            MiniMapNative::Shutdown();
            return;
        }
        MiniMapFogOfWar::SetFowDataOffset(g_fowDataField.offset);
        g_fowDataFieldResolved = true;

        g_playerMapDataComponentField.offset = -1;
        g_playerMapDataComponentField.resolvedSize = 0;
        g_playerCharacterClassSize.offset = -1;
        g_playerCharacterClassSize.resolvedSize = 0;
        if (!PluginMap::ResolveField(scanner, self, g_playerMapDataComponentField) ||
            !PluginMap::ResolveField(scanner, self, g_playerCharacterClassSize))
        {
            MiniMapNative::Shutdown();
            return;
        }
        const uint64_t playerMapComponentEnd =
            static_cast<uint64_t>(g_playerMapDataComponentField.offset) +
            g_playerMapDataComponentField.resolvedSize;
        if (g_playerMapDataComponentField.offset < 0 ||
            g_playerMapDataComponentField.resolvedSize !=
                sizeof(SDK::UCrPlayerMapMenuDataComponent*) ||
            playerMapComponentEnd > g_playerCharacterClassSize.resolvedSize ||
            g_playerMapDataComponentField.offset %
                alignof(SDK::UCrPlayerMapMenuDataComponent*) != 0)
        {
            requiredScanner.Fail(
                "ACrCharacterPlayerBase::PlayerMapMenuDataComponent",
                "resolved player map-data component field is missing or outside the running character layout");
            MiniMapNative::Shutdown();
            return;
        }
        g_playerFieldOffsets.PlayerMapDataComponent = g_playerMapDataComponentField.offset;
        g_playerMapDataComponentFieldResolved = true;

        if (!ClientTerrainFieldContract::Resolve(scanner, self, requiredScanner))
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

        if (!ClientCaptureFieldContract::Resolve(scanner, self, requiredScanner))
        {
            MiniMapNative::Shutdown();
            return;
        }
        g_captureFieldsResolved = true;
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
    if (!g_captureFieldsResolved)
    {
        if (self && self->logger && self->logger->Error)
            self->logger->Error(self,
                "MiniMap: Client admission refused: required sprite-capture fields were not validated");
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
