#if defined(MODLOADER_CLIENT_BUILD)

#include "Terrain.h"

#include "Map.h"
#include "TerrainCache.h"
#include "TerrainChunks.h"
#include "TerrainDiagnostics.h"
#include "TerrainSource.h"

#include "../Native/NativeApi.h"
#include "../Native/TextureAccess.h"
#include "../plugin.h"
#include "../plugin_helpers.h"

#include "SDK/Chimera_classes.hpp"
#include "SDK/Engine_classes.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>

namespace
{
    struct ChunkIdentity
    {
        int RadiationLevel = 0;
        int GridX = 0;
        int GridY = 0;
        int ChunkX = 0;
        int ChunkY = 0;
    };


    struct LoadedChunk
    {
        ChunkIdentity Identity = {};
        PluginTextureHandle Texture = nullptr;

        int GlobalChunkX = 0;
        int GlobalChunkY = 0;
    };


    IPluginSelf* g_terrainSelf = nullptr;

    bool g_tickRegistered = false;

    float g_updateAccumulator = 0.0f;

    constexpr float kUpdateIntervalSeconds =
        0.25f;

    constexpr double kChunkWorldUnits =
        static_cast<double>(
            MiniMapTerrainChunks::kChunkWorldSizeMeters) *
        100.0;

    constexpr double kDefaultMetersPerPixel =
        100.0 / 300.0;

    constexpr float kDefaultViewportPixelWidth =
        300.0f;

    constexpr float kDefaultViewportPixelHeight =
        300.0f;

    std::mutex g_renderMutex;
    std::vector<LoadedChunk> g_loadedChunks;

    float g_viewportPixelWidth =
        kDefaultViewportPixelWidth;

    float g_viewportPixelHeight =
        kDefaultViewportPixelHeight;

    bool g_hasViewportAnchor = false;

    SDK::FVector g_viewportAnchorWorldPosition = {};

    double g_viewportAnchorGlobalChunkX = 0.0;
    double g_viewportAnchorGlobalChunkY = 0.0;

    double g_playerGlobalChunkX = 0.0;
    double g_playerGlobalChunkY = 0.0;


    bool SameIdentity(
        const ChunkIdentity& left,
        const ChunkIdentity& right)
    {
        return
            left.RadiationLevel ==
            right.RadiationLevel &&
            left.GridX ==
            right.GridX &&
            left.GridY ==
            right.GridY &&
            left.ChunkX ==
            right.ChunkX &&
            left.ChunkY ==
            right.ChunkY;
    }


    int FloorDiv(
        int value,
        int divisor)
    {
        int quotient =
            value /
            divisor;

        const int remainder =
            value %
            divisor;

        if (remainder < 0)
        {
            --quotient;
        }

        return quotient;
    }


    int PositiveMod(
        int value,
        int divisor)
    {
        int remainder =
            value %
            divisor;

        if (remainder < 0)
        {
            remainder +=
                divisor;
        }

        return remainder;
    }


    void FreeTexture(
        PluginTextureHandle texture)
    {
        if (texture == nullptr ||
            g_terrainSelf == nullptr ||
            g_terrainSelf->hooks == nullptr ||
            g_terrainSelf->hooks->ImGuiTextures == nullptr)
        {
            return;
        }

        g_terrainSelf->
            hooks->
            ImGuiTextures->
            FreeTexture(
                texture);
    }


    void ReleaseLoadedChunks()
    {
        std::lock_guard<std::mutex> lock(
            g_renderMutex);

        for (const LoadedChunk& chunk :
            g_loadedChunks)
        {
            FreeTexture(
                chunk.Texture);
        }

        g_loadedChunks.clear();
    }


    PluginTextureHandle FindExistingTexture(
        const std::vector<LoadedChunk>& existingChunks,
        const ChunkIdentity& identity)
    {
        for (const LoadedChunk& chunk :
            existingChunks)
        {
            if (chunk.Texture != nullptr &&
                SameIdentity(
                    chunk.Identity,
                    identity))
            {
                return chunk.Texture;
            }
        }

        return nullptr;
    }


    bool ContainsTexture(
        const std::vector<LoadedChunk>& chunks,
        PluginTextureHandle texture)
    {
        if (texture == nullptr)
        {
            return false;
        }

        for (const LoadedChunk& chunk :
            chunks)
        {
            if (chunk.Texture ==
                texture)
            {
                return true;
            }
        }

        return false;
    }


    PluginTextureHandle LoadChunkTexture(
        const MiniMapTerrainSource::SourceTile& sourceTile,
        int chunkX,
        int chunkY,
        bool& outLoadedFromCache)
    {
        outLoadedFromCache =
            false;

        if (g_terrainSelf == nullptr ||
            g_terrainSelf->hooks == nullptr ||
            g_terrainSelf->hooks->ImGuiTextures == nullptr ||
            sourceTile.Texture == nullptr)
        {
            return nullptr;
        }

        SDK::UTexture2D* sourceTexture =
            sourceTile.Texture;

        const MiniMapNative::NativeApi* native =
            MiniMapNative::Get();

        if (native == nullptr ||
            native->
            texture.
            waitForPendingInitOrStreaming ==
            nullptr)
        {
            return nullptr;
        }

        auto* streamableTexture =
            static_cast<SDK::UStreamableRenderAsset*>(
                sourceTexture);

        native->
            texture.
            waitForPendingInitOrStreaming(
                streamableTexture,
                true,
                true);

        MiniMapNative::TextureAccess::PlatformData platformData = {};

        if (!MiniMapNative::TextureAccess::QueryPlatformData(
            sourceTexture,
            platformData))
        {
            LOG_ERROR(
                "MiniMap: terrain: "
                "failed to query source platform data");

            return nullptr;
        }

        if (platformData.PixelFormat != 5 ||
            platformData.VTData != nullptr ||
            platformData.SizeX !=
            MiniMapTerrainChunks::kChunksPerAxis *
            MiniMapTerrainChunks::kChunkSizePixels ||
            platformData.SizeY !=
            MiniMapTerrainChunks::kChunksPerAxis *
            MiniMapTerrainChunks::kChunkSizePixels ||
            platformData.MipCount <= 0 ||
            !platformData.HasMipPointers)
        {
            LOG_ERROR(
                "MiniMap: terrain: unsupported terrain source "
                "size=%dx%d format=%u VTData=%p mips=%d",
                platformData.SizeX,
                platformData.SizeY,
                static_cast<unsigned int>(
                    platformData.PixelFormat),
                platformData.VTData,
                platformData.MipCount);

            return nullptr;
        }

        MiniMapNative::TextureAccess::MipData mipZero = {};

        if (!MiniMapNative::TextureAccess::QueryMipData(
            platformData,
            0,
            mipZero))
        {
            LOG_ERROR(
                "MiniMap: terrain: "
                "failed to query source mip zero");

            return nullptr;
        }

        const int sourceWidth =
            static_cast<int>(
                mipZero.SizeX);

        const int sourceHeight =
            static_cast<int>(
                mipZero.SizeY);

        const int64_t expectedBC1ByteCount =
            static_cast<int64_t>(
                sourceWidth /
                MiniMapTerrainChunks::kBC1BlockSizePixels) *
            static_cast<int64_t>(
                sourceHeight /
                MiniMapTerrainChunks::kBC1BlockSizePixels) *
            MiniMapTerrainChunks::kBC1BytesPerBlock;

        if (sourceWidth !=
            platformData.SizeX ||
            sourceHeight !=
            platformData.SizeY ||
            mipZero.SizeZ == 0 ||
            mipZero.BulkData == nullptr ||
            mipZero.BulkSize !=
            expectedBC1ByteCount)
        {
            LOG_ERROR(
                "MiniMap: terrain: "
                "invalid source mip zero metadata");

            return nullptr;
        }

        std::vector<uint8_t> compressedChunk;

        outLoadedFromCache =
            MiniMapTerrainCache::TryLoadChunk(
                sourceTile.RadiationLevel,
                sourceTile.GridX,
                sourceTile.GridY,
                sourceWidth,
                sourceHeight,
                platformData.PixelFormat,
                platformData.SRGB,
                chunkX,
                chunkY,
                compressedChunk);

        if (!outLoadedFromCache)
        {
            void* ownedMipZero =
                nullptr;

            if (!MiniMapNative::TextureAccess::CopyBulkData(
                mipZero,
                &ownedMipZero,
                false) ||
                ownedMipZero ==
                nullptr)
            {
                LOG_ERROR(
                    "MiniMap: terrain: "
                    "failed to copy terrain mip zero");

                return nullptr;
            }

            const bool cacheWritten =
                MiniMapTerrainCache::StoreSourceTile(
                    sourceTile.RadiationLevel,
                    sourceTile.GridX,
                    sourceTile.GridY,
                    sourceWidth,
                    sourceHeight,
                    platformData.PixelFormat,
                    platformData.SRGB,
                    static_cast<const uint8_t*>(
                        ownedMipZero),
                    static_cast<size_t>(
                        mipZero.BulkSize));

            if (!cacheWritten)
            {
                LOG_WARN(
                    "MiniMap: terrain: "
                    "cache write failed for R%d grid=(%d, %d)",
                    sourceTile.RadiationLevel,
                    sourceTile.GridX,
                    sourceTile.GridY);
            }

            const bool extracted =
                MiniMapTerrainChunks::ExtractBC1Chunk(
                    static_cast<const uint8_t*>(
                        ownedMipZero),
                    sourceWidth,
                    sourceHeight,
                    chunkX,
                    chunkY,
                    compressedChunk);

            MiniMapNative::TextureAccess::FreeBulkDataCopy(
                ownedMipZero);

            if (!extracted)
            {
                LOG_ERROR(
                    "MiniMap: terrain: "
                    "failed to extract BC1 chunk");

                return nullptr;
            }
        }

        std::vector<uint8_t> rgbaChunk;

        if (!MiniMapTerrainChunks::DecodeBC1Chunk(
            compressedChunk,
            rgbaChunk))
        {
            LOG_ERROR(
                "MiniMap: terrain: "
                "failed to decode BC1 chunk");

            return nullptr;
        }

        char textureName[128] = {};

        std::snprintf(
            textureName,
            sizeof(textureName),
            "MiniMap_Terrain_R%d_%d_%d_%d_%d",
            sourceTile.RadiationLevel,
            sourceTile.GridX,
            sourceTile.GridY,
            chunkX,
            chunkY);

        PluginTextureHandle texture =
            g_terrainSelf->
            hooks->
            ImGuiTextures->
            LoadFromRGBA(
                rgbaChunk.data(),
                MiniMapTerrainChunks::kChunkSizePixels,
                MiniMapTerrainChunks::kChunkSizePixels,
                textureName);

        if (texture == nullptr)
        {
            LOG_ERROR(
                "MiniMap: terrain: "
                "failed to upload terrain chunk");

            return nullptr;
        }

        LOG_INFO(
            "MiniMap: terrain: "
            "loaded R%d grid=(%d, %d) "
            "chunk=(%d, %d) source=%s",
            sourceTile.RadiationLevel,
            sourceTile.GridX,
            sourceTile.GridY,
            chunkX,
            chunkY,
            outLoadedFromCache
            ? "cache"
            : "mip0");

        return texture;
    }


    bool ReconcileViewport(
        const SDK::FVector& playerLocation)
    {
        if (g_terrainSelf == nullptr ||
            g_terrainSelf->hooks == nullptr ||
            g_terrainSelf->hooks->ImGuiTextures == nullptr ||
            !MiniMapMap::HasWorld())
        {
            return false;
        }

        MiniMapTerrainSource::SourceTile playerTile = {};

        if (!MiniMapTerrainSource::TryResolveSourceTile(
            playerLocation,
            playerTile,
            false))
        {
            return false;
        }

        const double playerGlobalChunkX =
            static_cast<double>(
                playerTile.GridY *
                MiniMapTerrainChunks::kChunksPerAxis) +
            playerTile.LocalU *
            static_cast<double>(
                MiniMapTerrainChunks::kChunksPerAxis);

        const double playerGlobalChunkY =
            static_cast<double>(
                playerTile.GridX *
                MiniMapTerrainChunks::kChunksPerAxis) +
            playerTile.LocalV *
            static_cast<double>(
                MiniMapTerrainChunks::kChunksPerAxis);

        std::vector<LoadedChunk> existingChunks;

        float viewportPixelWidth =
            kDefaultViewportPixelWidth;

        float viewportPixelHeight =
            kDefaultViewportPixelHeight;

        {
            std::lock_guard<std::mutex> lock(
                g_renderMutex);

            existingChunks =
                g_loadedChunks;

            viewportPixelWidth =
                g_viewportPixelWidth;

            viewportPixelHeight =
                g_viewportPixelHeight;
        }

        const double viewportWidthMeters =
            static_cast<double>(
                viewportPixelWidth) *
            kDefaultMetersPerPixel;

        const double viewportHeightMeters =
            static_cast<double>(
                viewportPixelHeight) *
            kDefaultMetersPerPixel;

        const double viewportWidthInChunks =
            viewportWidthMeters /
            static_cast<double>(
                MiniMapTerrainChunks::kChunkWorldSizeMeters);

        const double viewportHeightInChunks =
            viewportHeightMeters /
            static_cast<double>(
                MiniMapTerrainChunks::kChunkWorldSizeMeters);

        const double viewportHalfWidthInChunks =
            viewportWidthInChunks *
            0.5;

        const double viewportHalfHeightInChunks =
            viewportHeightInChunks *
            0.5;

        const double viewportMinX =
            playerGlobalChunkX -
            viewportHalfWidthInChunks;

        const double viewportMaxX =
            playerGlobalChunkX +
            viewportHalfWidthInChunks;

        const double viewportMinY =
            playerGlobalChunkY -
            viewportHalfHeightInChunks;

        const double viewportMaxY =
            playerGlobalChunkY +
            viewportHalfHeightInChunks;

        constexpr double kBoundaryEpsilon =
            1.0e-9;

        const int firstGlobalChunkX =
            static_cast<int>(
                std::floor(
                    viewportMinX));

        const int lastGlobalChunkX =
            static_cast<int>(
                std::floor(
                    viewportMaxX -
                    kBoundaryEpsilon));

        const int firstGlobalChunkY =
            static_cast<int>(
                std::floor(
                    viewportMinY));

        const int lastGlobalChunkY =
            static_cast<int>(
                std::floor(
                    viewportMaxY -
                    kBoundaryEpsilon));

        std::vector<LoadedChunk> nextChunks;

        const int chunkCountX =
            lastGlobalChunkX -
            firstGlobalChunkX +
            1;

        const int chunkCountY =
            lastGlobalChunkY -
            firstGlobalChunkY +
            1;

        if (chunkCountX > 0 &&
            chunkCountY > 0)
        {
            nextChunks.reserve(
                static_cast<size_t>(
                    chunkCountX *
                    chunkCountY));
        }

        for (int globalChunkY =
            firstGlobalChunkY;
            globalChunkY <=
            lastGlobalChunkY;
            ++globalChunkY)
        {
            for (int globalChunkX =
                firstGlobalChunkX;
                globalChunkX <=
                lastGlobalChunkX;
                ++globalChunkX)
            {
                const int gridY =
                    FloorDiv(
                        globalChunkX,
                        MiniMapTerrainChunks::kChunksPerAxis);

                const int gridX =
                    FloorDiv(
                        globalChunkY,
                        MiniMapTerrainChunks::kChunksPerAxis);

                const int chunkX =
                    PositiveMod(
                        globalChunkX,
                        MiniMapTerrainChunks::kChunksPerAxis);

                const int chunkY =
                    PositiveMod(
                        globalChunkY,
                        MiniMapTerrainChunks::kChunksPerAxis);

                const double targetWorldX =
                    playerLocation.X +
                    (static_cast<double>(
                        globalChunkX) +
                        0.5 -
                        playerGlobalChunkX) *
                    kChunkWorldUnits;

                const double targetWorldY =
                    playerLocation.Y +
                    (static_cast<double>(
                        globalChunkY) +
                        0.5 -
                        playerGlobalChunkY) *
                    kChunkWorldUnits;

                SDK::FVector targetWorldPosition =
                    playerLocation;

                targetWorldPosition.X =
                    targetWorldX;

                targetWorldPosition.Y =
                    targetWorldY;

                MiniMapTerrainSource::SourceTile sourceTile = {};

                if (!MiniMapTerrainSource::TryResolveSourceTile(
                    targetWorldPosition,
                    sourceTile,
                    false))
                {
                    continue;
                }

                if (sourceTile.GridX !=
                    gridX ||
                    sourceTile.GridY !=
                    gridY)
                {
                    continue;
                }

                ChunkIdentity identity = {};

                identity.RadiationLevel =
                    sourceTile.RadiationLevel;

                identity.GridX =
                    gridX;

                identity.GridY =
                    gridY;

                identity.ChunkX =
                    chunkX;

                identity.ChunkY =
                    chunkY;

                PluginTextureHandle texture =
                    FindExistingTexture(
                        existingChunks,
                        identity);

                if (texture == nullptr)
                {
                    bool loadedFromCache =
                        false;

                    texture =
                        LoadChunkTexture(
                            sourceTile,
                            chunkX,
                            chunkY,
                            loadedFromCache);
                }

                if (texture == nullptr)
                {
                    continue;
                }

                LoadedChunk renderChunk = {};

                renderChunk.Identity =
                    identity;

                renderChunk.Texture =
                    texture;

                renderChunk.GlobalChunkX =
                    globalChunkX;

                renderChunk.GlobalChunkY =
                    globalChunkY;

                nextChunks.push_back(
                    renderChunk);
            }
        }

        {
            std::lock_guard<std::mutex> lock(
                g_renderMutex);

            for (const LoadedChunk& oldChunk :
                g_loadedChunks)
            {
                if (!ContainsTexture(
                    nextChunks,
                    oldChunk.Texture))
                {
                    FreeTexture(
                        oldChunk.Texture);
                }
            }

            g_loadedChunks =
                std::move(
                    nextChunks);

            g_viewportAnchorWorldPosition =
                playerLocation;

            g_viewportAnchorGlobalChunkX =
                playerGlobalChunkX;

            g_viewportAnchorGlobalChunkY =
                playerGlobalChunkY;

            g_playerGlobalChunkX =
                playerGlobalChunkX;

            g_playerGlobalChunkY =
                playerGlobalChunkY;

            g_hasViewportAnchor =
                true;
        }

        return true;
    }


    void UpdateFastViewportCenter(
        const SDK::FVector& playerLocation)
    {
        std::lock_guard<std::mutex> lock(
            g_renderMutex);

        if (!g_hasViewportAnchor)
        {
            return;
        }

        g_playerGlobalChunkX =
            g_viewportAnchorGlobalChunkX +
            (playerLocation.X -
                g_viewportAnchorWorldPosition.X) /
            kChunkWorldUnits;

        g_playerGlobalChunkY =
            g_viewportAnchorGlobalChunkY +
            (playerLocation.Y -
                g_viewportAnchorWorldPosition.Y) /
            kChunkWorldUnits;
    }


    void OnTerrainTick(
        float deltaSeconds)
    {
        if (!MiniMapMap::HasWorld())
        {
            return;
        }

        SDK::FVector playerLocation = {};

        if (!MiniMapMap::TryGetPlayerWorldPosition(
            playerLocation))
        {
            return;
        }

        UpdateFastViewportCenter(
            playerLocation);

        g_updateAccumulator +=
            deltaSeconds;

        if (g_updateAccumulator <
            kUpdateIntervalSeconds)
        {
            return;
        }

        g_updateAccumulator =
            0.0f;

        ReconcileViewport(
            playerLocation);
    }
}


namespace MiniMapTerrain
{
    bool Initialize(
        IPluginSelf* self)
    {
        g_terrainSelf =
            self;

        g_updateAccumulator =
            kUpdateIntervalSeconds;

        {
            std::lock_guard<std::mutex> lock(
                g_renderMutex);

            g_viewportPixelWidth =
                kDefaultViewportPixelWidth;

            g_viewportPixelHeight =
                kDefaultViewportPixelHeight;

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
        }

        ReleaseLoadedChunks();

        if (g_terrainSelf == nullptr ||
            g_terrainSelf->hooks == nullptr ||
            g_terrainSelf->hooks->Engine == nullptr ||
            g_terrainSelf->hooks->ImGuiTextures == nullptr)
        {
            LOG_ERROR(
                "MiniMap: terrain initialization failed: "
                "required hooks are unavailable");

            return false;
        }

        if (!MiniMapTerrainDiagnostics::Initialize(
            self))
        {
            return false;
        }

        if (!g_tickRegistered)
        {
            g_terrainSelf->
                hooks->
                Engine->
                RegisterOnTick(
                    &OnTerrainTick);

            g_tickRegistered =
                true;

            LOG_INFO(
                "MiniMap: registered terrain update tick");
        }

        return true;
    }


    void Shutdown()
    {
        if (g_tickRegistered &&
            g_terrainSelf != nullptr &&
            g_terrainSelf->hooks != nullptr &&
            g_terrainSelf->hooks->Engine != nullptr)
        {
            g_terrainSelf->
                hooks->
                Engine->
                UnregisterOnTick(
                    &OnTerrainTick);

            LOG_INFO(
                "MiniMap: unregistered terrain update tick");
        }

        g_tickRegistered =
            false;

        ReleaseLoadedChunks();

        MiniMapTerrainDiagnostics::Shutdown();

        g_terrainSelf =
            nullptr;

        g_updateAccumulator =
            0.0f;

        {
            std::lock_guard<std::mutex> lock(
                g_renderMutex);

            g_viewportPixelWidth =
                kDefaultViewportPixelWidth;

            g_viewportPixelHeight =
                kDefaultViewportPixelHeight;

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
        }
    }


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


    void Render(
        IModLoaderImGui* ui,
        float windowX,
        float windowY,
        float windowWidth,
        float windowHeight)
    {
        if (ui == nullptr ||
            windowWidth <= 0.0f ||
            windowHeight <= 0.0f)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(
            g_renderMutex);

        g_viewportPixelWidth =
            windowWidth;

        g_viewportPixelHeight =
            windowHeight;

        if (g_loadedChunks.empty() ||
            !g_hasViewportAnchor)
        {
            return;
        }

        const double viewportWidthMeters =
            static_cast<double>(
                windowWidth) *
            kDefaultMetersPerPixel;

        const double viewportHeightMeters =
            static_cast<double>(
                windowHeight) *
            kDefaultMetersPerPixel;

        const double viewportWidthInChunks =
            viewportWidthMeters /
            static_cast<double>(
                MiniMapTerrainChunks::kChunkWorldSizeMeters);

        const double viewportHeightInChunks =
            viewportHeightMeters /
            static_cast<double>(
                MiniMapTerrainChunks::kChunkWorldSizeMeters);

        if (viewportWidthInChunks <= 0.0 ||
            viewportHeightInChunks <= 0.0)
        {
            return;
        }

        const double viewportMinX =
            g_playerGlobalChunkX -
            viewportWidthInChunks *
            0.5;

        const double viewportMinY =
            g_playerGlobalChunkY -
            viewportHeightInChunks *
            0.5;

        PluginDrawList drawList =
            ui->GetWindowDrawList();

        for (const LoadedChunk& chunk :
            g_loadedChunks)
        {
            if (chunk.Texture ==
                nullptr)
            {
                continue;
            }

            const double rawX0 =
                (static_cast<double>(
                    chunk.GlobalChunkX) -
                    viewportMinX) /
                viewportWidthInChunks;

            const double rawX1 =
                (static_cast<double>(
                    chunk.GlobalChunkX + 1) -
                    viewportMinX) /
                viewportWidthInChunks;

            const double rawY0 =
                (static_cast<double>(
                    chunk.GlobalChunkY) -
                    viewportMinY) /
                viewportHeightInChunks;

            const double rawY1 =
                (static_cast<double>(
                    chunk.GlobalChunkY + 1) -
                    viewportMinY) /
                viewportHeightInChunks;

            const double clippedX0 =
                std::clamp(
                    rawX0,
                    0.0,
                    1.0);

            const double clippedX1 =
                std::clamp(
                    rawX1,
                    0.0,
                    1.0);

            const double clippedY0 =
                std::clamp(
                    rawY0,
                    0.0,
                    1.0);

            const double clippedY1 =
                std::clamp(
                    rawY1,
                    0.0,
                    1.0);

            if (clippedX1 <=
                clippedX0 ||
                clippedY1 <=
                clippedY0)
            {
                continue;
            }

            const float x0 =
                windowX +
                static_cast<float>(
                    clippedX0) *
                windowWidth;

            const float y0 =
                windowY +
                static_cast<float>(
                    clippedY0) *
                windowHeight;

            const float x1 =
                windowX +
                static_cast<float>(
                    clippedX1) *
                windowWidth;

            const float y1 =
                windowY +
                static_cast<float>(
                    clippedY1) *
                windowHeight;

            const float u0 =
                static_cast<float>(
                    (clippedX0 -
                        rawX0) /
                    (rawX1 -
                        rawX0));

            const float u1 =
                static_cast<float>(
                    (clippedX1 -
                        rawX0) /
                    (rawX1 -
                        rawX0));

            const float v0 =
                static_cast<float>(
                    (clippedY0 -
                        rawY0) /
                    (rawY1 -
                        rawY0));

            const float v1 =
                static_cast<float>(
                    (clippedY1 -
                        rawY0) /
                    (rawY1 -
                        rawY0));

            ui->DL_AddImage(
                drawList,
                chunk.Texture,
                x0,
                y0,
                x1,
                y1,
                u0,
                v0,
                u1,
                v1,
                0xFFFFFFFFu);
        }
    }
}

#endif
