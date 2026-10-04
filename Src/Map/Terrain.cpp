#if defined(MODLOADER_CLIENT_BUILD)

#include "Terrain.h"
#include "Map.h"

#include "SDK/Chimera_classes.hpp"
#include "SDK/Engine_classes.hpp"
#include "SDK/ChimeraUI_classes.hpp"

#include <cmath>

#include "../plugin.h"
#include "../plugin_helpers.h"

#include <atomic>
#include <cstdint>

namespace
{
    IPluginSelf* g_terrainSelf = nullptr;
    SDK::UCrMapMenuTerrainData* g_terrainData = nullptr;

    PluginTextureHandle g_terrainTexture = nullptr;

    std::atomic<bool> g_diagnosticPending = false;

    bool g_diagnosticKeyRegistered = false;
    bool g_tickRegistered = false;

    constexpr const char* kDiagnosticTextureName =
        "MiniMap_Terrain_Current";

    constexpr const char* kDiagnosticKey = "F8";


    using LoadSynchronousFn =
        SDK::UObject* (*)(void*);

    using GetBrushTextureFn =
        SDK::UTexture2D* (*)(
            const SDK::FSlateBrush&);

    using GetNumResidentMipsFn =
        int32_t(*)(const SDK::UTexture2D*);

    using GetNumMipsAllowedFn =
        int32_t(*)(const SDK::UTexture2D*,
            bool);

    using GetNumMipsFn =
        int32_t(*)(const SDK::UTexture2D*);

    using StreamInFn =
        bool (*)(SDK::UTexture2D*,
            int32_t,
            bool);

    using WaitForPendingInitOrStreamingFn =
        void (*)(SDK::UStreamableRenderAsset*,
            bool,
            bool);


    SDK::UCrMapMenuDevSettings* FindMapMenuDevSettingsCDO()
    {
        if (g_terrainSelf == nullptr ||
            g_terrainSelf->hooks == nullptr ||
            g_terrainSelf->hooks->ObjectWalker == nullptr)
        {
            LOG_ERROR(
                "MiniMap: ObjectWalker is unavailable");

            return nullptr;
        }

        auto* walker =
            g_terrainSelf->hooks->ObjectWalker;

        if (!walker->IsReady())
        {
            LOG_ERROR(
                "MiniMap: ObjectWalker is not ready");

            return nullptr;
        }

        PluginObjectInfo objects[8] = {};

        const int count =
            walker->FindObjectsByClassNameInto(
                "CrMapMenuDevSettings",
                PluginObjectLookup_CDOOnly,
                objects,
                8);

        LOG_INFO(
            "MiniMap: found %d CrMapMenuDevSettings CDO(s)",
            count);

        const int loggedCount =
            count < 8 ? count : 8;

        for (int i = 0; i < loggedCount; ++i)
        {
            LOG_INFO(
                "MiniMap: DevSettings CDO [%d]: "
                "object=%p name=%s class=%s",
                i,
                objects[i].object,
                objects[i].objectName,
                objects[i].className);
        }

        if (count != 1 ||
            objects[0].object == nullptr)
        {
            LOG_WARN(
                "MiniMap: expected exactly one "
                "CrMapMenuDevSettings CDO");

            return nullptr;
        }

        return static_cast<SDK::UCrMapMenuDevSettings*>(
            objects[0].object);
    }


    void ProcessTerrainDiagnostic()
    {
        LOG_INFO(
            "MiniMap: processing F8 terrain diagnostic "
            "on game-thread tick");

        if (g_terrainSelf == nullptr ||
            g_terrainSelf->hooks == nullptr)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "plugin hooks are unavailable");

            return;
        }

        if (!MiniMapMap::HasWorld())
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "active gameplay world is unavailable");

            return;
        }

        if (g_terrainTexture != nullptr)
        {
            LOG_INFO(
                "MiniMap: F8 diagnostic: "
                "terrain texture is already loaded: %p",
                g_terrainTexture);

            return;
        }

        if (g_terrainSelf->hooks->ImGuiTextures == nullptr)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "ImGui texture interface is unavailable");

            return;
        }

        const uintptr_t loadSynchronousAddress =
            GetSoftObjectLoadSynchronousAddress();

        const uintptr_t getBrushTextureAddress =
            GetBrushResourceAsTexture2DAddress();

        const uintptr_t getResidentMipsAddress =
            GetNumResidentMipsAddress();

        const uintptr_t getAllowedMipsAddress =
            GetNumMipsAllowedAddress();

        const uintptr_t getNumMipsAddress =
            GetNumMipsAddress();

        const uintptr_t streamInAddress =
            GetStreamInAddress();

        const uintptr_t waitPendingAddress =
            GetWaitForPendingInitOrStreamingAddress();

        if (loadSynchronousAddress == 0 ||
            getBrushTextureAddress == 0 ||
            getResidentMipsAddress == 0 ||
            getAllowedMipsAddress == 0 ||
            getNumMipsAddress == 0 ||
            streamInAddress == 0 ||
            waitPendingAddress == 0)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "one or more required native functions are unavailable");

            return;
        }

        auto loadSynchronous =
            reinterpret_cast<LoadSynchronousFn>(
                loadSynchronousAddress);

        auto getBrushTexture =
            reinterpret_cast<GetBrushTextureFn>(
                getBrushTextureAddress);

        auto getNumResidentMips =
            reinterpret_cast<GetNumResidentMipsFn>(
                getResidentMipsAddress);

        auto getNumMipsAllowed =
            reinterpret_cast<GetNumMipsAllowedFn>(
                getAllowedMipsAddress);

        auto getNumMips =
            reinterpret_cast<GetNumMipsFn>(
                getNumMipsAddress);

        auto streamIn =
            reinterpret_cast<StreamInFn>(
                streamInAddress);

        auto waitForPendingInitOrStreaming =
            reinterpret_cast<WaitForPendingInitOrStreamingFn>(
                waitPendingAddress);

        // ---------------------------------------------------------------------
        // Obtain the local pawn's physical world position.
        //
        // World ownership and player discovery are map-wide responsibilities.
        // Terrain consumes only the resulting physical world position.
        // ---------------------------------------------------------------------

        SDK::FVector playerLocation = {};

        if (!MiniMapMap::TryGetPlayerWorldPosition(
            playerLocation))
        {
            return;
        }

        LOG_INFO(
            "MiniMap: F8 diagnostic: "
            "player world location=(%.3f, %.3f, %.3f)",
            playerLocation.X,
            playerLocation.Y,
            playerLocation.Z);

        // ---------------------------------------------------------------------
        // Load UCrMapMenuTerrainData.
        // ---------------------------------------------------------------------

        auto* devSettings =
            FindMapMenuDevSettingsCDO();

        if (devSettings == nullptr)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "CrMapMenuDevSettings CDO was not found");

            return;
        }

        void* terrainDataSoftPtr =
            static_cast<void*>(
                &devSettings->TerrainData);

        SDK::UObject* loadedObject =
            loadSynchronous(
                terrainDataSoftPtr);

        if (loadedObject == nullptr)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "TerrainData could not be loaded");

            return;
        }

        auto* loadedTerrainData =
            static_cast<SDK::UCrMapMenuTerrainData*>(
                loadedObject);

        g_terrainData =
            loadedTerrainData;

        const auto& pivot =
            loadedTerrainData->MapTerrainTopLeftPivotPoint;

        const auto& segmentSize =
            loadedTerrainData->MapTerrainSegmentSize;

        const int segmentCount =
            loadedTerrainData->TerrainSegmentsData.Num();

        LOG_INFO(
            "MiniMap: F8 diagnostic: "
            "TerrainData origin=(%.3f, %.3f, %.3f) "
            "segment-size=(%.3f, %.3f, %.3f) "
            "segments=%d",
            pivot.X,
            pivot.Y,
            pivot.Z,
            segmentSize.X,
            segmentSize.Y,
            segmentSize.Z,
            segmentCount);

        // ---------------------------------------------------------------------
        // Convert physical player world position to terrain-grid coordinates.
        //
        // Native StarRupture terrain placement establishes a factor of 100
        // between MapTerrainSegmentSize canvas units and world units.
        //
        // World X -> terrain horizontal -> TerrainSegmentGridIndex.Y
        // World Y -> terrain vertical   -> TerrainSegmentGridIndex.X
        // ---------------------------------------------------------------------

        const double worldTileWidth =
            100.0 *
            static_cast<double>(
                segmentSize.X);

        const double worldTileHeight =
            100.0 *
            static_cast<double>(
                segmentSize.Y);

        if (worldTileWidth <= 0.0 ||
            worldTileHeight <= 0.0)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "invalid terrain world tile size "
                "(%.3f x %.3f)",
                worldTileWidth,
                worldTileHeight);

            return;
        }

        const double horizontalTile =
            (playerLocation.X -
                static_cast<double>(pivot.X)) /
            worldTileWidth;

        const double verticalTile =
            (playerLocation.Y -
                static_cast<double>(pivot.Y)) /
            worldTileHeight;

        const int gridX =
            static_cast<int>(
                std::floor(
                    verticalTile));

        const int gridY =
            static_cast<int>(
                std::floor(
                    horizontalTile));

        const double localU =
            horizontalTile -
            static_cast<double>(gridY);

        const double localV =
            verticalTile -
            static_cast<double>(gridX);

        LOG_INFO(
            "MiniMap: F8 diagnostic: "
            "player terrain grid=(%d, %d) "
            "local-uv=(%.6f, %.6f)",
            gridX,
            gridY,
            localU,
            localV);

        // ---------------------------------------------------------------------
        // Find the actual terrain record by TerrainSegmentGridIndex.
        // Do not assume array order matches grid order.
        // ---------------------------------------------------------------------

        const SDK::FCrTerrainSegmentData* diagnosticSegment =
            nullptr;

        for (int i = 0; i < segmentCount; ++i)
        {
            const auto& segment =
                loadedTerrainData->
                TerrainSegmentsData[i];

            if (segment.TerrainSegmentGridIndex.X ==
                gridX &&
                segment.TerrainSegmentGridIndex.Y ==
                gridY)
            {
                diagnosticSegment =
                    &segment;

                break;
            }
        }

        if (diagnosticSegment == nullptr)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "player position maps outside available terrain "
                "or grid=(%d, %d) has no terrain record",
                gridX,
                gridY);

            return;
        }

        LOG_INFO(
            "MiniMap: F8 diagnostic: "
            "selected terrain segment grid=(%d, %d)",
            diagnosticSegment->
            TerrainSegmentGridIndex.X,
            diagnosticSegment->
            TerrainSegmentGridIndex.Y);

        // ---------------------------------------------------------------------
        // Obtain the UTexture2D from the selected terrain brush.
        // ---------------------------------------------------------------------

        SDK::UTexture2D* sourceTexture =
            getBrushTexture(
                diagnosticSegment->
                TerrainSegmentTexture);

        LOG_INFO(
            "MiniMap: F8 diagnostic: "
            "GetBrushResourceAsTexture2D returned %p",
            sourceTexture);

        if (sourceTexture == nullptr)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "selected terrain brush did not provide "
                "a UTexture2D");

            return;
        }

        auto* streamableTexture =
            static_cast<SDK::UStreamableRenderAsset*>(
                sourceTexture);

        // ---------------------------------------------------------------------
        // First allow any initialization or streaming work already associated
        // with this texture to finish.
        //
        // StreamIn rejects a new request while pending work exists. This was
        // observed at runtime: the first F8 press returned false while the same
        // request succeeded shortly afterwards.
        // ---------------------------------------------------------------------

        LOG_INFO(
            "MiniMap: F8 diagnostic: "
            "waiting for existing texture initialization/streaming");

        waitForPendingInitOrStreaming(
            streamableTexture,
            true,
            true);

        LOG_INFO(
            "MiniMap: F8 diagnostic: "
            "existing texture initialization/streaming completed");

        // ---------------------------------------------------------------------
        // Request exactly nine resident mips when possible.
        //
        // For the observed conventional 2048x2048 / 12-mip terrain textures:
        //
        //   9 resident mips -> 256x256 live GPU resource.
        //
        // StreamIn can only increase residency. It cannot reduce a texture that
        // already has more than the requested number of resident mips.
        // ---------------------------------------------------------------------

        constexpr int32_t targetResidentMips =
            9;

        const int32_t totalMipsBefore =
            getNumMips(
                sourceTexture);

        const int32_t allowedMipsBefore =
            getNumMipsAllowed(
                sourceTexture,
                false);

        const int32_t residentMipsBefore =
            getNumResidentMips(
                sourceTexture);

        int32_t residentMips =
            residentMipsBefore;

        LOG_INFO(
            "MiniMap: F8 diagnostic: "
            "texture mips before StreamIn: "
            "total=%d allowed=%d resident=%d target=%d",
            totalMipsBefore,
            allowedMipsBefore,
            residentMipsBefore,
            targetResidentMips);

        if (allowedMipsBefore < targetResidentMips)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "texture permits only %d mip(s); "
                "target is %d",
                allowedMipsBefore,
                targetResidentMips);

            return;
        }

        if (totalMipsBefore < targetResidentMips)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "texture contains only %d mip(s); "
                "target is %d",
                totalMipsBefore,
                targetResidentMips);

            return;
        }

        if (residentMips < targetResidentMips)
        {
            LOG_INFO(
                "MiniMap: F8 diagnostic: "
                "calling StreamIn(%d, true)",
                targetResidentMips);

            const bool streamAccepted =
                streamIn(
                    sourceTexture,
                    targetResidentMips,
                    true);

            LOG_INFO(
                "MiniMap: F8 diagnostic: "
                "StreamIn returned %s",
                streamAccepted
                ? "true"
                : "false");

            if (!streamAccepted)
            {
                LOG_ERROR(
                    "MiniMap: F8 diagnostic: "
                    "StreamIn request was not accepted");

                return;
            }

            LOG_INFO(
                "MiniMap: F8 diagnostic: "
                "waiting for requested texture transition");

            waitForPendingInitOrStreaming(
                streamableTexture,
                true,
                true);

            LOG_INFO(
                "MiniMap: F8 diagnostic: "
                "requested texture transition completed");

            residentMips =
                getNumResidentMips(
                    sourceTexture);
        }
        else if (residentMips > targetResidentMips)
        {
            LOG_WARN(
                "MiniMap: F8 diagnostic: "
                "texture already has %d resident mips; "
                "StreamIn cannot reduce it to target %d",
                residentMips,
                targetResidentMips);
        }

        const int32_t totalMipsAfter =
            getNumMips(
                sourceTexture);

        const int32_t allowedMipsAfter =
            getNumMipsAllowed(
                sourceTexture,
                false);

        const int32_t residentMipsAfter =
            getNumResidentMips(
                sourceTexture);

        LOG_INFO(
            "MiniMap: F8 diagnostic: "
            "texture mips before AlienX copy: "
            "total=%d allowed=%d resident=%d target=%d",
            totalMipsAfter,
            allowedMipsAfter,
            residentMipsAfter,
            targetResidentMips);

        if (residentMipsAfter < targetResidentMips)
        {
            LOG_ERROR(
                "MiniMap: F8 diagnostic: "
                "texture did not reach requested residency "
                "(resident=%d target=%d)",
                residentMipsAfter,
                targetResidentMips);

            return;
        }

        if (residentMipsBefore <= targetResidentMips &&
            residentMipsAfter != targetResidentMips)
        {
            LOG_WARN(
                "MiniMap: F8 diagnostic: "
                "exact-nine diagnostic did not finish at target "
                "(resident=%d target=%d)",
                residentMipsAfter,
                targetResidentMips);
        }

        // ---------------------------------------------------------------------
        // Take the independent AlienX-owned GPU copy immediately.
        // ---------------------------------------------------------------------

        LOG_INFO(
            "MiniMap: F8 diagnostic: "
            "copying current player terrain grid=(%d, %d) "
            "through AlienX",
            gridX,
            gridY);

        g_terrainTexture =
            g_terrainSelf->
            hooks->
            ImGuiTextures->
            LoadFromUTexture2D(
                sourceTexture,
                kDiagnosticTextureName);

        if (g_terrainTexture == nullptr)
        {
            LOG_WARN(
                "MiniMap: F8 diagnostic: "
                "AlienX terrain texture load returned null");

            return;
        }

        int textureWidth = 0;
        int textureHeight = 0;

        g_terrainSelf->
            hooks->
            ImGuiTextures->
            GetSize(
                g_terrainTexture,
                &textureWidth,
                &textureHeight);

        LOG_INFO(
            "MiniMap: F8 diagnostic: "
            "AlienX terrain texture loaded: "
            "grid=(%d, %d) handle=%p size=%dx%d",
            gridX,
            gridY,
            g_terrainTexture,
            textureWidth,
            textureHeight);
    }


    void OnDiagnosticKeyPressed(
        EModKey key,
        EModKeyEvent event)
    {
        (void)key;
        (void)event;

        g_diagnosticPending.store(
            true,
            std::memory_order_release);

        LOG_INFO(
            "MiniMap: F8 terrain diagnostic requested");
    }


    void OnTick(
        float deltaSeconds)
    {
        (void)deltaSeconds;

        const bool diagnosticRequested =
            g_diagnosticPending.exchange(
                false,
                std::memory_order_acq_rel);

        if (!diagnosticRequested)
        {
            return;
        }

        ProcessTerrainDiagnostic();
    }
}


namespace MiniMapTerrain
{
    void CancelPendingDiagnostic()
    {
        g_diagnosticPending.store(
            false,
            std::memory_order_release);
    }

    bool Initialize(IPluginSelf* self)
    {
        g_terrainSelf = self;
        g_terrainData = nullptr;

        g_diagnosticPending.store(
            false,
            std::memory_order_release);

        if (g_terrainSelf == nullptr ||
            g_terrainSelf->hooks == nullptr)
        {
            LOG_ERROR(
                "MiniMap: hooks are unavailable "
                "during terrain initialization");

            return false;
        }

        return true;
    }


    bool RegisterDiagnostics(IPluginSelf* self)
    {
        if (self == nullptr ||
            self->hooks == nullptr)
        {
            LOG_ERROR(
                "MiniMap: cannot register terrain diagnostics "
                "because plugin hooks are unavailable");

            return false;
        }

        if (self->hooks->Input == nullptr)
        {
            LOG_ERROR(
                "MiniMap: cannot register terrain diagnostic key "
                "because input hooks are unavailable");

            return false;
        }

        if (self->hooks->Engine == nullptr)
        {
            LOG_ERROR(
                "MiniMap: cannot register terrain diagnostic tick "
                "because engine hooks are unavailable");

            return false;
        }

        g_terrainSelf = self;

        g_diagnosticPending.store(
            false,
            std::memory_order_release);

        self->hooks->Engine->RegisterOnTick(
            &OnTick);

        g_tickRegistered = true;

        self->hooks->Input->RegisterKeybindByName(
            kDiagnosticKey,
            EModKeyEvent::Pressed,
            &OnDiagnosticKeyPressed);

        g_diagnosticKeyRegistered = true;

        LOG_INFO(
            "MiniMap: registered terrain diagnostic key: %s",
            kDiagnosticKey);

        LOG_INFO(
            "MiniMap: registered terrain diagnostic game-thread tick");

        return true;
    }


    void UnregisterDiagnostics()
    {
        g_diagnosticPending.store(
            false,
            std::memory_order_release);

        if (g_terrainSelf != nullptr &&
            g_terrainSelf->hooks != nullptr)
        {
            if (g_diagnosticKeyRegistered &&
                g_terrainSelf->hooks->Input != nullptr)
            {
                g_terrainSelf->
                    hooks->
                    Input->
                    UnregisterKeybindByName(
                        kDiagnosticKey,
                        EModKeyEvent::Pressed,
                        &OnDiagnosticKeyPressed);

                LOG_INFO(
                    "MiniMap: unregistered terrain diagnostic key: %s",
                    kDiagnosticKey);
            }

            if (g_tickRegistered &&
                g_terrainSelf->hooks->Engine != nullptr)
            {
                g_terrainSelf->
                    hooks->
                    Engine->
                    UnregisterOnTick(
                        &OnTick);

                LOG_INFO(
                    "MiniMap: unregistered terrain diagnostic "
                    "game-thread tick");
            }
        }

        g_diagnosticKeyRegistered = false;
        g_tickRegistered = false;
    }


    void Shutdown()
    {
        g_diagnosticPending.store(
            false,
            std::memory_order_release);

        if (g_terrainTexture != nullptr)
        {
            if (g_terrainSelf != nullptr &&
                g_terrainSelf->hooks != nullptr &&
                g_terrainSelf->hooks->ImGuiTextures != nullptr)
            {
                g_terrainSelf->
                    hooks->
                    ImGuiTextures->
                    FreeTexture(
                        g_terrainTexture);

                LOG_INFO(
                    "MiniMap: terrain texture released");
            }
            else
            {
                LOG_WARN(
                    "MiniMap: terrain texture handle could not "
                    "be released because ImGuiTextures is unavailable");
            }

            g_terrainTexture = nullptr;
        }

        g_terrainData = nullptr;
    }


    PluginTextureHandle GetTerrainTexture()
    {
        return g_terrainTexture;
    }
}

#endif