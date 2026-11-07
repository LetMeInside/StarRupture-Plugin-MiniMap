#pragma once

#include "SDK/Engine_classes.hpp"
#include <plugin_interface.h>
#include <plugin_map.h>

#include <cstdint>

// Engine members the building-sprite capture reads that the Game-SDK folds into
// padding (they are not reflected), so each is a native row of the loader's
// map: the world's flag bytes, the engine's world list and the world a context
// holds, and one flag of a render target. The engine moves them with its
// version or build configuration, not only with a game build. The loader holds
// a row per game build; the compiled one below is used only on the build the
// plugin was compiled for. All of them belong to the building-sprites layer,
// which is off on a build whose rows the map does not have yet.

namespace MiniMapEngineFields
{
	// TIndirectArray<FWorldContext>: the array header (the engine's TArray
	// layout) of pointers to the contexts.
	struct WorldContextArray
	{
		const uintptr_t* Data = nullptr;
		int32_t Num = 0;
		int32_t Max = 0;
	};

	constexpr PluginBitField FlagOf(const char* key, const char* className, const char* member)
	{
		return PluginBitField{ { key, className, member, PLUGIN_FIELD_NATIVE | PLUGIN_FIELD_BITFIELD,
			static_cast<uint32_t>(sizeof(uint8_t)), -1 } };
	}

	// EWorldType::Type, one byte; the world a capture owns is a GamePreview.
	inline auto WorldType = PluginField<uint8_t>{ { "UWorld::WorldType", "UWorld", "WorldType",
		PLUGIN_FIELD_NATIVE, sizeof(uint8_t), 0x18a } };
	inline auto WorldInitialized  = FlagOf("UWorld::bIsWorldInitialized", "UWorld", "bIsWorldInitialized");
	inline auto ActorsInitialized = FlagOf("UWorld::bActorsInitialized", "UWorld", "bActorsInitialized");
	inline auto TearingDown       = FlagOf("UWorld::bIsTearingDown", "UWorld", "bIsTearingDown");
	inline auto BegunPlay         = FlagOf("UWorld::bBegunPlay", "UWorld", "bBegunPlay");
	inline auto EverInitialized   = FlagOf("UWorld::bHasEverBeenInitialized", "UWorld", "bHasEverBeenInitialized");

	inline auto WorldList = PluginField<WorldContextArray>{ { "UEngine::WorldList", "UEngine", "WorldList",
		PLUGIN_FIELD_NATIVE, sizeof(WorldContextArray), 0x11c8 } };
	inline auto ContextWorld = PluginField<uintptr_t>{ { "FWorldContext::ThisCurrentWorld", "FWorldContext",
		"ThisCurrentWorld", PLUGIN_FIELD_NATIVE, sizeof(uintptr_t), 0x2c0 } };

	// The render target's "needs two copies" bit, the first flag of the dword
	// after TargetGamma.
	inline auto NeedsTwoCopies = FlagOf("UTextureRenderTarget::bNeedsTwoCopies", "UTextureRenderTarget", "bNeedsTwoCopies");

	// The rows for the build the plugin was compiled for: offset, size and, for
	// a flag, the bit's mask in its byte.
	inline const PluginFieldCandidate kWorldTypeRows[]         = { { 127004, 0x18a, 1, 0x00, nullptr } };
	inline const PluginFieldCandidate kWorldInitializedRows[]  = { { 127004, 0x18b, 1, 0x40, nullptr } };
	inline const PluginFieldCandidate kActorsInitializedRows[] = { { 127004, 0x18c, 1, 0x80, nullptr } };
	inline const PluginFieldCandidate kTearingDownRows[]       = { { 127004, 0x18d, 1, 0x20, nullptr } };
	inline const PluginFieldCandidate kBegunPlayRows[]         = { { 127004, 0x18d, 1, 0x01, nullptr } };
	inline const PluginFieldCandidate kEverInitializedRows[]   = { { 127004, 0x18f, 1, 0x02, nullptr } };
	inline const PluginFieldCandidate kWorldListRows[]         = { { 127004, 0x11c8, static_cast<uint32_t>(sizeof(WorldContextArray)), 0x00, nullptr } };
	inline const PluginFieldCandidate kContextWorldRows[]      = { { 127004, 0x2c0, static_cast<uint32_t>(sizeof(uintptr_t)), 0x00, nullptr } };
	inline const PluginFieldCandidate kNeedsTwoCopiesRows[]    = { { 127004, 0x144, 1, 0x01, nullptr } };

	// Asks the loader for every row above. False when any did not resolve; the
	// loader has then recorded the failure and turned off the layer that lists it.
	inline bool Resolve(const IPluginSelf* self, IPluginHookScanner* scanner)
	{
		const auto ask = [&](PluginFieldSpec& field, const auto& rows)
		{ return PluginMap::ResolveField(scanner, self, field, rows, static_cast<uint32_t>(sizeof(rows) / sizeof(rows[0]))); };
		bool all = ask(WorldType, kWorldTypeRows);
		all = ask(WorldInitialized, kWorldInitializedRows) && all;
		all = ask(ActorsInitialized, kActorsInitializedRows) && all;
		all = ask(TearingDown, kTearingDownRows) && all;
		all = ask(BegunPlay, kBegunPlayRows) && all;
		all = ask(EverInitialized, kEverInitializedRows) && all;
		all = ask(WorldList, kWorldListRows) && all;
		all = ask(ContextWorld, kContextWorldRows) && all;
		all = ask(NeedsTwoCopies, kNeedsTwoCopiesRows) && all;
		return all;
	}
}
