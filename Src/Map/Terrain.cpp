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

#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
    IPluginSelf* g_terrainSelf = nullptr;

    PluginTextureHandle g_terrainTexture = nullptr;

    bool g_tickRegistered = false;

    float g_updateAccumulator = 0.0f;

    constexpr float kUpdateIntervalSeconds =
        0.25f;

    int g_loadedRadiationLevel = -1;
    int g_loadedGridX = -1;
    int g_loadedGridY = -1;
    int g_loadedChunkX = -1;
    int g_loadedChunkY = -1;


    void ResetLoadedChunkIdentity()
    {
        g_loadedRadiationLevel = -1;
        g_loadedGridX = -1;
        g_loadedGridY = -1;
        g_loadedChunkX = -1;
        g_loadedChunkY = -1;
    }


    void ReleaseTerrainTexture()
    {
        if (g_terrainTexture == nullptr)
        {
            return;
        }

        if (g_terrainSelf != nullptr &&
            g_terrainSelf->hooks != nullptr &&
            g_terrainSelf->hooks->ImGuiTextures != nullptr)
        {
            g_terrainSelf->
                hooks->
                ImGuiTextures->
                FreeTexture(
                    g_terrainTexture);
        }

        g_terrainTexture = nullptr;

        ResetLoadedChunkIdentity();
    }


    bool IsCurrentChunkLoaded(
        const MiniMapTerrainSource::SourceTile& sourceTile,
        int chunkX,
        int chunkY)
    {
        return
            g_terrainTexture != nullptr &&
            g_loadedRadiationLevel ==
            sourceTile.RadiationLevel &&
            g_loadedGridX ==
            sourceTile.GridX &&
            g_loadedGridY ==
            sourceTile.GridY &&
            g_loadedChunkX ==
            chunkX &&
            g_loadedChunkY ==
            chunkY;
    }


    bool LoadCurrentChunk()
    {
        if (g_terrainSelf == nullptr ||
            g_terrainSelf->hooks == nullptr ||
            g_terrainSelf->hooks->ImGuiTextures == nullptr ||
            !MiniMapMap::HasWorld())
        {
            return false;
        }

        SDK::FVector playerLocation = {};

        if (!MiniMapMap::TryGetPlayerWorldPosition(
            playerLocation))
        {
            return false;
        }

        MiniMapTerrainSource::SourceTile sourceTile = {};

        if (!MiniMapTerrainSource::TryResolveSourceTile(
            playerLocation,
            sourceTile,
            false))
        {
            return false;
        }

        int chunkX = 0;
        int chunkY = 0;

        if (!MiniMapTerrainChunks::TryGetChunkCoordinates(
            sourceTile.LocalU,
            sourceTile.LocalV,
            chunkX,
            chunkY))
        {
            return false;
        }

        if (IsCurrentChunkLoaded(
            sourceTile,
            chunkX,
            chunkY))
        {
            return true;
        }

        SDK::UTexture2D* sourceTexture =
            sourceTile.Texture;

        if (sourceTexture == nullptr)
        {
            return false;
        }

        const MiniMapNative::NativeApi* native =
            MiniMapNative::Get();

        if (native == nullptr ||
            native->texture.waitForPendingInitOrStreaming == nullptr)
        {
            return false;
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

            return false;
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

            return false;
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

            return false;
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

        if (sourceWidth != platformData.SizeX ||
            sourceHeight != platformData.SizeY ||
            mipZero.SizeZ == 0 ||
            mipZero.BulkData == nullptr ||
            mipZero.BulkSize != expectedBC1ByteCount)
        {
            LOG_ERROR(
                "MiniMap: terrain: invalid source mip zero metadata");

            return false;
        }

        std::vector<uint8_t> compressedChunk;

        bool loadedFromCache =
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

        if (!loadedFromCache)
        {
            void* ownedMipZero = nullptr;

            if (!MiniMapNative::TextureAccess::CopyBulkData(
                mipZero,
                &ownedMipZero,
                false) ||
                ownedMipZero == nullptr)
            {
                LOG_ERROR(
                    "MiniMap: terrain: "
                    "failed to copy terrain mip zero");

                return false;
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
                    "failed to extract current BC1 chunk");

                return false;
            }
        }

        std::vector<uint8_t> rgbaChunk;

        if (!MiniMapTerrainChunks::DecodeBC1Chunk(
            compressedChunk,
            rgbaChunk))
        {
            LOG_ERROR(
                "MiniMap: terrain: "
                "failed to decode current BC1 chunk");

            return false;
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

        PluginTextureHandle newTexture =
            g_terrainSelf->
            hooks->
            ImGuiTextures->
            LoadFromRGBA(
                rgbaChunk.data(),
                MiniMapTerrainChunks::kChunkSizePixels,
                MiniMapTerrainChunks::kChunkSizePixels,
                textureName);

        if (newTexture == nullptr)
        {
            LOG_ERROR(
                "MiniMap: terrain: "
                "failed to upload current terrain chunk");

            return false;
        }

        if (g_terrainTexture != nullptr)
        {
            g_terrainSelf->
                hooks->
                ImGuiTextures->
                FreeTexture(
                    g_terrainTexture);
        }

        g_terrainTexture =
            newTexture;

        g_loadedRadiationLevel =
            sourceTile.RadiationLevel;

        g_loadedGridX =
            sourceTile.GridX;

        g_loadedGridY =
            sourceTile.GridY;

        g_loadedChunkX =
            chunkX;

        g_loadedChunkY =
            chunkY;

        LOG_INFO(
            "MiniMap: terrain: "
            "current chunk loaded R%d grid=(%d, %d) "
            "chunk=(%d, %d) source=%s",
            sourceTile.RadiationLevel,
            sourceTile.GridX,
            sourceTile.GridY,
            chunkX,
            chunkY,
            loadedFromCache
            ? "cache"
            : "mip0");

        return true;
    }


    void OnTerrainTick(
        float deltaSeconds)
    {
        g_updateAccumulator +=
            deltaSeconds;

        if (g_updateAccumulator <
            kUpdateIntervalSeconds)
        {
            return;
        }

        g_updateAccumulator =
            0.0f;

        LoadCurrentChunk();
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

        ReleaseTerrainTexture();

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

        ReleaseTerrainTexture();

        MiniMapTerrainDiagnostics::Shutdown();

        g_terrainSelf =
            nullptr;

        g_updateAccumulator =
            0.0f;
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


    PluginTextureHandle GetTerrainTexture()
    {
        return g_terrainTexture;
    }
}

#endif
