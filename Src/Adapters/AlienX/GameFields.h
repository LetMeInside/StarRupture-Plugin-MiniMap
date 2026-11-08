#pragma once

#include "Native/EngineFields.h"
#include "Native/NativeKeys.h"

#include "SDK/Engine_classes.hpp"
#include "SDK/Chimera_classes.hpp"
#include "SDK/Chimera_structs.hpp"
#include "SDK/AuActorPlacement_classes.hpp"
#include "SDK/AuActorPlacement_structs.hpp"
#include "SDK/AuItems_classes.hpp"
#include "SDK/AuItems_structs.hpp"
#include "SDK/ChimeraUI_classes.hpp"
#include "SDK/ChimeraUI_structs.hpp"
#include "SDK/MassEntity_structs.hpp"
#include <plugin_interface.h>
#include <plugin_map.h>

#include <cstdint>
#include <vector>

// The members of game classes MiniMap uses and the map layers it can run
// without, as the loader's shared version map sees them for the running game
// build (see plugin_map.h). The functions are requested in
// MiniMapFingerprints::Resolve, under the names in Native/NativeKeys.h; the
// members in MiniMapFields::Resolve. Both run in OnPluginLoadHooks after the
// layers below are declared, so a miss turns off the layer it belongs to and
// not the plugin. The engine members of the building-picture capture are in Native/EngineFields.h.

namespace MiniMapFields
{
	namespace Keys = MiniMapNative::Keys;

	// The player character and controller. Every layer starts from the player's
	// pose, so these are core: a miss refuses the plugin.
	inline auto PlayerMainCharacter = SR_FIELD(SDK::ACrCharacterPlayerBase*, ACrPlayerControllerBase, PlayerMainCharacter, 0);
	inline auto IsDead              = SR_FIELD(bool, ACrCharacterPlayerBase, bDead, 0);
	inline auto DroneOwner          = SR_FIELD(SDK::ACrCharacterPlayerBase*, ACrCharacterDroneBase, OwnerPlayer, 0);

	// Members of game classes that sit at different offsets from one game build
	// to the next (a class grows, a member moves), so MiniMap reads them through
	// the map and not at the SDK's compiled offset. The world's game state holds
	// the abandoned-base replicator and the map-menu data helper; the replicator
	// feeds the Foundables and Points Of Interest layers, the helper the terrain.
	// ACrGameStateBase itself grew on a newer game build, so its size is not used anywhere.
	inline auto AbandonBaseInfoReplicator    = SR_FIELD(SDK::ACrAbandonBaseInfoReplicator*, ACrGameStateBase, AbandonBaseInfoReplicator, 0);
	inline auto MapMenuDataReplicationHelper = SR_FIELD(SDK::ACrMapMenuDataReplicationHelper*, ACrGameStateBase, MapMenuDataReplicationHelper, 0);

	// What the Buildings layer reads off a building's placement data and the
	// walkway's class default object. All three moved on a newer game build.
	inline auto BuildingTags         = SR_FIELD(SDK::TArray<SDK::FName>, UCrBuildingData, Tags, 0);
	inline auto WalkwaySplineMesh    = SR_FIELD(SDK::UStaticMesh*, ACrModularRampWalkway, SplineMesh, 0);

	// UCrPlayerMapMenuDataComponent::FOWData, the fog segments' TArray. The
	// Game-SDK folds it into padding, so it has no SDK name: a native row, with
	// CL-127004's offset for when the loader's map has none for the running build.
	struct RawTArray
	{
		const void* Data = nullptr;
		int32_t Num = 0;
		int32_t Max = 0;
	};
	inline auto FogData = PluginField<RawTArray>{ { "UCrPlayerMapMenuDataComponent::FOWData",
		"UCrPlayerMapMenuDataComponent", "FOWData", PLUGIN_FIELD_NATIVE, sizeof(RawTArray), 0x100 } };
	inline const PluginFieldCandidate kFogDataRows[] = {
		{ 127004, 0x100, static_cast<uint32_t>(sizeof(RawTArray)), 0, nullptr },
	};

	// The rest of what the Buildings layer reads off a building: the building's
	// parameters hold its placement data, the placement data the Mass entity config
	// and the actor class, the config's traits the template actors, and the Mass
	// spline fragment and the spline payload the shape of a rail or a pipe. A
	// member that is a game struct held by value (the entity type, the spline data,
	// the payload handle) is requested at any size, since only its own members are
	// read, each through its own key. A TArray of the point struct is read through
	// RawTArray and stepped by the point's size (PluginMap::ElementAt).
	inline auto BuildingPlacementData  = SR_FIELD(SDK::UAuActorPlacementData*, FCrBuildingParameters, PlacementData, 0);
	inline auto PlacementBuildingID    = SR_FIELD(SDK::ECrBuildingID, UAuActorPlacementData, BuildingID, 0);
	inline auto PlacementActorClass    = SR_FIELD(SDK::TSoftClassPtr<SDK::UClass>, UAuActorPlacementData, ActorClass, 0);
	inline auto PlacementEntityType    = PluginField<SDK::FAuAPMassSpawnedEntityType>{ SR_FIELD_ANY_SIZE(UAuActorPlacementData, EntityType, 0) };
	inline auto EntityConfig           = SR_FIELD(SDK::TSoftObjectPtr<SDK::UMassEntityConfigAsset>, FAuAPMassSpawnedEntityType, EntityConfig, 0);
	inline auto EntityConfigPtr        = SR_FIELD(SDK::UMassEntityConfigAsset*, FAuAPMassSpawnedEntityType, EntityConfigPtr, 0);
	inline auto CosmeticHighResActor   = SR_FIELD(SDK::TSubclassOf<SDK::AActor>, UCrMassRepVisCosmeticsTrait, HighResTemplateActor, 0);
	inline auto CosmeticLowResActor    = SR_FIELD(SDK::TSubclassOf<SDK::AActor>, UCrMassRepVisCosmeticsTrait, LowResTemplateActor, 0);
	inline auto VisualHighResActor     = SR_FIELD(SDK::TSubclassOf<SDK::AActor>, UMassVisualizationTrait, HighResTemplateActor, 0);
	inline auto VisualLowResActor      = SR_FIELD(SDK::TSubclassOf<SDK::AActor>, UMassVisualizationTrait, LowResTemplateActor, 0);
	inline auto SplineFragmentData     = PluginField<SDK::FAuAPSplineData>{ SR_FIELD_ANY_SIZE(FAuSplineConnectionFragment, Data, 0) };
	inline auto SplineFragmentCurves   = SR_FIELD(SDK::FSplineCurves, FAuSplineConnectionFragment, SplineCurves, 0);
	inline auto SplineFragmentBuilt    = SR_FIELD(bool, FAuSplineConnectionFragment, bSplineCurvesInitialized, 0);
	inline auto SplineDataPoints       = SR_FIELD(RawTArray, FAuAPSplineData, SplineData, 0);
	inline auto SplinePointPosition    = SR_FIELD(SDK::FVector_NetQuantize, FAuAPSplinePointData, Position, 0);
	inline auto SplinePointTangent     = SR_FIELD(SDK::FVector_NetQuantize10, FAuAPSplinePointData, Tangent, 0);
	inline auto SplinePointSize        = SR_SIZE(FAuAPSplinePointData);
	inline auto SplinePayloadHandle    = PluginField<SDK::FAuAPPayloadDataHandle>{ SR_FIELD_ANY_SIZE(FAuAPMassFragment, Payload, 0) };
	inline auto SplinePayloadSize      = SR_SIZE(FAuAPSplineActorPayloadData);
	inline auto SplinePayloadData      = PluginField<SDK::FAuAPSplineData>{ SR_FIELD_ANY_SIZE(FAuAPSplineActorPayloadData, Data, 0) };

	// What the map layers read off the game's map-menu data. The player character holds
	// the data component the filters and the fog live on; a marker category holds the
	// brush a marker is drawn with, whether it can be filtered and whether the fog hides
	// it (the point-of-interest category adds two brushes of its own); the dev settings
	// name the terrain data asset, and that asset holds the terrain's origin, its tile
	// size and one record per tile, each with the brush of the tile's picture. The
	// brushes, the vectors and the grid index are engine types, read at their compiled
	// size; the tile record is a game struct stepped by the size the game reports.
	inline auto BuildingDrone          = SR_FIELD(SDK::ACrCharacterDroneBase*, ACrCharacterPlayerBase, BuildingDrone, 0);
	inline auto PlayerMapData          = SR_FIELD(SDK::UCrPlayerMapMenuDataComponent*, ACrCharacterPlayerBase, PlayerMapMenuDataComponent, 0);
	inline auto CategoryIcon           = SR_FIELD(SDK::FSlateBrush, UCrMapMenuCategoryData, Icon, 0);
	inline auto CategoryBlocked        = SR_FIELD(bool, UCrMapMenuCategoryData, CategoryBlocked, 0);
	inline auto CategoryLegendFilter   = SR_FIELD(SDK::ECrMapMenuMarkerFilter, UCrMapMenuCategoryData, LegendFilter, 0);
	inline auto CategoryFogHidable     = SR_FIELD(bool, UCrMapMenuCategoryData, CanBeHiddenByFogOfWar, 0);
	inline auto PoiUnknownIcon         = SR_FIELD(SDK::FSlateBrush, UCrMapMenuPOIData, UnknownIcon, 0);
	inline auto PoiCompletedIcon       = SR_FIELD(SDK::FSlateBrush, UCrMapMenuPOIData, AbandonBaseCompletedIcon, 0);
	inline auto DevSettingsTerrainData = SR_FIELD(SDK::TSoftObjectPtr<SDK::UCrMapMenuTerrainData>, UCrMapMenuDevSettings, TerrainData, 0);
	inline auto TerrainPivot           = SR_FIELD(SDK::FVector3f, UCrMapMenuTerrainData, MapTerrainTopLeftPivotPoint, 0);
	inline auto TerrainSegmentSize     = SR_FIELD(SDK::FVector3f, UCrMapMenuTerrainData, MapTerrainSegmentSize, 0);
	inline auto TerrainSegments        = SR_FIELD(RawTArray, UCrMapMenuTerrainData, TerrainSegmentsData, 0);
	inline auto TerrainSegmentStride   = SR_SIZE(FCrTerrainSegmentData);
	inline auto TerrainSegmentGrid     = SR_FIELD(SDK::FIntPoint, FCrTerrainSegmentData, TerrainSegmentGridIndex, 0);
	inline auto TerrainSegmentTexture  = SR_FIELD(SDK::FSlateBrush, FCrTerrainSegmentData, TerrainSegmentTexture, 0);
	inline auto TerrainSegmentRadiation2 = SR_FIELD(SDK::FSlateBrush, FCrTerrainSegmentData, TerrainSegmentTextureRadiation2, 0);
	inline auto RadiationLevel         = SR_FIELD(int32_t, ACrMapMenuDataReplicationHelper, CurrentRadiationLevelReplicated, 0);

	// What the point-of-interest and foundable layers read off a point-of-interest marker
	// and the abandoned-base replicator: the marker's type, state and guid, the story
	// items assigned to it, and the replicator's records (one per base, each with the
	// base's guid and a record per story item). The records are game structs held in
	// arrays and stepped by the size the game reports; the replicator's container is
	// held by value and requested at any size.
	inline auto PoiState               = SR_FIELD(SDK::EPointOfInterestState, ACrPointOfInterestMarkerActor, PointOfInterestState, 0);
	inline auto PoiType                = SR_FIELD(SDK::ECrPointOfInterestType, ACrPointOfInterestMarkerActor, PointOfInterestType, 0);
	inline auto PoiGuid                = SR_FIELD(SDK::FGuid, ACrPointOfInterestMarkerActor, Guid, 0);
	inline auto PoiStoryItems          = SR_FIELD(SDK::TArray<SDK::FGuid>, ACrPointOfInterestMarkerActor, AbandonBaseStoryItemsAssigned, 0);
	inline auto AbandonBaseContainer   = PluginField<SDK::FCrAbandonBaseDataContainer>{ SR_FIELD_ANY_SIZE(ACrAbandonBaseInfoReplicator, AbandonBaseDataContainer, 0) };
	inline auto AbandonBaseRecords     = SR_FIELD(RawTArray, FCrAbandonBaseDataContainer, AbandonBaseData, 0);
	inline auto AbandonBaseRecordSize  = SR_SIZE(FCrAbandonBaseData);
	inline auto AbandonBasePOIId       = SR_FIELD(SDK::FGuid, FCrAbandonBaseData, AbandonBasePOIId, 0);
	inline auto AbandonBaseStatuses    = SR_FIELD(RawTArray, FCrAbandonBaseData, StoryItemsCollectedStatus, 0);
	inline auto StoryStatusSize        = SR_SIZE(FCrAbandonBaseDataCollectedStatus);
	inline auto StoryStatusItem        = SR_FIELD(SDK::FGuid, FCrAbandonBaseDataCollectedStatus, Item, 0);
	// Read as the byte it is: a byte that is neither 0 nor 1 means unknown.
	inline auto StoryStatusValue       = SR_FIELD(uint8_t, FCrAbandonBaseDataCollectedStatus, Status, 0);
	inline auto DatapadGuid            = SR_FIELD(SDK::FGuid, ACrDatapadBase, Guid, 0);
	inline auto AudiologGuid           = SR_FIELD(SDK::FGuid, ACrAudiologBase, Guid, 0);

	// What the foundable layer reads off a Mass foundable: its parameters' type, and what
	// its inventory fragment holds. The two item containers are game structs held by
	// value, each with a TArray of storage records, a game struct stepped by the size
	// the game reports; a record holds a simple item (by value) with its count, its
	// item data and its id (by value, over a guid).
	inline auto FoundableType          = SR_FIELD(SDK::ECrFoundableType, FCrMassFoundableParameters, FoundableType, 0);
	inline auto InventoryMain          = PluginField<SDK::FCrItemsStorageContainer>{ SR_FIELD_ANY_SIZE(FCrInventoryFragment, MainInventoryContainer, 0) };
	inline auto InventoryInput         = PluginField<SDK::FCrItemsStorageContainer>{ SR_FIELD_ANY_SIZE(FCrInventoryFragment, InputInventoryContainer, 0) };
	inline auto StorageItems           = SR_FIELD(RawTArray, FCrItemsStorageContainer, Items, 0);
	inline auto StorageItemSize        = SR_SIZE(FCrStorageItem);
	inline auto StorageItemItem        = PluginField<SDK::FAuSimpleItem>{ SR_FIELD_ANY_SIZE(FCrStorageItem, Item, 0) };
	inline auto SimpleItemCount        = SR_FIELD(int32_t, FAuSimpleItem, Count, 0);
	inline auto SimpleItemData         = SR_FIELD(SDK::UAuItemDataBase*, FAuSimpleItem, ItemDataBase, 0);
	inline auto SimpleItemId           = PluginField<SDK::FAuItemId>{ SR_FIELD_ANY_SIZE(FAuSimpleItem, ItemId, 0) };
	inline auto ItemIdHandle           = SR_FIELD(SDK::FGuid, FAuItemId, Handle, 0);
	inline auto ItemUIType             = SR_FIELD(SDK::EUIItemType, UAuItemDataBase, UIItemType, 0);

	// The terrain record of tile (gridX, gridY), or null. The array order does not follow
	// the grid, so each record's own index is compared.
	inline const uint8_t* FindTerrainSegment(const void* terrainData, int gridX, int gridY)
	{
		const RawTArray* segments = TerrainSegments.In(terrainData);
		if (segments == nullptr) return nullptr;

		for (int32_t i = 0; i < segments->Num; ++i)
		{
			const uint8_t* segment = PluginMap::ElementAt(segments->Data, segments->Num, i, TerrainSegmentStride);
			const SDK::FIntPoint* grid = TerrainSegmentGrid.In(segment);
			if (grid != nullptr && grid->X == gridX && grid->Y == gridY) return segment;
		}
		return nullptr;
	}

	// The Mass enemy state (the fragment's initialisation state). The game does not
	// reflect it (the Game-SDK folds the fragment into padding), so it is a native
	// row, with CL-127004's offset for when the loader's map has none for the
	// running build.
	inline auto EnemyState = PluginField<uint8_t>{ { "FMassEnemyStateFragment::State",
		"FMassEnemyStateFragment", "State", PLUGIN_FIELD_NATIVE, sizeof(uint8_t), 0x1 } };
	inline const PluginFieldCandidate kEnemyStateRows[] = {
		{ 127004, 0x1, static_cast<uint32_t>(sizeof(uint8_t)), 0, nullptr },
	};

	// Reflected, so the game's own metadata gives them on every build: the first
	// three doubles of an FTransform and a bit of a texture's flags byte.
	inline auto TransformTranslation = SR_FIELD(SDK::FVector, FTransform, Translation, 0);
	inline auto TextureSRGB = SR_BITFIELD(UTexture, SRGB);

	// FMassEntityQuery is a reflected script struct (MassEntity.MassEntityQuery), so
	// the loader gives its size from the game's own metadata on every build, with no
	// row. Code that holds a query takes a PluginMap::StructBuffer of this size,
	// never a fixed array, so a build with a bigger query cannot be overrun.
	inline auto MassQuerySize = SR_SIZE(FMassEntityQuery);

	// The Mass entity manager (a TSharedPtr: the object and its reference
	// controller) of the two subsystems of the sprite capture's world. The
	// Game-SDK folds both into padding, so each is a native row, with CL-127004's
	// offset for when the loader's map has none for the running build. The
	// capture checks that both are still empty in its private world.
	inline auto ElectricityEntityManager = PluginField<PluginMap::SharedRef>{ { "UCrElectricityGraphSubsystem::EntityManager",
		"UCrElectricityGraphSubsystem", "EntityManager", PLUGIN_FIELD_NATIVE, sizeof(PluginMap::SharedRef), 0x290 } };
	inline auto GridEntityManager        = PluginField<PluginMap::SharedRef>{ { "UCrEntityGridSubsystem::EntityManager",
		"UCrEntityGridSubsystem", "EntityManager", PLUGIN_FIELD_NATIVE, sizeof(PluginMap::SharedRef), 0xf0 } };
	inline const PluginFieldCandidate kElectricityEntityManagerRows[] = {
		{ 127004, 0x290, static_cast<uint32_t>(sizeof(PluginMap::SharedRef)), 0, nullptr },
	};
	inline const PluginFieldCandidate kGridEntityManagerRows[] = {
		{ 127004, 0xf0, static_cast<uint32_t>(sizeof(PluginMap::SharedRef)), 0, nullptr },
	};

	// The Mass entity manager of a subsystem is the SDK's own native field,
	// PluginMap::MassEntityManagerField, with its 127004 row; it is resolved below.

	// Asks the loader for every row above. False when any did not resolve; the
	// loader has then recorded the failure and refuses the plugin or turns off
	// the feature that lists it.
	inline bool Resolve(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		const auto rowCount = [](const auto& rows) { return static_cast<uint32_t>(sizeof(rows) / sizeof(rows[0])); };
		bool all = PluginMap::ResolveField(scanner, self, FogData, kFogDataRows, rowCount(kFogDataRows));
		all = PluginMap::ResolveField(scanner, self, EnemyState, kEnemyStateRows, rowCount(kEnemyStateRows)) && all;
		all = PluginMap::ResolveField(scanner, self, ElectricityEntityManager, kElectricityEntityManagerRows, rowCount(kElectricityEntityManagerRows)) && all;
		all = PluginMap::ResolveField(scanner, self, GridEntityManager, kGridEntityManagerRows, rowCount(kGridEntityManagerRows)) && all;
		all = PluginMap::ResolveMassEntityManager(scanner, self) && all;
		all = MiniMapEngineFields::Resolve(self, scanner) && all;
		return PluginMap::ResolveFields(scanner, self,
			PlayerMainCharacter, IsDead, DroneOwner, MapMenuDataReplicationHelper,
			AbandonBaseInfoReplicator, BuildingTags, WalkwaySplineMesh,
			TransformTranslation, TextureSRGB, MassQuerySize,
			BuildingPlacementData, PlacementBuildingID, PlacementActorClass, PlacementEntityType, EntityConfig, EntityConfigPtr,
			CosmeticHighResActor, CosmeticLowResActor, VisualHighResActor, VisualLowResActor,
			SplineFragmentData, SplineFragmentCurves, SplineFragmentBuilt, SplineDataPoints,
			SplinePointPosition, SplinePointTangent, SplinePointSize, SplinePayloadHandle,
			SplinePayloadSize, SplinePayloadData,
			BuildingDrone, PlayerMapData, CategoryIcon, CategoryBlocked, CategoryLegendFilter, CategoryFogHidable,
			PoiUnknownIcon, PoiCompletedIcon, DevSettingsTerrainData, TerrainPivot, TerrainSegmentSize,
			TerrainSegments, TerrainSegmentStride, TerrainSegmentGrid, TerrainSegmentTexture,
			TerrainSegmentRadiation2, RadiationLevel,
			PoiState, PoiType, PoiGuid, PoiStoryItems, AbandonBaseContainer, AbandonBaseRecords,
			AbandonBaseRecordSize, AbandonBasePOIId, AbandonBaseStatuses, StoryStatusSize, StoryStatusItem,
			StoryStatusValue, DatapadGuid, AudiologGuid,
			FoundableType, InventoryMain, InventoryInput, StorageItems, StorageItemSize, StorageItemItem,
			SimpleItemCount, SimpleItemData, SimpleItemId, ItemIdHandle, ItemUIType) && all;
	}

	using PluginMap::KeysOf;

	// `keys` and the engine members of Native/EngineFields.h.
	inline std::vector<const char*> WithEngineKeys(std::vector<const char*> keys)
	{
		const auto engine = MiniMapEngineFields::Keys();
		keys.insert(keys.end(), engine.begin(), engine.end());
		return keys;
	}

	// The layers that can be off while the rest runs. Each lists the keys only it
	// needs; a key two layers need is listed in both. The terrain is not one:
	// it gives every other layer its transform, so a miss there leaves nothing
	// to draw and refuses the plugin, so its keys (the map-menu data above) are listed
	// by none. Declared in OnPluginLoadHooks before any key
	// is resolved. Every entry point of a layer checks On(), so an off layer runs
	// nothing and reads none of its keys or config entries.
	namespace Features
	{
		inline PluginMap::Feature FogOfWar{ "fog-of-war", "Fog of war on the minimap",
			KeysOf(FogData, PlayerMapData),
			{ "Fog Of War.DisableFog", "Fog Of War.FeatherRange" } };

		inline PluginMap::Feature PointsOfInterest{ "points-of-interest", "Points of interest on the minimap",
			KeysOf(Keys::GetAllActorsOfClass, Keys::PoiStaticClass, Keys::PoiCategory, Keys::MarkerFilter,
				Keys::AbandonBaseCompleted, AbandonBaseInfoReplicator,
				PlayerMapData, CategoryIcon, CategoryLegendFilter, CategoryFogHidable, PoiUnknownIcon, PoiCompletedIcon,
				PoiState, PoiType, PoiGuid, AbandonBaseContainer, AbandonBaseRecords, AbandonBaseRecordSize,
				AbandonBasePOIId),
			{ "Points Of Interest.ShowLayer", "Points Of Interest.MarkerScale" } };

		inline PluginMap::Feature Foundables{ "foundables", "Foundables on the minimap",
			KeysOf(Keys::GetAllActorsOfClass, Keys::FoundableCategory, Keys::MarkerFilter,
				Keys::MassSubsystem, Keys::QueryConstruct, Keys::QueryDestruct, Keys::MatchingHandles,
				Keys::FragmentDataPtr, Keys::ConstSharedPtr, Keys::AddInventoryRequirement,
				Keys::AddTransformRequirement, Keys::AddFoundableParameters, Keys::AddFoundableTag,
				Keys::AddTagRequirement, Keys::TransformStaticStruct,
				Keys::FoundableStaticStruct, AbandonBaseInfoReplicator,
				PluginMap::MassEntityManagerField, MassQuerySize,
				PlayerMapData, CategoryIcon, CategoryBlocked, CategoryLegendFilter, FoundableType,
				InventoryMain, InventoryInput, StorageItems, StorageItemSize, StorageItemItem, SimpleItemCount,
				SimpleItemData, SimpleItemId, ItemIdHandle, ItemUIType,
				PoiType, PoiGuid, PoiStoryItems, AbandonBaseContainer, AbandonBaseRecords, AbandonBaseRecordSize,
				AbandonBasePOIId, AbandonBaseStatuses, StoryStatusSize, StoryStatusItem, StoryStatusValue,
				DatapadGuid, AudiologGuid,
				MiniMapEngineFields::WorldGameState, MiniMapEngineFields::ActorRootComponent,
				MiniMapEngineFields::OwningWorld, MiniMapEngineFields::ActorBeingDestroyed),
			{ "Foundables.ShowLayer", "Foundables.ShowLootInBases", "Foundables.MarkerScale" } };

		inline PluginMap::Feature AlienActors{ "alien-actors", "Alien actors on the minimap",
			KeysOf(Keys::MassSubsystem, Keys::QueryConstruct, Keys::QueryDestruct, Keys::MatchingHandles,
				Keys::FragmentDataPtr, Keys::AddTransformRequirement, Keys::AddEnemyStateRequirement,
				Keys::AddTagRequirement, Keys::TransformStaticStruct, Keys::EnemyStateStaticStruct,
				Keys::EnemyTagStaticStruct, Keys::NeutralTagStaticStruct,
				PluginMap::MassEntityManagerField, MassQuerySize, EnemyState, TransformTranslation),
			{ "Alien Actors.ShowLayer" } };

		inline PluginMap::Feature Buildings{ "buildings", "Buildings on the minimap",
			KeysOf(Keys::MassSubsystem, Keys::QueryConstruct, Keys::QueryDestruct, Keys::MatchingHandles,
				Keys::FragmentDataPtr, Keys::ConstSharedPtr, Keys::AddTransformRequirement,
				Keys::AddTagRequirement, Keys::IsEntityBuilt, Keys::IsEntityValid, Keys::GridSubsystem,
				Keys::GridForEachCell, BuildingTags, WalkwaySplineMesh,
				PluginMap::MassEntityManagerField, MassQuerySize,
				BuildingPlacementData, PlacementBuildingID, PlacementActorClass, PlacementEntityType, EntityConfig, EntityConfigPtr,
				CosmeticHighResActor, CosmeticLowResActor, VisualHighResActor, VisualLowResActor,
				SplineFragmentData, SplineFragmentCurves, SplineFragmentBuilt, SplineDataPoints,
				SplinePointPosition, SplinePointTangent, SplinePointSize),
			{ "Buildings.ShowLayer", "Buildings.RenderDroneRails" } };

		// The BuildingComplex observer hooks the save-load notifications to know when the
		// buildings are restored, and the Add and Remove of a building entity to follow
		// them. Without one of the four it is not installed, and the pictures built on
		// it wait for a build that has the function.
		inline PluginMap::Feature BuildingRestore{ "building-restore", "Building tracking across save loads",
			KeysOf(Keys::SaveLoaded, Keys::PostSaveLoaded, Keys::BuildingAdd, Keys::BuildingRemove), {} };

		// Rails and pipes are drawn from the spline payload of the building actor.
		// Without it the buildings still draw, as boxes.
		inline PluginMap::Feature BuildingSplines{ "building-splines", "Rails and pipes on the minimap",
			KeysOf(Keys::SplinePayloadGet, Keys::SplinePayloadDestroy,
				SplinePayloadHandle, SplineDataPoints, SplinePointPosition, SplinePointTangent, SplinePointSize,
				SplinePayloadSize, SplinePayloadData),
			{ "Buildings.RenderDroneRails" } };

		inline PluginMap::Feature BuildingSprites{ "building-sprites", "Building pictures on the minimap",
			WithEngineKeys(KeysOf(Keys::SpawnObject, Keys::SetObjectFlags, Keys::RegisterComponent, Keys::UnregisterComponent,
				Keys::DestroyComponent, Keys::InitRenderTarget, Keys::RenderTargetResource,
				Keys::TransitionResources, Keys::FenceConstruct, Keys::FenceDestroy, Keys::FenceBegin,
				Keys::FencePoll, Keys::FromValidEName, Keys::SubmitRenderCommand, Keys::ImmediateCommandList,
				Keys::CreateRHITexture, Keys::ReadbackConstruct, Keys::ReadbackDestroy, Keys::ReadbackCopy,
				Keys::ReadbackLock, Keys::ReadbackUnlock, Keys::GpuFencePoll, Keys::MarkForDelete,
				Keys::MemoryMalloc, Keys::DynamicRHI, Keys::RenderCommandPipe,
				Keys::ThreadedRendering, Keys::RHIMultithreading, Keys::GpuCount,
				Keys::CreateWorld, Keys::InitWorld, Keys::UpdateWorldComponents,
				Keys::CleanupWorld, Keys::WorldContext, Keys::GetSubsystemInternal,
				Keys::GarbageCollectingLocking, Keys::PurgePending, Keys::ReachabilityPending,
				Keys::TickableTickType, Keys::VisibilityPending, Keys::DestroyActor,
				Keys::ActiveGarbageCollection, Keys::EngineStorage,
				ElectricityEntityManager, GridEntityManager)),
			{ "Buildings.CaptureSprites" } };

		// The camera follows the build drone while the player flies it. Without the
		// player's drone member the minimap tracks the character, as it does with
		// General.FollowBuildDrone off.
		inline PluginMap::Feature FollowBuildDrone{ "follow-build-drone", "Minimap follows the build drone",
			KeysOf(BuildingDrone),
			{ "General.FollowBuildDrone" } };

		// Declares every layer; call before any key is resolved.
		inline bool Declare(IPluginSelf* self, IPluginHookScanner* scanner)
		{
			return PluginMap::DeclareFeatures(scanner, self,
				FogOfWar, PointsOfInterest, Foundables, AlienActors, Buildings, BuildingRestore,
				BuildingSplines, BuildingSprites, FollowBuildDrone);
		}
	}
}
