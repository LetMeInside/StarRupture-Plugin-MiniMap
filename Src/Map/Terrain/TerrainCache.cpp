#if defined(MODLOADER_CLIENT_BUILD)

#include "TerrainCache.h"
#include "TerrainChunks.h"
#include "CacheIdentity.h"
#include "PluginPaths.h"

#include "plugin_helpers.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <string>
#include <vector>

namespace
{
    constexpr std::array<char, 8> kCacheMagic =
    {
        'M', 'M', 'T', 'C', 'A', 'C', 'H', 'E'
    };

    constexpr uint32_t kCacheVersion = 1;


    constexpr uint32_t kBC1PixelFormat = 5;

    constexpr uint32_t kChunkByteCount =
        (MiniMapTerrainChunks::kChunkSizePixels /
            MiniMapTerrainChunks::kBC1BlockSizePixels) *
        (MiniMapTerrainChunks::kChunkSizePixels /
            MiniMapTerrainChunks::kBC1BlockSizePixels) *
        MiniMapTerrainChunks::kBC1BytesPerBlock;

    constexpr uint32_t kChunkCount =
        MiniMapTerrainChunks::kChunksPerAxis *
        MiniMapTerrainChunks::kChunksPerAxis;

#pragma pack(push, 1)
    struct CacheHeader
    {
        char Magic[8];
        uint32_t Version;

        int32_t GridX;
        int32_t GridY;
        int32_t RadiationLevel;

        uint32_t SourceWidth;
        uint32_t SourceHeight;

        uint32_t PixelFormat;
        uint32_t ChunksPerAxis;
        uint32_t ChunkWidth;
        uint32_t ChunkHeight;
        uint32_t BytesPerChunk;

        uint8_t SRGB;
        uint8_t Reserved[7];
    };


#pragma pack(pop)


    static_assert(
        sizeof(CacheHeader) == 60);

    bool GetPluginDirectory(
        std::filesystem::path& outDirectory)
    {
        const char* failure = nullptr;
        if (!MiniMapPluginPaths::GetPluginDirectory(outDirectory, &failure))
        {
            LOG_ERROR(
                "MiniMap: TerrainCache: "
                "plugin module directory resolution failed: %s", failure);

            return false;
        }
        return true;
    }


    bool GetTerrainCacheRoot(
        std::filesystem::path& outDirectory)
    {
        std::filesystem::path pluginDirectory;

        if (!GetPluginDirectory(
            pluginDirectory))
        {
            return false;
        }

        outDirectory =
            pluginDirectory /
            L"MiniMap" /
            L"Cache" /
            L"Terrain";

        return true;
    }


    bool GetCacheDirectory(
        int radiationLevel,
        std::filesystem::path& outDirectory)
    {
        if (!MiniMapCacheIdentity::IsReady())
        {
            return false;
        }

        std::filesystem::path root;

        if (!GetTerrainCacheRoot(
            root))
        {
            return false;
        }

        const std::wstring radiationDirectory =
            L"R" +
            std::to_wstring(
                radiationLevel);

        outDirectory =
            root /
            radiationDirectory;

        std::error_code error;

        std::filesystem::create_directories(
            outDirectory,
            error);

        if (error)
        {
            LOG_ERROR(
                "MiniMap: TerrainCache: "
                "failed to create cache directory");

            return false;
        }

        return true;
    }


    std::filesystem::path GetCacheFilePath(
        const std::filesystem::path& directory,
        int gridX,
        int gridY)
    {
        const std::wstring fileName =
            L"Terrain_" +
            std::to_wstring(
                gridX) +
            L"_" +
            std::to_wstring(
                gridY) +
            L".terrain";

        return directory /
            fileName;
    }


    CacheHeader MakeHeader(
        int radiationLevel,
        int gridX,
        int gridY,
        int sourceWidth,
        int sourceHeight,
        uint8_t pixelFormat,
        bool sRGB)
    {
        CacheHeader header = {};

        for (size_t i = 0;
            i < kCacheMagic.size();
            ++i)
        {
            header.Magic[i] =
                kCacheMagic[i];
        }

        header.Version =
            kCacheVersion;

        header.GridX =
            gridX;

        header.GridY =
            gridY;

        header.RadiationLevel =
            radiationLevel;

        header.SourceWidth =
            static_cast<uint32_t>(
                sourceWidth);

        header.SourceHeight =
            static_cast<uint32_t>(
                sourceHeight);

        header.PixelFormat =
            pixelFormat;

        header.ChunksPerAxis =
            MiniMapTerrainChunks::kChunksPerAxis;

        header.ChunkWidth =
            MiniMapTerrainChunks::kChunkSizePixels;

        header.ChunkHeight =
            MiniMapTerrainChunks::kChunkSizePixels;

        header.BytesPerChunk =
            kChunkByteCount;

        header.SRGB =
            sRGB ? 1 : 0;

        return header;
    }


    bool ValidateHeader(
        const CacheHeader& header,
        int radiationLevel,
        int gridX,
        int gridY,
        int sourceWidth,
        int sourceHeight,
        uint8_t pixelFormat,
        bool sRGB)
    {
        for (size_t i = 0;
            i < kCacheMagic.size();
            ++i)
        {
            if (header.Magic[i] !=
                kCacheMagic[i])
            {
                return false;
            }
        }

        return
            header.Version == kCacheVersion &&
            header.GridX == gridX &&
            header.GridY == gridY &&
            header.RadiationLevel == radiationLevel &&
            header.SourceWidth ==
            static_cast<uint32_t>(
                sourceWidth) &&
            header.SourceHeight ==
            static_cast<uint32_t>(
                sourceHeight) &&
            header.PixelFormat ==
            static_cast<uint32_t>(
                pixelFormat) &&
            header.ChunksPerAxis ==
            MiniMapTerrainChunks::kChunksPerAxis &&
            header.ChunkWidth ==
            MiniMapTerrainChunks::kChunkSizePixels &&
            header.ChunkHeight ==
            MiniMapTerrainChunks::kChunkSizePixels &&
            header.BytesPerChunk ==
            kChunkByteCount &&
            header.SRGB ==
            static_cast<uint8_t>(
                sRGB ? 1 : 0);
    }


    uint64_t ExpectedCacheFileSize()
    {
        return
            static_cast<uint64_t>(
                sizeof(CacheHeader)) +
            static_cast<uint64_t>(
                kChunkCount) *
            static_cast<uint64_t>(
                kChunkByteCount);
    }
}


namespace MiniMapTerrainCache
{
    bool GetCacheRoot(std::filesystem::path& outDirectory)
    {
        return GetTerrainCacheRoot(outDirectory);
    }


    bool Clear()
    {
        std::filesystem::path root;
        if (!GetTerrainCacheRoot(root))
        {
            return false;
        }

        std::error_code error;
        std::filesystem::remove_all(root, error);
        if (error)
        {
            LOG_ERROR(
                "MiniMap: TerrainCache: "
                "failed to clear terrain cache");
            return false;
        }

        return true;
    }


    bool GetBuildingDiagnosticsDirectory(std::filesystem::path& out)
    {
        if(!GetPluginDirectory(out))return false;
        out/=L"MiniMap\\Cache\\Buildings\\Diagnostics";
        return true;
    }
    bool TryLoadChunk(
        int radiationLevel,
        int gridX,
        int gridY,
        int sourceWidth,
        int sourceHeight,
        uint8_t pixelFormat,
        bool sRGB,
        int chunkX,
        int chunkY,
        std::vector<uint8_t>& compressedChunk)
    {
        compressedChunk.clear();

        if (radiationLevel < 0)
        {
            return false;
        }

        if (chunkX < 0 ||
            chunkX >= MiniMapTerrainChunks::kChunksPerAxis ||
            chunkY < 0 ||
            chunkY >= MiniMapTerrainChunks::kChunksPerAxis)
        {
            return false;
        }

        std::filesystem::path directory;

        if (!GetCacheDirectory(
            radiationLevel,
            directory))
        {
            return false;
        }

        const std::filesystem::path filePath =
            GetCacheFilePath(
                directory,
                gridX,
                gridY);

        std::error_code error;

        if (!std::filesystem::exists(
            filePath,
            error) ||
            error)
        {
            LOG_INFO(
                "MiniMap: TerrainCache: "
                "MISS R%d grid=(%d, %d)",
                radiationLevel,
                gridX,
                gridY);

            return false;
        }

        const uint64_t fileSize =
            MiniMapPluginPaths::FileSize(
                filePath,
                error);

        if (error ||
            fileSize != ExpectedCacheFileSize())
        {
            LOG_WARN(
                "MiniMap: TerrainCache: "
                "invalid cache file size for R%d grid=(%d, %d)",
                radiationLevel,
                gridX,
                gridY);

            return false;
        }

        std::ifstream stream(
            filePath,
            std::ios::binary);

        if (!stream)
        {
            LOG_WARN(
                "MiniMap: TerrainCache: "
                "failed to open cache file for R%d grid=(%d, %d)",
                radiationLevel,
                gridX,
                gridY);

            return false;
        }

        CacheHeader header = {};

        stream.read(
            reinterpret_cast<char*>(
                &header),
            sizeof(header));

        if (!stream ||
            !ValidateHeader(
                header,
                radiationLevel,
                gridX,
                gridY,
                sourceWidth,
                sourceHeight,
                pixelFormat,
                sRGB))
        {
            LOG_WARN(
                "MiniMap: TerrainCache: "
                "cache header mismatch for R%d grid=(%d, %d)",
                radiationLevel,
                gridX,
                gridY);

            return false;
        }

        const uint32_t chunkIndex =
            static_cast<uint32_t>(
                chunkY *
                MiniMapTerrainChunks::kChunksPerAxis +
                chunkX);

        const std::streamoff offset =
            static_cast<std::streamoff>(
                sizeof(CacheHeader)) +
            static_cast<std::streamoff>(
                chunkIndex) *
            static_cast<std::streamoff>(
                kChunkByteCount);

        stream.seekg(
            offset,
            std::ios::beg);

        compressedChunk.resize(
            kChunkByteCount);

        stream.read(
            reinterpret_cast<char*>(
                compressedChunk.data()),
            static_cast<std::streamsize>(
                compressedChunk.size()));

        if (!stream)
        {
            compressedChunk.clear();

            LOG_WARN(
                "MiniMap: TerrainCache: "
                "failed to read chunk=(%d, %d) "
                "for R%d grid=(%d, %d)",
                chunkX,
                chunkY,
                radiationLevel,
                gridX,
                gridY);

            return false;
        }

        LOG_INFO(
            "MiniMap: TerrainCache: "
            "HIT R%d grid=(%d, %d) chunk=(%d, %d) bytes=%u",
            radiationLevel,
            gridX,
            gridY,
            chunkX,
            chunkY,
            kChunkByteCount);

        return true;
    }


    bool StoreSourceTile(
        int radiationLevel,
        int gridX,
        int gridY,
        int sourceWidth,
        int sourceHeight,
        uint8_t pixelFormat,
        bool sRGB,
        const uint8_t* sourceData,
        size_t sourceDataSize)
    {
        if (radiationLevel < 0 ||
            sourceData == nullptr ||
            sourceWidth !=
            MiniMapTerrainChunks::kChunksPerAxis *
            MiniMapTerrainChunks::kChunkSizePixels ||
            sourceHeight !=
            MiniMapTerrainChunks::kChunksPerAxis *
            MiniMapTerrainChunks::kChunkSizePixels ||
            pixelFormat != kBC1PixelFormat)
        {
            return false;
        }

        const size_t expectedSourceBytes =
            static_cast<size_t>(
                sourceWidth /
                MiniMapTerrainChunks::kBC1BlockSizePixels) *
            static_cast<size_t>(
                sourceHeight /
                MiniMapTerrainChunks::kBC1BlockSizePixels) *
            MiniMapTerrainChunks::kBC1BytesPerBlock;

        if (sourceDataSize !=
            expectedSourceBytes)
        {
            return false;
        }

        std::filesystem::path directory;

        if (!GetCacheDirectory(
            radiationLevel,
            directory))
        {
            return false;
        }

        const std::filesystem::path filePath =
            GetCacheFilePath(
                directory,
                gridX,
                gridY);

        const std::filesystem::path temporaryPath =
            filePath.wstring() +
            L".tmp";

        std::ofstream stream(
            temporaryPath,
            std::ios::binary |
            std::ios::trunc);

        if (!stream)
        {
            return false;
        }

        const CacheHeader header =
            MakeHeader(
                radiationLevel,
                gridX,
                gridY,
                sourceWidth,
                sourceHeight,
                pixelFormat,
                sRGB);

        stream.write(
            reinterpret_cast<const char*>(
                &header),
            sizeof(header));

        std::vector<uint8_t> chunk;

        for (int chunkY = 0;
            chunkY < MiniMapTerrainChunks::kChunksPerAxis;
            ++chunkY)
        {
            for (int chunkX = 0;
                chunkX < MiniMapTerrainChunks::kChunksPerAxis;
                ++chunkX)
            {
                if (!MiniMapTerrainChunks::ExtractBC1Chunk(
                    sourceData,
                    sourceWidth,
                    sourceHeight,
                    chunkX,
                    chunkY,
                    chunk))
                {
                    stream.close();

                    std::error_code removeError;

                    std::filesystem::remove(
                        temporaryPath,
                        removeError);

                    return false;
                }

                if (chunk.size() !=
                    kChunkByteCount)
                {
                    stream.close();

                    std::error_code removeError;

                    std::filesystem::remove(
                        temporaryPath,
                        removeError);

                    return false;
                }

                stream.write(
                    reinterpret_cast<const char*>(
                        chunk.data()),
                    static_cast<std::streamsize>(
                        chunk.size()));

                if (!stream)
                {
                    stream.close();

                    std::error_code removeError;

                    std::filesystem::remove(
                        temporaryPath,
                        removeError);

                    return false;
                }
            }
        }

        stream.close();

        if (!stream)
        {
            std::error_code removeError;

            std::filesystem::remove(
                temporaryPath,
                removeError);

            return false;
        }

        if (!MoveFileExW(
            temporaryPath.c_str(),
            filePath.c_str(),
            MOVEFILE_REPLACE_EXISTING |
            MOVEFILE_WRITE_THROUGH))
        {
            std::error_code removeError;

            std::filesystem::remove(
                temporaryPath,
                removeError);

            return false;
        }

        LOG_INFO(
            "MiniMap: TerrainCache: "
            "stored R%d grid=(%d, %d) chunks=%u bytes=%llu",
            radiationLevel,
            gridX,
            gridY,
            kChunkCount,
            static_cast<unsigned long long>(
                ExpectedCacheFileSize()));

        return true;
    }
}

#endif
