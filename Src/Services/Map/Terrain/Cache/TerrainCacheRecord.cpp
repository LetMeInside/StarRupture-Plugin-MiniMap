// Implements checked generic v2 metadata, provider page naming and tile-major BC1 persistence.
#if defined(MODLOADER_CLIENT_BUILD)
#include "TerrainCacheRecord.h"
#include "TerrainFileCache.h"
#include "../Processing/TerrainBc1Codec.h"
#include <array>
#include <bit>
#include <charconv>
#include <istream>
#include <limits>
#include <ostream>

namespace Services::Map::Terrain::Cache
{
    /** Checks addition before evaluating it. */
    bool TerrainCacheRecord::Add(std::uint64_t left, std::uint64_t right, std::uint64_t& result) noexcept
    {
        if (right > (std::numeric_limits<std::uint64_t>::max)() - left) return false;
        result = left + right;
        return true;
    }

    /** Checks multiplication before evaluating it. */
    bool TerrainCacheRecord::Multiply(std::uint64_t left, std::uint64_t right, std::uint64_t& result) noexcept
    {
        if (left && right > (std::numeric_limits<std::uint64_t>::max)() / left) return false;
        result = left * right;
        return true;
    }

    /** Serializes integers without native packing or endianness assumptions. */
    void TerrainCacheRecord::AppendInteger(std::vector<std::uint8_t>& bytes, std::uint64_t value, unsigned count)
    {
        for (unsigned index = 0; index < count; ++index)
            bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8u)));
    }

    /** Bounds identity allocations and checks both prefix and content sizes. */
    bool TerrainCacheRecord::AppendString(std::vector<std::uint8_t>& bytes, const std::string& value)
    {
        std::uint64_t length = 0;
        if (!Add(bytes.size(), 8, length) || !Add(length, value.size(), length) ||
            length > MaximumIdentityBytes) return false;
        AppendInteger(bytes, value.size(), 8);
        bytes.insert(bytes.end(), value.begin(), value.end());
        return true;
    }

    /** Normalizes signed zero so equal numeric registrations produce identical persistent names. */
    bool TerrainCacheRecord::EncodeIdentity(const TerrainCacheIdentity& identity, std::vector<std::uint8_t>& bytes)
    {
        static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);
        if (!AppendString(bytes, identity.Source.SourceId) ||
            !AppendString(bytes, identity.Source.ContentRevision) || !AppendString(bytes, identity.Locator)) return false;
        std::uint64_t length = 0;
        if (!Add(bytes.size(), 64, length) || length > MaximumIdentityBytes) return false;
        AppendInteger(bytes, identity.Width, 4);
        AppendInteger(bytes, identity.Height, 4);
        AppendInteger(bytes, static_cast<std::uint32_t>(Encoding::BC1), sizeof(std::uint32_t));
        AppendInteger(bytes, identity.SRgb ? 1 : 0, 4);
        const auto& placement = identity.Registration;
        for (double value : {placement.Origin.X, placement.Origin.Y, placement.U.X,
            placement.U.Y, placement.V.X, placement.V.Y})
            AppendInteger(bytes, std::bit_cast<std::uint64_t>(value == 0.0 ? 0.0 : value), 8);
        if (!AppendString(bytes, FoldCollectionKey(identity.Page.CollectionKey))) return false;
        if (!Add(bytes.size(), 8, length) || length > MaximumIdentityBytes) return false;
        AppendInteger(bytes, static_cast<std::uint32_t>(identity.Page.X), 4);
        AppendInteger(bytes, static_cast<std::uint32_t>(identity.Page.Y), 4);
        return true;
    }

    /** Header values are derived from validated dimensions and checked sizes, never input disk fields. */
    void TerrainCacheRecord::AppendHeader(const TerrainCacheIdentity& identity, std::uint64_t identityBytes,
        std::uint64_t payloadBytes, std::uint64_t fileBytes, std::uint64_t tileCount,
        std::vector<std::uint8_t>& bytes)
    {
        constexpr char magic[] = "TRNCACHE";
        bytes.insert(bytes.end(), magic, magic + 8);
        AppendInteger(bytes, 2, 4);
        AppendInteger(bytes, HeaderBytes, 4);
        AppendInteger(bytes, identityBytes, 8);
        AppendInteger(bytes, payloadBytes, 8);
        AppendInteger(bytes, fileBytes, 8);
        AppendInteger(bytes, identity.Width, 4);
        AppendInteger(bytes, identity.Height, 4);
        AppendInteger(bytes, TerrainCacheIdentity::TileDimension, 4);
        AppendInteger(bytes, TerrainCacheIdentity::TileDimension, 4);
        AppendInteger(bytes, identity.Width / TerrainCacheIdentity::TileDimension, 4);
        AppendInteger(bytes, identity.Height / TerrainCacheIdentity::TileDimension, 4);
        AppendInteger(bytes, tileCount, 4);
        AppendInteger(bytes, static_cast<std::uint32_t>(Encoding::BC1), sizeof(std::uint32_t));
        AppendInteger(bytes, identity.SRgb ? 1 : 0, 4);
        AppendInteger(bytes, 1, 4); // Y-major/X-minor tiles, packed BC1 block rows within each tile.
        AppendInteger(bytes, TerrainCacheIdentity::TileBytes, 8);
    }

    /** Checks every variable layout product/sum before allocating metadata or admitting file IO. */
    bool TerrainCacheRecord::Describe(const TerrainCacheIdentity& identity,
        std::vector<std::uint8_t>& metadata, std::uint64_t& fileBytes)
    {
        metadata.clear();
        fileBytes = 0;
        if (!identity.IsValid() || !IsValidCollectionKey(identity.Page.CollectionKey)) return false;
        std::vector<std::uint8_t> encodedIdentity;
        if (!EncodeIdentity(identity, encodedIdentity)) return false;
        std::uint64_t tileCount = 0;
        std::uint64_t payloadBytes = 0;
        std::uint64_t metadataBytes = 0;
        if (!Multiply(identity.Width / TerrainCacheIdentity::TileDimension,
            identity.Height / TerrainCacheIdentity::TileDimension, tileCount) ||
            !Multiply(tileCount, TerrainCacheIdentity::TileBytes, payloadBytes) ||
            !Add(HeaderBytes, encodedIdentity.size(), metadataBytes) ||
            !Add(metadataBytes, payloadBytes, fileBytes) ||
            fileBytes > static_cast<std::uint64_t>((std::numeric_limits<std::streamoff>::max)()) ||
            metadataBytes > (std::numeric_limits<std::size_t>::max)() ||
            metadataBytes > static_cast<std::uint64_t>((std::numeric_limits<std::streamsize>::max)())) return false;
        AppendHeader(identity, encodedIdentity.size(), payloadBytes, fileBytes, tileCount, metadata);
        metadata.insert(metadata.end(), encodedIdentity.begin(), encodedIdentity.end());
        return true;
    }

    /** Rejects invalid names rather than sanitizing/truncating them into another page's namespace. */
    bool TerrainCacheRecord::IsValidCollectionKey(const std::string& key)
    {
        if (key.empty() || key.size() > MaximumCollectionKeyBytes || key.front() == '.' ||
            key.back() == '.' || key.find("..") != std::string::npos) return false;
        for (const char character : key)
        {
            const bool letter = (character >= 'A' && character <= 'Z') ||
                (character >= 'a' && character <= 'z');
            const bool digit = character >= '0' && character <= '9';
            if (!letter && !digit && character != '_' && character != '-' && character != '.') return false;
        }
        return !IsReservedCollectionKey(FoldCollectionKey(key));
    }

    /** Defines case equivalence explicitly for the accepted ASCII alphabet, independent of locale. */
    std::string TerrainCacheRecord::FoldCollectionKey(const std::string& key)
    {
        std::string folded = key;
        for (char& character : folded)
        {
            if (character >= 'A' && character <= 'Z')
                character = static_cast<char>(character + ('a' - 'A'));
        }
        return folded;
    }

    /** Device-name extensions remain reserved; all other prohibited device spellings contain rejected characters. */
    bool TerrainCacheRecord::IsReservedCollectionKey(const std::string& foldedKey)
    {
        const auto stem = foldedKey.substr(0, foldedKey.find('.'));
        if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") return true;
        return stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) &&
            stem[3] >= '1' && stem[3] <= '9';
    }

    /** Twelve bytes exceed the eleven characters needed by INT32_MIN, so to_chars cannot exhaust this buffer. */
    std::string TerrainCacheRecord::FormatPageIndex(std::int32_t index)
    {
        std::array<char, 12> digits{};
        const auto result = std::to_chars(digits.data(), digits.data() + digits.size(), index);
        return {digits.data(), result.ptr};
    }

    /** Preserves provider casing in the filename; Windows aliases share the case-folded stored collection key. */
    bool TerrainCacheRecord::GetFilename(const TerrainPageIdentity& page, std::wstring& filename)
    {
        filename.clear();
        if (!IsValidCollectionKey(page.CollectionKey)) return false;
        const auto name = page.CollectionKey + "_" + FormatPageIndex(page.X) + "_" +
            FormatPageIndex(page.Y) + ".terrain";
        filename.assign(name.begin(), name.end());
        return true;
    }

    /** Never trusts stored length fields for allocation, seeking or identity validation. */
    bool TerrainCacheRecord::ReadMetadata(std::istream& stream, const std::vector<std::uint8_t>& expected)
    {
        std::vector<std::uint8_t> stored(expected.size());
        stream.read(reinterpret_cast<char*>(stored.data()), static_cast<std::streamsize>(stored.size()));
        return stream && stored == expected;
    }

    /** Checks the tile index, byte product and absolute offset before converting to stream offsets. */
    bool TerrainCacheRecord::ReadTile(std::istream& stream, const TerrainCacheIdentity& identity,
        std::size_t metadataBytes, int x, int y, std::vector<std::uint8_t>& tile)
    {
        tile.clear();
        if (!identity.IsValid() || x < 0 || y < 0 ||
            static_cast<std::uint32_t>(x) >= identity.Width / TerrainCacheIdentity::TileDimension ||
            static_cast<std::uint32_t>(y) >= identity.Height / TerrainCacheIdentity::TileDimension) return false;
        std::uint64_t offset = 0;
        std::uint64_t end = 0;
        if (!Multiply(y, identity.Width / TerrainCacheIdentity::TileDimension, offset) ||
            !Add(offset, x, offset) || !Multiply(offset, TerrainCacheIdentity::TileBytes, offset) ||
            !Add(offset, metadataBytes, offset) || !Add(offset, TerrainCacheIdentity::TileBytes, end) ||
            end > static_cast<std::uint64_t>((std::numeric_limits<std::streamoff>::max)())) return false;
        stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        static_assert(TerrainCacheIdentity::TileBytes <= (std::numeric_limits<std::streamsize>::max)());
        tile.resize(static_cast<std::size_t>(TerrainCacheIdentity::TileBytes));
        stream.read(reinterpret_cast<char*>(tile.data()), static_cast<std::streamsize>(tile.size()));
        if (stream) return true;
        tile.clear();
        return false;
    }

    /** Supported pages bound origins to 0..1792; extraction and BC1 validation stay in the existing codec. */
    bool TerrainCacheRecord::Write(std::ostream& stream, const TerrainCacheIdentity& identity,
        const std::vector<std::uint8_t>& metadata, std::span<const std::uint8_t> source,
        const TerrainService::CacheTrace* trace)
    {
        if (!identity.IsValid() || !Processing::TerrainBc1Codec::Validate(source, identity.Width, identity.Height))
            return false;
        stream.write(reinterpret_cast<const char*>(metadata.data()), static_cast<std::streamsize>(metadata.size()));
        std::vector<std::uint8_t> tile;
        for (std::uint32_t y = 0; y < identity.Height; y += TerrainCacheIdentity::TileDimension)
        {
            for (std::uint32_t x = 0; x < identity.Width; x += TerrainCacheIdentity::TileDimension)
            {
                {
                    TerrainFileCache::Activity extraction(trace, TerrainService::CacheActivity::Extraction);
                    if (!Processing::TerrainBc1Codec::Extract(source, identity.Width, identity.Height, x, y,
                        TerrainCacheIdentity::TileDimension, TerrainCacheIdentity::TileDimension, tile)) return false;
                }
                stream.write(reinterpret_cast<const char*>(tile.data()), static_cast<std::streamsize>(tile.size()));
                if (!stream) return false;
            }
        }
        return true;
    }
}
#endif
