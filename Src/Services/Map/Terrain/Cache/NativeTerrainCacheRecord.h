// Serializes the fixed version-one native-compatible BC1 record; never interprets source metadata.
#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "../TerrainService.h"
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <vector>

namespace Services::Map::Terrain::Cache
{
    /** Stateless codec for the original packed header and chunk-major BC1 payload. */
    class NativeTerrainCacheRecord final
    {
    public:
        static constexpr int ChunksPerAxis = 8;
        static constexpr int SourceDimension = 2048;
        static constexpr std::size_t ChunkBytes = 32768;
        static constexpr std::size_t SourceBytes = 2097152;
        static constexpr std::uint64_t FileBytes = 2097212;

        /** Checks the original full-tile write preconditions; no source acquisition is performed. */
        static bool CanStore(const TerrainService::NativeCacheKey& key,
            const std::uint8_t* source, std::size_t size) noexcept;
        /** Reads and validates the header against caller metadata, deliberately ignoring reserved bytes. */
        static bool ReadHeader(std::istream& stream, const TerrainService::NativeCacheKey& key);
        /** Reads exactly one chunk, clearing output on a short read or seek failure. */
        static bool ReadChunk(std::istream& stream, int chunkX, int chunkY,
            std::vector<std::uint8_t>& chunk);
        /** Writes the zero-reserved header and all 64 chunks, preserving compressed bytes. */
        static bool Write(std::ostream& stream, const TerrainService::NativeCacheKey& key,
            const std::uint8_t* source, const TerrainService::NativeCacheTrace* trace);

    private:
#pragma pack(push, 1)
        /** Exactly the existing 60-byte disk header, using neutral numeric source labels. */
        struct Header
        {
            char Magic[8];
            std::uint32_t Version;
            std::int32_t TileIndex0;
            std::int32_t TileIndex1;
            std::int32_t SourceState;
            std::uint32_t SourceWidth;
            std::uint32_t SourceHeight;
            std::uint32_t PixelFormat;
            std::uint32_t ChunksPerAxis;
            std::uint32_t ChunkWidth;
            std::uint32_t ChunkHeight;
            std::uint32_t BytesPerChunk;
            std::uint8_t SRGB;
            std::uint8_t Reserved[7];
        };
#pragma pack(pop)
        static_assert(sizeof(Header) == 60);
        static_assert(FileBytes == sizeof(Header) + ChunksPerAxis * ChunksPerAxis * ChunkBytes);
        static_assert(SourceBytes == ChunksPerAxis * ChunksPerAxis * ChunkBytes);
        static_assert(offsetof(Header, Magic) == 0);
        static_assert(offsetof(Header, Version) == 8);
        static_assert(offsetof(Header, TileIndex0) == 12);
        static_assert(offsetof(Header, TileIndex1) == 16);
        static_assert(offsetof(Header, SourceState) == 20);
        static_assert(offsetof(Header, SourceWidth) == 24);
        static_assert(offsetof(Header, SourceHeight) == 28);
        static_assert(offsetof(Header, PixelFormat) == 32);
        static_assert(offsetof(Header, ChunksPerAxis) == 36);
        static_assert(offsetof(Header, ChunkWidth) == 40);
        static_assert(offsetof(Header, ChunkHeight) == 44);
        static_assert(offsetof(Header, BytesPerChunk) == 48);
        static_assert(offsetof(Header, SRGB) == 52);
        static_assert(offsetof(Header, Reserved) == 53);
        /** Constructs original field values, including zeroed reserved bytes. */
        static Header MakeHeader(const TerrainService::NativeCacheKey& key) noexcept;
        /** Compares only the historically validated fields. */
        static bool Matches(const Header& header, const TerrainService::NativeCacheKey& key) noexcept;
        /** Copies one block-row-strided chunk from a validated full source tile. */
        static void ExtractChunk(const std::uint8_t* source, int chunkX, int chunkY,
            std::vector<std::uint8_t>& chunk);
    };
}
#endif
