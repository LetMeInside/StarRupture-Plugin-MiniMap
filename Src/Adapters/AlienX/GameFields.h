#pragma once

#include "Native/NativeKeys.h"

#include "SDK/Engine_classes.hpp"
#include "SDK/Chimera_classes.hpp"
#include "SDK/Chimera_structs.hpp"
#include "SDK/AuActorPlacement_classes.hpp"
#include "SDK/AuActorPlacement_structs.hpp"
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
// not the plugin. Engine layout MiniMap keeps compiled is listed in the README.

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
	inline auto PlacementEntityType  = SR_FIELD(SDK::FAuAPMassSpawnedEntityType, UAuActorPlacementData, EntityType, 0);
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

	// Asks the loader for every row above. False when any did not resolve; the
	// loader has then recorded the failure and refuses the plugin or turns off
	// the feature that lists it.
	inline bool Resolve(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		const bool fog = PluginMap::ResolveField(scanner, self, FogData, kFogDataRows,
			static_cast<uint32_t>(sizeof(kFogDataRows) / sizeof(kFogDataRows[0])));
		return PluginMap::ResolveFields(scanner, self,
			PlayerMainCharacter, IsDead, DroneOwner, MapMenuDataReplicationHelper,
			AbandonBaseInfoReplicator, BuildingTags, PlacementEntityType, WalkwaySplineMesh) && fog;
	}

	using PluginMap::KeysOf;

	// The layers that can be off while the rest runs. Each lists the keys only it
	// needs; a key two layers need is listed in both. The terrain is not one:
	// it gives every other layer its transform, so a miss there leaves nothing
	// to draw and refuses the plugin. Declared in OnPluginLoadHooks before any key
	// is resolved. Every entry point of a layer checks On(), so an off layer runs
	// nothing and reads none of its keys or config entries.
	namespace Features
	{
		inline PluginMap::Feature FogOfWar{ "fog-of-war", "Fog of war on the minimap",
			KeysOf(FogData),
			{ "Fog Of War.DisableFog", "Fog Of War.FeatherRange" } };

		inline PluginMap::Feature PointsOfInterest{ "points-of-interest", "Points of interest on the minimap",
			KeysOf(Keys::GetAllActorsOfClass, Keys::PoiStaticClass, Keys::PoiCategory, Keys::MarkerFilter,
				Keys::AbandonBaseCompleted, AbandonBaseInfoReplicator),
			{ "Points Of Interest.ShowLayer", "Points Of Interest.MarkerScale" } };

		inline PluginMap::Feature Foundables{ "foundables", "Foundables on the minimap",
			KeysOf(Keys::GetAllActorsOfClass, Keys::FoundableCategory, Keys::MarkerFilter,
				Keys::MassSubsystem, Keys::QueryConstruct, Keys::QueryDestruct, Keys::MatchingHandles,
				Keys::FragmentDataPtr, Keys::ConstSharedPtr, Keys::AddInventoryRequirement,
				Keys::AddTransformRequirement, Keys::AddFoundableParameters, Keys::AddFoundableTag,
				Keys::AddTagRequirement, Keys::TransformStaticStruct, Keys::InventoryStaticStruct,
				Keys::FoundableStaticStruct, AbandonBaseInfoReplicator),
			{ "Foundables.ShowLayer", "Foundables.ShowLootInBases", "Foundables.MarkerScale" } };

		inline PluginMap::Feature AlienActors{ "alien-actors", "Alien actors on the minimap",
			KeysOf(Keys::MassSubsystem, Keys::QueryConstruct, Keys::QueryDestruct, Keys::MatchingHandles,
				Keys::FragmentDataPtr, Keys::AddTransformRequirement, Keys::AddEnemyStateRequirement,
				Keys::AddTagRequirement, Keys::TransformStaticStruct, Keys::EnemyStateStaticStruct,
				Keys::EnemyTagStaticStruct, Keys::NeutralTagStaticStruct),
			{ "Alien Actors.ShowLayer" } };

		inline PluginMap::Feature Buildings{ "buildings", "Buildings on the minimap",
			KeysOf(Keys::MassSubsystem, Keys::QueryConstruct, Keys::QueryDestruct, Keys::MatchingHandles,
				Keys::FragmentDataPtr, Keys::ConstSharedPtr, Keys::AddTransformRequirement,
				Keys::AddTagRequirement, Keys::IsEntityBuilt, Keys::IsEntityValid, Keys::GridSubsystem,
				Keys::GridForEachCell, BuildingTags, PlacementEntityType, WalkwaySplineMesh),
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
			KeysOf(Keys::SplinePayloadGet, Keys::SplinePayloadDestroy, Keys::SplinePayloadStaticStruct),
			{ "Buildings.RenderDroneRails" } };

		// ReadbackGlobals and ReleaseGuard are not requests: they name the checks that
		// report through the scanner, so a failed check turns this layer off. The loader
		// notes that no request asked for them.
		inline PluginMap::Feature BuildingSprites{ "building-sprites", "Building pictures on the minimap",
			KeysOf(Keys::SpawnObject, Keys::SetObjectFlags, Keys::RegisterComponent, Keys::UnregisterComponent,
				Keys::DestroyComponent, Keys::InitRenderTarget, Keys::RenderTargetResource,
				Keys::TransitionResources, Keys::FenceConstruct, Keys::FenceDestroy, Keys::FenceBegin,
				Keys::FencePoll, Keys::FromValidEName, Keys::SubmitRenderCommand, Keys::ImmediateCommandList,
				Keys::CreateRHITexture, Keys::ReadbackConstruct, Keys::ReadbackDestroy, Keys::ReadbackCopy,
				Keys::ReadbackLock, Keys::ReadbackUnlock, Keys::GpuFencePoll, Keys::MarkForDelete,
				Keys::MemoryMalloc, Keys::DynamicRHI, Keys::RenderCommandPipe,
				Keys::ThreadedRendering, Keys::RHIMultithreading, Keys::GpuCount, Keys::ReadbackGlobals,
				Keys::ReleaseGuard, Keys::CreateWorld, Keys::InitWorld, Keys::UpdateWorldComponents,
				Keys::CleanupWorld, Keys::WorldContext, Keys::GetSubsystemInternal,
				Keys::GarbageCollectingLocking, Keys::PurgePending, Keys::ReachabilityPending,
				Keys::TickableTickType, Keys::VisibilityPending, Keys::DestroyActor,
				Keys::ActiveGarbageCollection, Keys::EngineStorage),
			{ "Buildings.CaptureSprites" } };

		// Declares every layer; call before any key is resolved.
		inline bool Declare(IPluginSelf* self, IPluginHookScanner* scanner)
		{
			return PluginMap::DeclareFeatures(scanner, self,
				FogOfWar, PointsOfInterest, Foundables, AlienActors, Buildings, BuildingRestore,
				BuildingSplines, BuildingSprites);
		}
	}
}
