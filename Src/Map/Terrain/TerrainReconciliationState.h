// Shares private terrain lifecycle state with source reconciliation.
#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "TerrainReconciliation.h"
#include "TerrainChunks.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
namespace MiniMapTerrainSource::TerrainReconciliationState
{
    extern bool g_initialized;
    extern float g_updateAccumulator;
    extern std::mutex g_renderMutex;
    extern std::vector<TerrainReconciliation::LoadedChunk> g_loadedChunks;
    extern std::vector<std::unique_ptr<TerrainReconciliation::ResidentChunk>> g_residentChunks;
    extern uint64_t g_renderGeneration;
    extern float g_viewportPixelWidth;
    extern float g_viewportPixelHeight;
    extern double g_metersPerPixel;
    extern std::atomic<bool> g_reconcileRequested;
    extern bool g_hasViewportAnchor;
    extern bool g_registeredPresentation;
    extern SDK::FVector g_viewportAnchorWorldPosition;
    extern double g_viewportAnchorGlobalChunkX;
    extern double g_viewportAnchorGlobalChunkY;
    extern double g_playerGlobalChunkX;
    extern double g_playerGlobalChunkY;
    extern MiniMapMap::PresentationSample g_presentation;
    extern uint64_t g_presentationRevision;
    extern uint64_t g_reconciliationSequence;
    inline constexpr float kUpdateIntervalSeconds = 0.25f;
    inline constexpr double kChunkWorldUnits = static_cast<double>(MiniMapTerrainChunks::kChunkWorldSizeMeters) * 100.0;
    inline constexpr double kDefaultMetersPerPixel = 100.0 / 300.0;
    inline constexpr double kMinimumMetersPerPixel = MiniMapMap::MinimumMetersPerPixel;
    inline constexpr double kMaximumMetersPerPixel = MiniMapMap::MaximumMetersPerPixel;
    inline constexpr float kDefaultViewportPixelWidth = 300.0f;
    inline constexpr float kDefaultViewportPixelHeight = 300.0f;
}
#endif
