// Implements the single terrain cache IO owner; root identity and invalidation policy remain application-owned.
#if defined(MODLOADER_CLIENT_BUILD)
#include "TerrainFileCache.h"
#include "TerrainCacheRecord.h"
#include <windows.h>
#include <fstream>

namespace Services::Map::Terrain::Cache
{
    /** Copies the existing Terrain root; construction performs no file IO. */
    TerrainFileCache::TerrainFileCache(const TerrainService::CacheConfiguration& configuration)
        : configuration_(configuration)
    {
    }

    /** Rejects configuration replacement while supporting repeated identical startup. */
    bool TerrainFileCache::HasConfiguration(const TerrainService::CacheConfiguration& configuration) const
    {
        return configuration_ == configuration;
    }

    /** Starts a new application run using the same immutable subtree configuration. */
    void TerrainFileCache::Open()
    {
        std::lock_guard lock(fileMutex_);
        closed_ = false;
    }

    /** Drains preceding file operations before closing admission to any queued or later IO. */
    void TerrainFileCache::Close()
    {
        std::lock_guard lock(fileMutex_);
        closed_ = true;
    }

    /** Exposes only the configured terrain subtree, independent of source/session registrations. */
    const std::filesystem::path& TerrainFileCache::GetRoot() const noexcept
    {
        return configuration_.Root;
    }

    /** Uses only subtree exclusion, including when application root clearing already holds its own lock. */
    bool TerrainFileCache::Clear()
    {
        std::lock_guard lock(fileMutex_);
        std::error_code error;
        std::filesystem::remove_all(configuration_.Root, error);
        return !error;
    }

    /** Preserves lookup directory creation without consulting application root metadata or readiness. */
    TerrainService::CacheStatus TerrainFileCache::GetDirectory(std::filesystem::path& directory) const
    {
        using Status = TerrainService::CacheStatus;
        directory = configuration_.Root;
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        return error ? Status::DirectoryFailure : Status::Hit;
    }

    /** Names one complete page directly under the existing Terrain root; never creates subdirectories. */
    bool TerrainFileCache::GetFilePath(const std::filesystem::path& directory,
        const TerrainPageIdentity& page, std::filesystem::path& file) const
    {
        std::wstring filename;
        if (!TerrainCacheRecord::GetFilename(page, filename)) return false;
        file = directory / filename;
        return true;
    }

    /** Keeps the old output semantics: closed admission leaves output untouched; admitted attempts clear it. */
    TerrainService::CacheResult TerrainFileCache::TryLoadChunk(const TerrainCacheIdentity& key,
        int chunkX, int chunkY, std::vector<std::uint8_t>& chunk,
        const TerrainService::CacheTrace* trace) const
    {
        using Status = TerrainService::CacheStatus;
        std::lock_guard lock(fileMutex_);
        if (closed_) return {};
        Activity load(trace, TerrainService::CacheActivity::Load);
        Activity path(trace, TerrainService::CacheActivity::Path);
        chunk.clear();
        std::vector<std::uint8_t> metadata;
        std::uint64_t fileBytes = 0;
        if (!TerrainCacheRecord::Describe(key, metadata, fileBytes) || chunkX < 0 || chunkY < 0 ||
            static_cast<std::uint32_t>(chunkX) >= key.Width / TerrainCacheIdentity::TileDimension ||
            static_cast<std::uint32_t>(chunkY) >= key.Height / TerrainCacheIdentity::TileDimension)
            return {Status::InvalidInput};
        std::filesystem::path directory;
        const Status status = GetDirectory(directory);
        if (status != Status::Hit) return {status};
        std::filesystem::path file;
        if (!GetFilePath(directory, key.Page, file)) return {Status::InvalidInput};
        path.End();
        return ReadFile(file, key, metadata, fileBytes, chunkX, chunkY, chunk, trace);
    }

    /** Rejects malformed records without deleting, migrating or automatically rewriting them. */
    TerrainService::CacheResult TerrainFileCache::ReadFile(const std::filesystem::path& file,
        const TerrainCacheIdentity& key, const std::vector<std::uint8_t>& metadata,
        std::uint64_t fileBytes, int chunkX, int chunkY, std::vector<std::uint8_t>& chunk,
        const TerrainService::CacheTrace* trace)
    {
        using Status = TerrainService::CacheStatus;
        Activity header(trace, TerrainService::CacheActivity::Header);
        std::error_code error;
        if (!std::filesystem::exists(file, error) || error)
        {
            return {error ? Status::ExistenceFailure : Status::Miss, error};
        }
        const auto size = std::filesystem::file_size(file, error);
        if (error || size != fileBytes) return {Status::InvalidFileSize};
        std::ifstream stream(file, std::ios::binary);
        if (!stream) return {Status::OpenFailure};
        if (!TerrainCacheRecord::ReadMetadata(stream, metadata)) return {Status::HeaderMismatch};
        header.End();
        Activity read(trace, TerrainService::CacheActivity::Read);
        if (!TerrainCacheRecord::ReadTile(stream, key, metadata.size(), chunkX, chunkY, chunk)) return {Status::ReadFailure};
        return {Status::Hit};
    }

    /** Validates complete source bytes before any directory creation, preserving the original write contract. */
    TerrainService::CacheResult TerrainFileCache::StoreSourceTile(const TerrainCacheIdentity& key,
        const std::uint8_t* source, std::size_t size, const TerrainService::CacheTrace* trace) const
    {
        using Status = TerrainService::CacheStatus;
        std::lock_guard lock(fileMutex_);
        if (closed_) return {};
        Activity store(trace, TerrainService::CacheActivity::Store);
        std::vector<std::uint8_t> metadata;
        std::uint64_t fileBytes = 0;
        const auto expectedBytes = TerrainImage::CalculateByteCount(key.Width, key.Height);
        if (!source || !expectedBytes || size != *expectedBytes ||
            !TerrainCacheRecord::Describe(key, metadata, fileBytes)) return {Status::InvalidInput};
        std::filesystem::path directory;
        const Status status = GetDirectory(directory);
        if (status != Status::Hit) return {status};
        std::filesystem::path file;
        if (!GetFilePath(directory, key.Page, file)) return {Status::InvalidInput};
        return WriteFile(file, key, metadata, source, trace);
    }

    /** Preserves .tmp naming, close checking, Windows replacement flags and failure cleanup. */
    TerrainService::CacheResult TerrainFileCache::WriteFile(const std::filesystem::path& file,
        const TerrainCacheIdentity& key, const std::vector<std::uint8_t>& metadata,
        const std::uint8_t* source,
        const TerrainService::CacheTrace* trace)
    {
        using Status = TerrainService::CacheStatus;
        const std::filesystem::path temporary = file.wstring() + L".tmp";
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return {Status::OpenFailure};
        const bool written = TerrainCacheRecord::Write(stream, key, metadata,
            {source, *TerrainImage::CalculateByteCount(key.Width, key.Height)}, trace);
        stream.close();
        if (written && stream && MoveFileExW(temporary.c_str(), file.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return {Status::Stored};
        std::error_code removeError;
        std::filesystem::remove(temporary, removeError);
        return {Status::WriteFailure};
    }
}
#endif
