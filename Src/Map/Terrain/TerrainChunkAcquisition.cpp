// Shares the existing native mip-copy, BC1 decode and receipt upload route between
// legacy game tiles and registered images. Native borrows never enter resident state.
#include "TerrainChunkAcquisition.h"
#if defined(MODLOADER_CLIENT_BUILD)
#include "RegisteredTerrainSource.h"
#include "TerrainChunks.h"
#include "TerrainCache.h"
#include "TerrainProfile.h"
#include "Native/NativeApi.h"
#include "Map/Map.h"
#include "Services/Runtime.h"
#include "SDK/Engine_classes.hpp"
#include "plugin_helpers.h"
#include <algorithm>
#include <cstdio>
#include <memory>

namespace MiniMapTerrainSource
{
    MiniMap::Services::Texture TerrainChunkAcquisition::Load(const SourceTile& tile, int x, int y,
        MiniMap::Services::OwnedTexture& owner, bool& uncertain, bool& cached)
    {
        cached = false;
        auto* textures = MiniMap::Services::TryTextures();
        if (!textures || !textures->Available()) return {};
        std::vector<uint8_t> rgba;
        if (tile.RegisteredImage && tile.RegisteredImage->Type ==
            RegisteredTerrainImage::SourceType::CpuOwnedPixels)
        {
            if (!ExtractCpuChunk(*tile.RegisteredImage, x, y, rgba)) return {};
        }
        else
        {
            std::vector<uint8_t> compressed;
            if (!ReadCompressed(tile, x, y, compressed, cached)) return {};
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            MiniMapTerrainProfile::Scope profile(MiniMapTerrainProfile::Phase::Decode);
#endif
            if (!MiniMapTerrainChunks::DecodeBC1Chunk(compressed, rgba)) return {};
        }
        const auto name = SharingName(tile, x, y);
        const MiniMap::Services::TextureSize size{256, 256};
        const MiniMap::Services::TextureRequest request{name, size};
        MiniMap::Services::Texture texture;
        {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            MiniMapTerrainProfile::Scope profile(MiniMapTerrainProfile::Phase::Upload);
            MiniMapTerrainProfile::UploadResult result;
#endif
            texture = FinishUpload(textures->UploadRGBA(rgba, size, request, owner), owner, uncertain);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            result.Success = texture.id != 0 && !uncertain;
#endif
        }
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        if (texture.id != 0 && !uncertain)
            LOG_INFO("MiniMap: terrain: loaded R%d grid=(%d, %d) chunk=(%d, %d) source=%s image=%s",
                tile.RadiationLevel, tile.GridX, tile.GridY, x, y,
                tile.RegisteredImage && tile.RegisteredImage->Type ==
                    RegisteredTerrainImage::SourceType::CpuOwnedPixels ? "cpu" : (cached ? "cache" : "mip0"),
                tile.RegisteredImage ? tile.RegisteredImage->SourceId.c_str() : "game");
#endif
        return texture;
    }

    bool TerrainChunkAcquisition::ResolveReadyTexture(const SourceTile& tile, SDK::UTexture2D*& texture)
    {
        texture = tile.Texture;
        const auto generation = MiniMapMap::GetWorldGeneration();
        const auto selection = RegisteredTerrainSource::Revision();
        SourceTile resolved;
        if (tile.RegisteredImage)
        {
            if (!RegisteredTerrainSource::ResolveTexture(*tile.RegisteredImage, resolved)) return false;
            texture = resolved.Texture;
        }
        if (!WaitForTexture(texture)) return false;
        if (tile.RegisteredImage)
        {
            texture = nullptr;
            resolved = {};
            if (generation != MiniMapMap::GetWorldGeneration() ||
                selection != RegisteredTerrainSource::Revision() || !MiniMapMap::GameplayReady() ||
                !RegisteredTerrainSource::ResolveTexture(*tile.RegisteredImage, resolved)) return false;
            texture = resolved.Texture;
        }
        return true;
    }

    /** Retains the existing blocking readiness arguments and diagnostic scope for both source routes. */
    bool TerrainChunkAcquisition::WaitForTexture(SDK::UTexture2D* texture)
    {
        const auto* native = MiniMapNative::Get();
        if (!texture || !native || !native->texture.waitForPendingInitOrStreaming) return false;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        MiniMapTerrainProfile::Scope profile(MiniMapTerrainProfile::Phase::Readiness);
#endif
        native->texture.waitForPendingInitOrStreaming(texture, true, true);
        return true;
    }

    bool TerrainChunkAcquisition::ValidPlatform(const SourceTile& tile,
        const MiniMapNative::TextureAccess::PlatformData& platform)
    {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        MiniMapTerrainProfile::Scope profile(MiniMapTerrainProfile::Phase::PlatformValidation);
#endif
        if (platform.PixelFormat != 5 || platform.VTData || platform.SizeX != 2048 ||
            platform.SizeY != 2048 || platform.MipCount <= 0 || !platform.HasMipPointers) return false;
        if (!tile.RegisteredImage) return true;
        const auto& image = *tile.RegisteredImage;
        return image.IsValid() && image.WidthPixels == platform.SizeX &&
            image.HeightPixels == platform.SizeY && image.SRGB == platform.SRGB;
    }

    /** Preserves legacy readiness then metadata ordering without copying registered-image owners. */
    bool TerrainChunkAcquisition::ReadMetadata(const SourceTile& tile,
        MiniMapNative::TextureAccess::PlatformData& platform,
        MiniMapNative::TextureAccess::MipData& mip)
    {
        SDK::UTexture2D* texture = nullptr;
        if (!ResolveReadyTexture(tile, texture)) return false;
        return ReadReadyMetadata(tile, texture, platform, mip);
    }

    /** Reuses the active metadata validation without coupling native inspection to cache/upload work. */
    bool TerrainChunkAcquisition::ReadReadyMetadata(const SourceTile& tile, SDK::UTexture2D* texture,
        MiniMapNative::TextureAccess::PlatformData& platform,
        MiniMapNative::TextureAccess::MipData& mip)
    {
        {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            MiniMapTerrainProfile::Scope profile(MiniMapTerrainProfile::Phase::PlatformQuery);
#endif
            if (!MiniMapNative::TextureAccess::QueryPlatformData(texture, platform)) return false;
        }
        if (!ValidPlatform(tile, platform))
        {
            LOG_ERROR("MiniMap: terrain: unsupported terrain source size=%dx%d format=%u",
                platform.SizeX, platform.SizeY, static_cast<unsigned>(platform.PixelFormat));
            return false;
        }
        {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            MiniMapTerrainProfile::Scope profile(MiniMapTerrainProfile::Phase::MipQuery);
#endif
            if (!MiniMapNative::TextureAccess::QueryMipData(platform, 0, mip)) return false;
        }
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        MiniMapTerrainProfile::Scope profile(MiniMapTerrainProfile::Phase::MipValidation);
#endif
        const int64_t expectedBytes = static_cast<int64_t>(mip.SizeX / 4) * (mip.SizeY / 4) * 8;
        return mip.SizeX == platform.SizeX && mip.SizeY == platform.SizeY && mip.SizeZ != 0 &&
            mip.BulkData && mip.BulkSize == expectedBytes;
    }

    bool TerrainChunkAcquisition::ReadCompressed(const SourceTile& tile, int x, int y,
        std::vector<uint8_t>& compressed, bool& cached)
    {
        MiniMapNative::TextureAccess::PlatformData platform;
        MiniMapNative::TextureAccess::MipData mip;
        if (!ReadMetadata(tile, platform, mip)) return false;
        cached = !tile.RegisteredImage && MiniMapTerrainCache::TryLoadChunk(
            tile, mip.SizeX, mip.SizeY, platform.PixelFormat, platform.SRGB, x, y, compressed);
        return cached || CopyChunk(tile, x, y, platform, mip, compressed);
    }

    bool TerrainChunkAcquisition::ExtractCpuChunk(const RegisteredTerrainImage& image,
        int chunkX, int chunkY, std::vector<uint8_t>& rgba)
    {
        if (image.Type != RegisteredTerrainImage::SourceType::CpuOwnedPixels || !image.IsValid()) return false;
        int pixelX = 0;
        int pixelY = 0;
        int validWidth = 0;
        int validHeight = 0;
        if (!image.GetChunkPixelBounds(chunkX, chunkY, pixelX, pixelY, validWidth, validHeight)) return false;
        constexpr size_t bytesPerPixel = 4;
        constexpr size_t chunkRowBytes = 256u * bytesPerPixel;
        rgba.resize(chunkRowBytes * 256u);
        for (int row = 0; row < 256; ++row)
        {
            const size_t sourceY = static_cast<size_t>(pixelY + (std::min)(row, validHeight - 1));
            const uint8_t* sourceRow = image.CpuPixels->Bytes.data() + sourceY * image.RowStrideBytes;
            uint8_t* destinationRow = rgba.data() + static_cast<size_t>(row) * chunkRowBytes;
            for (int column = 0; column < 256; ++column)
            {
                const size_t sourceX = static_cast<size_t>(pixelX + (std::min)(column, validWidth - 1));
                const uint8_t* sourcePixel = sourceRow + sourceX * bytesPerPixel;
                uint8_t* destinationPixel = destinationRow + static_cast<size_t>(column) * bytesPerPixel;
                if (image.PixelFormat == RegisteredTerrainImage::Format::BGRA8)
                {
                    destinationPixel[0] = sourcePixel[2];
                    destinationPixel[1] = sourcePixel[1];
                    destinationPixel[2] = sourcePixel[0];
                }
                else
                {
                    destinationPixel[0] = sourcePixel[0];
                    destinationPixel[1] = sourcePixel[1];
                    destinationPixel[2] = sourcePixel[2];
                }
                destinationPixel[3] = sourcePixel[3];
            }
        }
        return true;
    }

    bool TerrainChunkAcquisition::CopyChunk(const SourceTile& tile, int x, int y,
        const MiniMapNative::TextureAccess::PlatformData& platform,
        const MiniMapNative::TextureAccess::MipData& mip, std::vector<uint8_t>& compressed)
    {
        void* copy = nullptr;
        {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            MiniMapTerrainProfile::Scope profile(MiniMapTerrainProfile::Phase::BulkCopy);
#endif
            if (!MiniMapNative::TextureAccess::CopyBulkData(mip, &copy, false) || !copy) return false;
        }
        const std::unique_ptr<void, decltype(&MiniMapNative::TextureAccess::FreeBulkDataCopy)> owner(
            copy, &MiniMapNative::TextureAccess::FreeBulkDataCopy);
        if (!tile.RegisteredImage && !MiniMapTerrainCache::StoreSourceTile(
            tile, mip.SizeX, mip.SizeY, platform.PixelFormat, platform.SRGB, static_cast<const uint8_t*>(copy), static_cast<size_t>(mip.BulkSize)))
        {
            LOG_WARN("MiniMap: terrain: cache write failed for R%d grid=(%d, %d)",
                tile.RadiationLevel, tile.GridX, tile.GridY);
        }
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
        MiniMapTerrainProfile::Scope profile(MiniMapTerrainProfile::Phase::RequestedExtraction);
#endif
        return MiniMapTerrainChunks::ExtractBC1Chunk(static_cast<const uint8_t*>(copy),
            mip.SizeX, mip.SizeY, x, y, compressed);
    }

    std::string TerrainChunkAcquisition::SharingName(const SourceTile& tile, int x, int y)
    {
        // A fresh backend name avoids truncation/hash aliasing between registered identities.
        // Full source identity remains in the resident key and acquisition log.
        if (tile.RegisteredImage) return {};
        char name[128]{};
        std::snprintf(name, sizeof(name), "MiniMap_Terrain_R%d_%d_%d_%d_%d",
            tile.RadiationLevel, tile.GridX, tile.GridY, x, y);
        return name;
    }

    MiniMap::Services::Texture TerrainChunkAcquisition::FinishUpload(
        MiniMap::Services::UploadState result, MiniMap::Services::OwnedTexture& owner, bool& uncertain)
    {
        using MiniMap::Services::UploadState;
        MiniMap::Services::Texture texture;
        switch (result)
        {
        case UploadState::Complete:
        {
            const auto size = owner.Size();
            texture = owner.View();
            uncertain = !owner.IsValidated() || texture.id == 0 || !size ||
                size->width != 256 || size->height != 256;
            break;
        }
        case UploadState::OwnershipUncertain:
            uncertain = true;
            break;
        case UploadState::Unavailable:
        case UploadState::Rejected:
            uncertain = owner.HasRecord();
            break;
        }
        return texture.id == 0 || uncertain ? MiniMap::Services::Texture{} : texture;
    }
}
#endif
