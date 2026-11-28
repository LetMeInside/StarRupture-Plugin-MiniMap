// Encodes generic v2 page records and stable filenames; never resolves application sources.
#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "TerrainCacheIdentity.h"
#include "../TerrainService.h"
#include <iosfwd>

namespace Services::Map::Terrain::Cache
{
    /** Stateless little-endian record codec; callers own file admission, locking and safe replacement. */
    class TerrainCacheRecord final
    {
    public:
        /** Builds canonical metadata and checked lengths; false makes the page ineligible for persistence. */
        static bool Describe(const TerrainCacheIdentity& identity, std::vector<std::uint8_t>& metadata,
            std::uint64_t& fileBytes);
        /** Validates page naming and constructs its configured stem or CollectionKey_X_Y.terrain. */
        static bool GetFilename(const TerrainPageIdentity& page, std::wstring& filename);
        /** Compares every stored metadata byte to expected identity/layout before permitting a tile read. */
        static bool ReadMetadata(std::istream& stream, const std::vector<std::uint8_t>& expected);
        /** Reads one checked Y-major/X-minor tile; clears output on rejection or a short read. */
        static bool ReadTile(std::istream& stream, const TerrainCacheIdentity& identity,
            std::size_t metadataBytes, int x, int y, std::vector<std::uint8_t>& tile);
        /** Writes metadata and full page via the existing BC1 extractor; observer is borrowed only. */
        static bool Write(std::ostream& stream, const TerrainCacheIdentity& identity,
            const std::vector<std::uint8_t>& metadata, std::span<const std::uint8_t> source,
            const TerrainService::CacheTrace* trace);

    private:
        /** Stable on-disk uint32 encoding identifiers; unsupported values never admit payload reads. */
        enum class Encoding : std::uint32_t
        {
            BC1 = 1
        };
        static constexpr std::uint64_t HeaderBytes = 88;
        static constexpr std::uint64_t MaximumIdentityBytes = 1024u * 1024u;
        static constexpr std::size_t MaximumCollectionKeyBytes = 128;
        /** Requires bounded ASCII component spelling and rejects traversal and Windows device names. */
        static bool IsValidCollectionKey(const std::string& key);
        /** Folds ASCII uppercase only, without locale rules or changing unsupported characters. */
        static std::string FoldCollectionKey(const std::string& key);
        /** Checks the case-folded stem before the first period against reserved Windows device names. */
        static bool IsReservedCollectionKey(const std::string& foldedKey);
        /** Formats every int32 value as locale-independent signed decimal without padding or a plus sign. */
        static std::string FormatPageIndex(std::int32_t index);
        /** Adds unsigned record lengths without overflow; leaves result unspecified on rejection. */
        static bool Add(std::uint64_t left, std::uint64_t right, std::uint64_t& result) noexcept;
        /** Multiplies unsigned layout values without overflow. */
        static bool Multiply(std::uint64_t left, std::uint64_t right, std::uint64_t& result) noexcept;
        /** Appends the requested low bytes in explicit little-endian order. */
        static void AppendInteger(std::vector<std::uint8_t>& bytes, std::uint64_t value, unsigned count);
        /** Appends a bounded byte string prefixed by its uint64 byte length; embedded NULs are preserved. */
        static bool AppendString(std::vector<std::uint8_t>& bytes, const std::string& value);
        /** Encodes strings, interpretation and canonical IEEE-754 affine registration in fixed field order. */
        static bool EncodeIdentity(const TerrainCacheIdentity& identity, std::vector<std::uint8_t>& bytes);
        /** Constructs the fixed 88-byte header from already checked layout sizes. */
        static void AppendHeader(const TerrainCacheIdentity& identity, std::uint64_t identityBytes,
            std::uint64_t payloadBytes, std::uint64_t fileBytes, std::uint64_t tileCount,
            std::vector<std::uint8_t>& bytes);
    };
}
#endif
