#pragma once

#include "SDK/Engine_classes.hpp"
#include <plugin_interface.h>
#include <plugin_map.h>

#include <cstdint>
#include <vector>

// The Unreal Engine members the building-sprite capture reads from the worlds,
// levels, subsystems, actors and components of its private world, as the
// loader's map places them for the running game build. The reflected ones are
// asked from the game's own metadata and need no row. The rest the Game-SDK
// folds into padding (they are not reflected), so each is a native row of the
// loader's map: the engine moves them with its version or build configuration,
// not only with a game build. The loader holds a row per game build; the compiled
// one below is used only on the build the plugin was compiled for. All of them
// belong to the building-sprites layer, which is off on a build whose rows the
// map does not have yet. The exceptions are WorldGameState and ActorRootComponent,
// which only the foundables layer reads, and ActorBeingDestroyed and OwningWorld,
// which both read.

namespace MiniMapEngineFields
{
	// The engine's TArray header: the data pointer, the count and the capacity.
	struct ArrayHeader
	{
		uintptr_t Data = 0;
		int32_t Num = 0;
		int32_t Max = 0;
	};
	// TIndirectArray<FWorldContext>: the array header of pointers to the contexts.
	using WorldContextArray = ArrayHeader;

	// Members that are plain engine containers, held by value, of which only their
	// own members are read: sized as the compiled engine has them.
	struct SubsystemCollectionBytes { uint8_t Bytes[0xc0]; };
	struct SubsystemMapBytes { uint8_t Bytes[0x50]; };

	// A native member of T, at `offset` on the build the plugin was compiled for.
	#define MM_NATIVE(T, Class, Member, offset) \
		PluginField<T>{ { #Class "::" #Member, #Class, #Member, PLUGIN_FIELD_NATIVE, sizeof(T), (offset) } }
	// A native flag: one byte and a mask, both from the row.
	#define MM_NATIVE_FLAG(Class, Member) \
		PluginBitField{ { #Class "::" #Member, #Class, #Member, PLUGIN_FIELD_NATIVE | PLUGIN_FIELD_BITFIELD, \
			static_cast<uint32_t>(sizeof(uint8_t)), -1 } }

	// Reflected: the game's own metadata gives them on every build.
	inline auto PersistentLevel        = SR_FIELD(uintptr_t, UWorld, PersistentLevel, 0);
	inline auto NetDriver              = SR_FIELD(uintptr_t, UWorld, NetDriver, 0);
	inline auto StreamingLevels        = SR_FIELD(ArrayHeader, UWorld, StreamingLevels, 0);
	// Held by value; only its own members are read, each through its own key.
	inline auto StreamingLevelsToConsider = PluginField<SDK::FStreamingLevelsToConsider>{ SR_FIELD_ANY_SIZE(UWorld, StreamingLevelsToConsider, 0) };
	inline auto ConsideredLevels       = SR_FIELD(ArrayHeader, FStreamingLevelsToConsider, StreamingLevels, 0);
	inline auto OwningWorld            = SR_FIELD(uintptr_t, ULevel, OwningWorld, 0);
	inline auto LevelWorldSettings     = SR_FIELD(uintptr_t, ULevel, WorldSettings, 0);
	inline auto EngineWorldSettingsClass = SR_FIELD(uintptr_t, UEngine, WorldSettingsClass, 0);
	inline auto ActorBeingDestroyed    = SR_BITFIELD(AActor, bActorIsBeingDestroyed);
	// What the foundables layer reads to tell its actors from the world's: the world's
	// game state and an actor's root component.
	inline auto WorldGameState         = SR_FIELD(SDK::AGameStateBase*, UWorld, GameState, 0);
	inline auto ActorRootComponent     = SR_FIELD(SDK::USceneComponent*, AActor, RootComponent, 0);
	// The sprite capture checks that its actors are unowned, and its world's game instance.
	inline auto WorldGameInstance      = SR_FIELD(uintptr_t, UWorld, OwningGameInstance, 0);
	inline auto ActorOwner             = SR_FIELD(uintptr_t, AActor, Owner, 0);
	inline auto ActorInstigator        = SR_FIELD(uintptr_t, AActor, Instigator, 0);
	// The capture component's target and source, and the render target it draws into.
	inline auto CaptureTarget          = SR_FIELD(uintptr_t, USceneCaptureComponent2D, TextureTarget, 0);
	inline auto CaptureSource          = SR_FIELD(SDK::ESceneCaptureSource, USceneCaptureComponent, CaptureSource, 0);
	inline auto TargetGamma            = SR_FIELD(float, UTextureRenderTarget, TargetGamma, 0);
	inline auto TargetClearColor       = SR_FIELD(SDK::FLinearColor, UTextureRenderTarget2D, ClearColor, 0);
	inline auto TargetSupportsUAV      = SR_BITFIELD(UTextureRenderTarget2D, bSupportsUAV);
	inline auto TargetAutoMips         = SR_BITFIELD(UTextureRenderTarget2D, bAutoGenerateMips);

	// EWorldType::Type, one byte; the world a capture owns is a GamePreview.
	inline auto WorldType = MM_NATIVE(uint8_t, UWorld, WorldType, 0x18a);
	inline auto WorldInitialized  = MM_NATIVE_FLAG(UWorld, bIsWorldInitialized);
	inline auto ActorsInitialized = MM_NATIVE_FLAG(UWorld, bActorsInitialized);
	inline auto TearingDown       = MM_NATIVE_FLAG(UWorld, bIsTearingDown);
	inline auto BegunPlay         = MM_NATIVE_FLAG(UWorld, bBegunPlay);
	inline auto EverInitialized   = MM_NATIVE_FLAG(UWorld, bHasEverBeenInitialized);

	inline auto WorldList = MM_NATIVE(ArrayHeader, UEngine, WorldList, 0x11c8);
	inline auto ContextWorld = MM_NATIVE(uintptr_t, FWorldContext, ThisCurrentWorld, 0x2c0);

	// The render target's "needs two copies" bit, the first flag of the dword
	// after TargetGamma.
	inline auto NeedsTwoCopies = MM_NATIVE_FLAG(UTextureRenderTarget, bNeedsTwoCopies);

	// UTickableWorldSubsystem::bInitialized, one byte: set once the subsystem is
	// initialised, clear after it is cleaned up.
	inline auto TickableInitialized = MM_NATIVE(uint8_t, UTickableWorldSubsystem, bInitialized, 0x38);

	// The world's scene, its subsystem collection (the last member of the world)
	// and the streaming bookkeeping that must be empty before cleanup.
	inline auto WorldScene             = MM_NATIVE(uintptr_t, UWorld, Scene, 0x258);
	inline auto SubsystemCollection    = MM_NATIVE(SubsystemCollectionBytes, UWorld, SubsystemCollection, 0x9f0);
	inline auto LevelsToProcess        = MM_NATIVE(ArrayHeader, FStreamingLevelsToConsider, LevelsToProcess, 0x10);
	inline auto LevelsBeingConsidered  = MM_NATIVE(int32_t, FStreamingLevelsToConsider, StreamingLevelsBeingConsidered, 0x20);

	// The collection's own members, relative to the collection: the two maps, its
	// outer object and the two flags that say it is being filled or walked.
	inline auto CollectionMap          = MM_NATIVE(SubsystemMapBytes, FSubsystemCollectionBase, SubsystemMap, 0x8);
	inline auto CollectionArrayMap     = MM_NATIVE(SubsystemMapBytes, FSubsystemCollectionBase, SubsystemArrayMap, 0x58);
	inline auto CollectionOuter        = MM_NATIVE(uintptr_t, FSubsystemCollectionBase, Outer, 0xb0);
	inline auto CollectionPopulating   = MM_NATIVE(uint8_t, FSubsystemCollectionBase, bPopulating, 0xb8);
	inline auto CollectionIterating    = MM_NATIVE(uint8_t, FSubsystemCollectionBase, bIterating, 0xb9);
	// What a subsystem records of the collection that owns it.
	inline auto SubsystemOwner         = MM_NATIVE(uintptr_t, USubsystem, InternalOwningSubsystem, 0x28);

	// The level's actors and a class's parent.
	inline auto LevelActors            = MM_NATIVE(ArrayHeader, ULevel, Actors, 0xa0);
	inline auto SuperStruct            = MM_NATIVE(uintptr_t, UStruct, SuperStruct, 0x40);

	// Actors and components: the owner and world a component records, the state
	// bits of an actor and of a component, and what a light component holds of
	// the scene it is added to.
	inline auto ComponentOwner         = MM_NATIVE(uintptr_t, UActorComponent, OwnerPrivate, 0xa0);
	inline auto ComponentWorld         = MM_NATIVE(uintptr_t, UActorComponent, WorldPrivate, 0xa8);
	inline auto ComponentRegistered    = MM_NATIVE_FLAG(UActorComponent, bRegistered);
	inline auto RenderStateCreated     = MM_NATIVE_FLAG(UActorComponent, bRenderStateCreated);
	inline auto ComponentInitialized   = MM_NATIVE_FLAG(UActorComponent, bHasBeenInitialized);
	inline auto ComponentBegunPlay     = MM_NATIVE_FLAG(UActorComponent, bHasBegunPlay);
	inline auto LightSceneProxy        = MM_NATIVE(uintptr_t, ULightComponent, SceneProxy, 0x340);
	inline auto LightAddedToScene      = MM_NATIVE_FLAG(ULightComponent, bAddedToSceneVisible);
	inline auto ActorFinishedSpawning  = MM_NATIVE_FLAG(AActor, bHasFinishedSpawning);
	inline auto ActorInitialized       = MM_NATIVE_FLAG(AActor, bActorInitialized);
	// A two-bit state (not begun, begun, ...) of which any bit means play has begun.
	inline auto ActorBeginPlay         = MM_NATIVE_FLAG(AActor, ActorHasBegunPlay);

	// The row of a flag for the build the plugin was compiled for: its byte and
	// the bit's mask in it.
	struct FlagRow { PluginBitField* flag; int32_t offset; uint8_t mask; };

	// Every key of this header, for the layer that lists them.
	inline std::vector<const char*> Keys()
	{
		return PluginMap::KeysOf(
			PersistentLevel, NetDriver, StreamingLevels, StreamingLevelsToConsider, ConsideredLevels, OwningWorld,
			LevelWorldSettings, EngineWorldSettingsClass, ActorBeingDestroyed,
			WorldGameInstance, ActorOwner, ActorInstigator, CaptureTarget, CaptureSource, TargetGamma, TargetClearColor,
			TargetSupportsUAV, TargetAutoMips,
			WorldType, WorldInitialized, ActorsInitialized, TearingDown, BegunPlay, EverInitialized,
			WorldList, ContextWorld, NeedsTwoCopies, TickableInitialized,
			WorldScene, SubsystemCollection, LevelsToProcess, LevelsBeingConsidered,
			CollectionMap, CollectionArrayMap, CollectionOuter, CollectionPopulating, CollectionIterating, SubsystemOwner,
			LevelActors, SuperStruct, ComponentOwner, ComponentWorld, ComponentRegistered, RenderStateCreated,
			ComponentInitialized, ComponentBegunPlay, LightSceneProxy, LightAddedToScene,
			ActorFinishedSpawning, ActorInitialized, ActorBeginPlay);
	}

	// Asks the loader for every row above. False when any did not resolve; the
	// loader has then recorded the failure and turned off the layer that lists it.
	inline bool Resolve(const IPluginSelf* self, IPluginHookScanner* scanner)
	{
		bool all = PluginMap::ResolveFields(scanner, self,
			PersistentLevel, NetDriver, StreamingLevels, StreamingLevelsToConsider, ConsideredLevels, OwningWorld,
			LevelWorldSettings, EngineWorldSettingsClass, ActorBeingDestroyed, WorldGameState, ActorRootComponent,
			WorldGameInstance, ActorOwner, ActorInstigator, CaptureTarget, CaptureSource, TargetGamma, TargetClearColor,
			TargetSupportsUAV, TargetAutoMips);

		// A member's row is its compiled offset and size.
		PluginFieldSpec* const members[] = {
			&WorldType, &WorldList, &ContextWorld, &TickableInitialized, &WorldScene, &SubsystemCollection,
			&LevelsToProcess, &LevelsBeingConsidered, &CollectionMap, &CollectionArrayMap, &CollectionOuter,
			&CollectionPopulating, &CollectionIterating, &SubsystemOwner, &LevelActors, &SuperStruct,
			&ComponentOwner, &ComponentWorld, &LightSceneProxy };
		for (PluginFieldSpec* field : members)
		{
			const PluginFieldCandidate row{ 127004, field->builtOffset, field->size, 0x00, nullptr };
			all = PluginMap::ResolveField(scanner, self, *field, &row, 1) && all;
		}

		const FlagRow flags[] = {
			{ &WorldInitialized, 0x18b, 0x40 }, { &ActorsInitialized, 0x18c, 0x80 }, { &TearingDown, 0x18d, 0x20 },
			{ &BegunPlay, 0x18d, 0x01 }, { &EverInitialized, 0x18f, 0x02 }, { &NeedsTwoCopies, 0x144, 0x01 },
			{ &ComponentRegistered, 0x94, 0x01 }, { &RenderStateCreated, 0x94, 0x02 },
			{ &ComponentInitialized, 0x97, 0x08 }, { &ComponentBegunPlay, 0x97, 0x20 },
			{ &LightAddedToScene, 0x3a0, 0x01 }, { &ActorFinishedSpawning, 0x5c, 0x01 },
			{ &ActorInitialized, 0x5c, 0x02 }, { &ActorBeginPlay, 0x5d, 0x18 } };
		for (const FlagRow& flag : flags)
		{
			const PluginFieldCandidate row{ 127004, flag.offset, flag.flag->size, flag.mask, nullptr };
			all = PluginMap::ResolveField(scanner, self, *flag.flag, &row, 1) && all;
		}
		return all;
	}

	#undef MM_NATIVE
	#undef MM_NATIVE_FLAG
}
