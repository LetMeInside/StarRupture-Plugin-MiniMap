// Owns terrain ticks, resident receipts and presentation; registered images share the same renderer.
#include "Services/Viewport.h"
#if defined(MODLOADER_CLIENT_BUILD)

#include "Terrain.h"
#include "Config/Config.h"
#include "TerrainProfile.h"
#include "Input/MouseWheel.h"
#include "UI/MiniMapUI.h"

#include "Map/Map.h"
#include "Map/MapTransform.h"
#include "TerrainCache.h"
#include "TerrainChunks.h"
#include "TerrainDiagnostics.h"
#include "TerrainSource.h"
#include "RegisteredTerrainSource.h"
#include "TerrainReconciliation.h"
#include "TerrainReconciliationState.h"

#include "plugin.h"
#include "plugin_helpers.h"
#include "Services/Runtime.h"
#include "Services/RuntimeEvents.h"

#include "SDK/Chimera_classes.hpp"
#include "SDK/Engine_classes.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <vector>

using namespace MiniMapTerrainSource::TerrainReconciliationState;

namespace
{
    MiniMap::Services::Registration g_tickRegistration{};
    using MiniMapTerrainSource::TerrainReconciliation;
    using MiniMapTerrainSource::RegisteredTerrainSource;
    using MiniMapTerrainSource::RegisteredTerrainImage;
    using ChunkIdentity = TerrainReconciliation::ChunkIdentity;
    using LoadedChunk = TerrainReconciliation::LoadedChunk;
    using ResidentState = TerrainReconciliation::ResidentState;
    using ResidentChunk = TerrainReconciliation::ResidentChunk;
    using ReconciliationContext = TerrainReconciliation::ReconciliationContext;

    uint64_t g_zoomWorldGeneration = 0; // Game-thread session boundary only.
    bool g_tickRegistered = false;
    MiniMapMap::PresentationSample g_coordinatePresentation = {};

    bool UpdateFastViewportCenter(
        const MiniMapMap::PresentationSample& sample,
        uint64_t revision)
    {
        std::lock_guard<std::mutex> lock(
            g_renderMutex);

        if (revision != g_presentationRevision || !MiniMapUI::IsVisible() ||
            !MiniMapMap::GameplayReady() || sample.WorldGeneration != MiniMapMap::GetWorldGeneration()) return false;
        if (!g_presentation.Valid || g_presentation.DroneActive != sample.DroneActive ||
            g_presentation.TrackingObjectIndex != sample.TrackingObjectIndex)
            g_reconcileRequested.store(true, std::memory_order_release);
        g_presentation = sample;
        const auto& playerPose = sample.Tracking;

        if (!g_hasViewportAnchor || g_registeredPresentation)
        {
            return true;
        }

        g_playerGlobalChunkX =
            g_viewportAnchorGlobalChunkX +
            (playerPose.WorldX -
                g_viewportAnchorWorldPosition.X) /
            kChunkWorldUnits;

        g_playerGlobalChunkY =
            g_viewportAnchorGlobalChunkY +
            (playerPose.WorldY -
                g_viewportAnchorWorldPosition.Y) /
            kChunkWorldUnits;
        return true;
    }


    void OnTerrainTick(
        float deltaSeconds)
    {
        TerrainReconciliation::SynchronizeSource();
        if (!MiniMapUI::IsVisible() || !MiniMapMap::GameplayReady() || !MiniMapMap::HasWorld())
        {
            MiniMapTerrain::InvalidatePresentation();
            MiniMapMouseWheel::ClearPendingZoom();
            return;
        }

        MiniMapMap::PresentationSample sample{};
        if (!MiniMapMap::TryGetPresentationSample(sample))
        {
            MiniMapTerrain::InvalidatePresentation();
            MiniMapMouseWheel::ClearPendingZoom();
            return;
        }

        {
            std::lock_guard lock(g_renderMutex);
            g_coordinatePresentation = sample;
        }

        const bool forgottenEngineFloorSelected =
            RegisteredTerrainSource::SynchronizeForgottenEngine(
                sample.InForgottenEngine, sample.Character.WorldZ);
        if (sample.InForgottenEngine && !forgottenEngineFloorSelected)
        {
            MiniMapTerrain::InvalidatePresentation();
            MiniMapMouseWheel::ClearPendingZoom();
            return;
        }

        uint64_t revision;
        {
            std::lock_guard lock(g_renderMutex);
            revision = g_presentationRevision;
        }

        const float zoomDelta =
            MiniMapMouseWheel::DrainZoomDelta();

        if (zoomDelta != 0.0f)
        {
            MiniMapTerrain::AdjustZoom(
                zoomDelta);
        }

        if (!UpdateFastViewportCenter(sample, revision)) return;

        TerrainReconciliation::AdvanceDemand(sample, revision, deltaSeconds);
    }
}


namespace MiniMapTerrain
{
    bool TryGetVisibleWorldBounds(double& x1, double& y1, double& x2, double& y2)
    {
        x1 = y1 = x2 = y2 = 0.0;
        const auto settings = MiniMapConfig::GetRenderSettings();
        if (!MiniMapMap::GameplayReady() || !MiniMapUI::IsVisible())
        {
            return false;
        }
        std::lock_guard<std::mutex> lock(g_renderMutex);
        if (!g_initialized || !g_hasViewportAnchor || !g_presentation.Valid ||
            g_presentation.WorldGeneration != MiniMapMap::GetWorldGeneration() ||
            g_presentation.TrackingRevision != settings.TrackingRevision ||
            !std::isfinite(g_presentation.Tracking.WorldX) ||
            !std::isfinite(g_presentation.Tracking.WorldY) ||
            !std::isfinite(g_metersPerPixel) || g_metersPerPixel <= 0.0 ||
            !std::isfinite(g_viewportPixelWidth) || g_viewportPixelWidth <= 0.0f ||
            !std::isfinite(g_viewportPixelHeight) || g_viewportPixelHeight <= 0.0f)
        {
            return false;
        }
        const double rotationDegrees = settings.RotatingMiniMap
            ? MiniMapMap::HeadingUpMapRotationDegrees(g_presentation.Tracking.ControlYawDegrees)
            : 0.0;
        constexpr double degreesToRadians = 3.1415926535897932384626433832795 / 180.0;
        const double angle = rotationDegrees * degreesToRadians;
        const double cosine = std::abs(std::cos(angle));
        const double sine = std::abs(std::sin(angle));
        const double halfWorldWidth = static_cast<double>(g_viewportPixelWidth) * g_metersPerPixel * 100.0 / 2.0;
        const double halfWorldHeight = static_cast<double>(g_viewportPixelHeight) * g_metersPerPixel * 100.0 / 2.0;
        const double halfX = cosine * halfWorldWidth + sine * halfWorldHeight;
        const double halfY = sine * halfWorldWidth + cosine * halfWorldHeight;
        const double centerX = g_presentation.Tracking.WorldX;
        const double centerY = g_presentation.Tracking.WorldY;
        const double left = centerX - halfX;
        const double top = centerY - halfY;
        const double right = centerX + halfX;
        const double bottom = centerY + halfY;
        if (!std::isfinite(rotationDegrees) || !std::isfinite(halfX) || !std::isfinite(halfY) ||
            !std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) || !std::isfinite(bottom) ||
            right <= left || bottom <= top)
        {
            return false;
        }
        x1 = left;
        y1 = top;
        x2 = right;
        y2 = bottom;
        return true;
    }

    void InvalidatePresentation()
    {
        std::lock_guard<std::mutex> lock(g_renderMutex);
        g_presentation = {};
        g_coordinatePresentation = {};
        ++g_presentationRevision;
    }

    bool Initialize(
        IPluginSelf* self)
    {
        const double initialZoom = MiniMapConfig::InitialZoom(); // Before the render mutex.
        if (g_initialized)
        {
            // A replacement world can be admitted before the previous world's
            // EndPlay. Reapply session zoom without restarting terrain acquisition.
            const auto generation = MiniMapMap::GetWorldGeneration();
            if (g_zoomWorldGeneration != generation)
            {
                std::lock_guard lock(g_renderMutex);
                g_metersPerPixel = initialZoom;
                g_presentation = {};
                g_coordinatePresentation = {};
                ++g_presentationRevision;
                g_hasViewportAnchor = false;
                g_zoomWorldGeneration = generation;
                g_reconcileRequested.store(true, std::memory_order_release);
            }
            return true;
        }

        g_zoomWorldGeneration = MiniMapMap::GetWorldGeneration();
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        MiniMapTerrainProfile::Reset();
#endif
        g_updateAccumulator =
            kUpdateIntervalSeconds;

        {
            std::lock_guard<std::mutex> lock(
                g_renderMutex);

            g_viewportPixelWidth =
                kDefaultViewportPixelWidth;

            g_viewportPixelHeight =
                kDefaultViewportPixelHeight;

            g_metersPerPixel =
                initialZoom;

            g_hasViewportAnchor =
                false;

            g_viewportAnchorWorldPosition = {};

            g_viewportAnchorGlobalChunkX =
                0.0;

            g_viewportAnchorGlobalChunkY =
                0.0;

            g_playerGlobalChunkX =
                0.0;

            g_playerGlobalChunkY =
                0.0;

            g_presentation = {};
            g_coordinatePresentation = {};
            ++g_presentationRevision;
        }

        g_reconcileRequested.store(
            false,
            std::memory_order_release);

        const bool terrainSelfAvailable=self!=nullptr;
        auto* terrainHooks=terrainSelfAvailable?self->hooks:nullptr;
        const bool terrainHooksAvailable=terrainHooks!=nullptr;
        auto* terrainTextures=MiniMap::Services::TryTextures();
        const bool terrainEngineAvailable=MiniMap::Services::TryRuntimeEvents()!=nullptr;
        const bool terrainTexturesAvailable=terrainTextures && terrainTextures->Available();
        if (!terrainSelfAvailable || !terrainHooksAvailable ||
            !terrainEngineAvailable || !terrainTexturesAvailable)
        {
            MiniMap::Adapters::AlienX::LogTerrainHookAvailability(self);

            return false;
        }

        if (!MiniMapTerrainSource::Initialize(
            self))
        {
            LOG_ERROR(
                "MiniMap: terrain source initialization failed");

            return false;
        }

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        if (!MiniMapTerrainDiagnostics::Initialize(
            self))
        {
            MiniMapTerrainSource::Shutdown();

            return false;
        }
#endif

        if (!g_tickRegistered)
        {
            const auto tickReceipt = MiniMap::Services::TryRuntimeEvents()->OnTick(
                [](void*, float delta) { OnTerrainTick(delta); }, nullptr);
            if (tickReceipt.token.id) g_tickRegistration = tickReceipt.token;
            if (tickReceipt.acceptance == MiniMap::Services::Acceptance::Rejected ||
                tickReceipt.acceptance == MiniMap::Services::Acceptance::BackendUnknown)
            {
                if (g_tickRegistration.id) MiniMap::Services::TryRuntimeEvents()->Remove(g_tickRegistration);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                MiniMapTerrainDiagnostics::Shutdown();
#endif
                MiniMapTerrainSource::Shutdown();
                return false;
            }

            g_tickRegistered =
                true;

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            LOG_INFO(
                "MiniMap: registered terrain update tick");
#endif
        }

        g_initialized =
            true;

        return true;
    }


    void Shutdown()
    {
        if (!g_initialized)
        {
            return;
        }

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        MiniMapTerrainProfile::Stop();
#endif
        MiniMapMouseWheel::ClearPendingZoom();

        if (g_tickRegistered)
        {
            if (auto* events = MiniMap::Services::TryRuntimeEvents())
            {
                const auto removal = events->Remove(g_tickRegistration);
                if (removal.registry == MiniMap::Services::RegistryRemoval::RequestReturned &&
                    removal.core == MiniMap::Services::CoreDrain::Quiescent) g_tickRegistration = {};
            }

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            LOG_INFO(
                "MiniMap: unregistered terrain update tick");
#endif
        }

        g_tickRegistered =
            false;

        {
            std::lock_guard<std::mutex> lock(
                g_renderMutex);

            // Stop exposing terrain textures to the MiniMap render path, but
            // retain resident ownership and unresolved debt. Dynamic retirement
            // remains the reconciliation policy; v70 manages GPU retirement.
            g_loadedChunks.clear();
            g_presentation = {};
            ++g_presentationRevision;

            // A later gameplay world may reuse matching residents. Inactive
            // residents will be scheduled again by ReconcileViewport against
            // the new render-generation sequence.
            for (const auto& resident : g_residentChunks)
            {
                resident->Descriptor.RetireAfterRenderGeneration = 0;
            }

            g_renderGeneration =
                0;
        }

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        MiniMapTerrainDiagnostics::Shutdown();
#endif
        MiniMapTerrainSource::Shutdown();

        g_updateAccumulator =
            0.0f;

        g_reconcileRequested.store(
            false,
            std::memory_order_release);

        {
            std::lock_guard<std::mutex> lock(
                g_renderMutex);

            g_viewportPixelWidth =
                kDefaultViewportPixelWidth;

            g_viewportPixelHeight =
                kDefaultViewportPixelHeight;

            g_metersPerPixel =
                kDefaultMetersPerPixel;

            g_hasViewportAnchor =
                false;

            g_viewportAnchorWorldPosition = {};

            g_viewportAnchorGlobalChunkX =
                0.0;

            g_viewportAnchorGlobalChunkY =
                0.0;

            g_playerGlobalChunkX =
                0.0;

            g_playerGlobalChunkY =
                0.0;

            g_presentation = {};
            ++g_presentationRevision;
        }

        g_initialized =
            false;
    }


#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
    bool RegisterDiagnostics(
        IPluginSelf* self)
    {
        return MiniMapTerrainDiagnostics::RegisterDiagnostics(
            self);
    }


    void UnregisterDiagnostics()
    {
        MiniMapTerrainDiagnostics::UnregisterDiagnostics();
    }


    void CancelPendingDiagnostic()
    {
        MiniMapTerrainDiagnostics::CancelPendingDiagnostic();
    }


#endif

    void AdjustZoom(
        float wheelDelta)
    {
        const double stepFactor = MiniMapConfig::ZoomStepFactor(); // Before the render mutex.
        if (!std::isfinite(wheelDelta) || wheelDelta == 0.0f)
        {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(
                g_renderMutex);

            const double zoomMultiplier =
                std::pow(
                    stepFactor,
                    static_cast<double>(
                        wheelDelta));

            const double newMetersPerPixel =
                std::clamp(
                    g_metersPerPixel /
                    zoomMultiplier,
                    kMinimumMetersPerPixel,
                    kMaximumMetersPerPixel);

            if (std::abs(
                newMetersPerPixel -
                g_metersPerPixel) <
                1.0e-9)
            {
                return;
            }

            g_metersPerPixel =
                newMetersPerPixel;
        }

        g_reconcileRequested.store(
            true,
            std::memory_order_release);
    }


    // Builds the common map transform and draws selected terrain under render exclusion.
    bool Render(
        MiniMap::Services::ICanvas& ui, float windowX, float windowY,
        float windowWidth, float windowHeight, MiniMapMap::Transform& outTransform,
        const MiniMapConfig::RenderSettings& settings)
    {
        outTransform = {};
        if (windowWidth <= 0.0f || windowHeight <= 0.0f) return false;
        std::lock_guard lock(g_renderMutex);
        // Counts completed callbacks, including withdrawn/empty presentation; never a GPU fence.
        struct RenderGenerationGuard
        {
            // Advances progress on every exit while the render mutex remains held.
            ~RenderGenerationGuard() { ++g_renderGeneration; }
        };
        RenderGenerationGuard renderGenerationGuard;
        g_viewportPixelWidth = windowWidth;
        g_viewportPixelHeight = windowHeight;
        if (MiniMapMap::GameplayReady() && g_coordinatePresentation.Valid &&
            g_coordinatePresentation.WorldGeneration == MiniMapMap::GetWorldGeneration() &&
            g_coordinatePresentation.TrackingRevision == settings.TrackingRevision)
        {
            const double coordinateRotation = settings.RotatingMiniMap
                ? MiniMapMap::HeadingUpMapRotationDegrees(
                    g_coordinatePresentation.Tracking.ControlYawDegrees)
                : 0.0;
            MiniMapMap::BuildTransform(g_coordinatePresentation, coordinateRotation,
                g_metersPerPixel, windowX, windowY, windowWidth, windowHeight, outTransform);
        }
        if (!g_hasViewportAnchor || !MiniMapMap::GameplayReady() || !g_presentation.Valid ||
            g_presentation.WorldGeneration != MiniMapMap::GetWorldGeneration() ||
            g_presentation.TrackingRevision != settings.TrackingRevision) return false;
        const double rotation = settings.RotatingMiniMap ? MiniMapMap::HeadingUpMapRotationDegrees(
            g_presentation.Tracking.ControlYawDegrees) : 0.0;
        if (!MiniMapMap::BuildTransform(g_presentation, rotation, g_metersPerPixel,
            windowX, windowY, windowWidth, windowHeight, outTransform)) return false;
        TerrainReconciliation::DrawLoadedChunks(ui, outTransform,
            windowX, windowY, windowWidth, windowHeight);
        return true;
    }
}

#endif
