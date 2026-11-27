// Implements byte-compatible version-one record IO without decoding or recompressing BC1.
#if defined(MODLOADER_CLIENT_BUILD)
#include "NativeTerrainCacheRecord.h"
#include "TerrainFileCache.h"
#include <bit>
#include <cstring>
#include <istream>
#include <ostream>

namespace Services::Map::Terrain::Cache
{
    static_assert(std::endian::native == std::endian::little);

    /** Preserves the original store-only dimension and format restrictions. */
    bool NativeTerrainCacheRecord::CanStore(const TerrainService::NativeCacheKey& key,
        const std::uint8_t* source, std::size_t size) noexcept
    {
        return key.SourceState >= 0 && source != nullptr &&
            key.SourceWidth == SourceDimension && key.SourceHeight == SourceDimension &&
            key.PixelFormat == 5 && size == SourceBytes;
    }

    /** Makes the legacy header; metadata omitted from the original key stays omitted. */
    NativeTerrainCacheRecord::Header NativeTerrainCacheRecord::MakeHeader(
        const TerrainService::NativeCacheKey& key) noexcept
    {
        Header header{};
        std::memcpy(header.Magic, "MMTCACHE", 8);
        header.Version = 1;
        header.TileIndex0 = key.TileIndex0;
        header.TileIndex1 = key.TileIndex1;
        header.SourceState = key.SourceState;
        header.SourceWidth = static_cast<std::uint32_t>(key.SourceWidth);
        header.SourceHeight = static_cast<std::uint32_t>(key.SourceHeight);
        header.PixelFormat = key.PixelFormat;
        header.ChunksPerAxis = ChunksPerAxis;
        header.ChunkWidth = 256;
        header.ChunkHeight = 256;
        header.BytesPerChunk = static_cast<std::uint32_t>(ChunkBytes);
        header.SRGB = key.SRGB ? 1 : 0;
        return header;
    }

    /** Reserved bytes remain unchecked, matching the original reader exactly. */
    bool NativeTerrainCacheRecord::Matches(const Header& header,
        const TerrainService::NativeCacheKey& key) noexcept
    {
        const Header expected = MakeHeader(key);
        return std::memcmp(header.Magic, expected.Magic, 8) == 0 &&
            header.Version == expected.Version && header.TileIndex0 == expected.TileIndex0 &&
            header.TileIndex1 == expected.TileIndex1 && header.SourceState == expected.SourceState &&
            header.SourceWidth == expected.SourceWidth && header.SourceHeight == expected.SourceHeight &&
            header.PixelFormat == expected.PixelFormat && header.ChunksPerAxis == expected.ChunksPerAxis &&
            header.ChunkWidth == expected.ChunkWidth && header.ChunkHeight == expected.ChunkHeight &&
            header.BytesPerChunk == expected.BytesPerChunk && header.SRGB == expected.SRGB;
    }

    /** Rejects short headers and mismatches without modifying the file. */
    bool NativeTerrainCacheRecord::ReadHeader(std::istream& stream,
        const TerrainService::NativeCacheKey& key)
    {
        Header header{};
        stream.read(reinterpret_cast<char*>(&header), sizeof(header));
        return stream && Matches(header, key);
    }

    /** Locates chunks by Y-major chunk index, not full-image BC1 row offset. */
    bool NativeTerrainCacheRecord::ReadChunk(std::istream& stream, int chunkX, int chunkY,
        std::vector<std::uint8_t>& chunk)
    {
        const std::streamoff offset = static_cast<std::streamoff>(sizeof(Header)) +
            static_cast<std::streamoff>(chunkY * ChunksPerAxis + chunkX) *
            static_cast<std::streamoff>(ChunkBytes);
        stream.seekg(offset, std::ios::beg);
        chunk.resize(ChunkBytes);
        stream.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
        if (stream) return true;
        chunk.clear();
        return false;
    }

    /** Preserves the existing 64 block rows of 512 bytes per chunk. */
    void NativeTerrainCacheRecord::ExtractChunk(const std::uint8_t* source, int chunkX, int chunkY,
        std::vector<std::uint8_t>& chunk)
    {
        chunk.resize(ChunkBytes);
        for (std::size_t row = 0; row < 64; ++row)
        {
            const std::size_t sourceOffset = ((chunkY * 64 + row) * 512 + chunkX * 64) * 8;
            std::memcpy(chunk.data() + row * 512, source + sourceOffset, 512);
        }
    }

    /** Streams chunks Y outer/X inner; the caller owns close, replacement and failure cleanup. */
    bool NativeTerrainCacheRecord::Write(std::ostream& stream,
        const TerrainService::NativeCacheKey& key, const std::uint8_t* source,
        const TerrainService::NativeCacheTrace* trace)
    {
        const Header header = MakeHeader(key);
        stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
        std::vector<std::uint8_t> chunk;
        for (int chunkY = 0; chunkY < ChunksPerAxis; ++chunkY)
        {
            for (int chunkX = 0; chunkX < ChunksPerAxis; ++chunkX)
            {
                {
                    TerrainFileCache::Activity extraction(trace, TerrainService::NativeCacheActivity::Extraction);
                    ExtractChunk(source, chunkX, chunkY, chunk);
                }
                stream.write(reinterpret_cast<const char*>(chunk.data()),
                    static_cast<std::streamsize>(chunk.size()));
                if (!stream) return false;
            }
        }
        return true;
    }
}
#endif
