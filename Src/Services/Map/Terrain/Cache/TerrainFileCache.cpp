// Implements the single terrain cache IO owner; root identity and invalidation policy remain application-owned.
#if defined(MODLOADER_CLIENT_BUILD)
#include "TerrainFileCache.h"
#include "NativeTerrainCacheRecord.h"
#include <windows.h>
#include <fstream>

namespace Services::Map::Terrain::Cache
{
    /** Copies supplied naming values; construction performs no file IO. */
    TerrainFileCache::TerrainFileCache(const TerrainService::NativeCacheConfiguration& configuration)
        : configuration_(configuration)
    {
    }

    /** Rejects configuration replacement while supporting repeated identical startup. */
    bool TerrainFileCache::HasConfiguration(const TerrainService::NativeCacheConfiguration& configuration) const
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
    TerrainService::NativeCacheStatus TerrainFileCache::GetDirectory(std::int32_t sourceState,
        std::filesystem::path& directory) const
    {
        using Status = TerrainService::NativeCacheStatus;
        directory = configuration_.Root / (configuration_.DirectoryPrefix + std::to_wstring(sourceState));
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        return error ? Status::DirectoryFailure : Status::Hit;
    }

    /** Reproduces application naming without interpreting indices or source state. */
    std::filesystem::path TerrainFileCache::GetFilePath(const std::filesystem::path& directory,
        const TerrainService::NativeCacheKey& key) const
    {
        return directory / (configuration_.FilePrefix + std::to_wstring(key.TileIndex0) +
            configuration_.IndexSeparator + std::to_wstring(key.TileIndex1) + configuration_.Extension);
    }

    /** Keeps the old output semantics: closed admission leaves output untouched; admitted attempts clear it. */
    TerrainService::NativeCacheResult TerrainFileCache::TryLoadChunk(const TerrainService::NativeCacheKey& key,
        int chunkX, int chunkY, std::vector<std::uint8_t>& chunk,
        const TerrainService::NativeCacheTrace* trace) const
    {
        using Status = TerrainService::NativeCacheStatus;
        std::lock_guard lock(fileMutex_);
        if (closed_) return {};
        Activity load(trace, TerrainService::NativeCacheActivity::Load);
        Activity path(trace, TerrainService::NativeCacheActivity::Path);
        chunk.clear();
        if (key.SourceState < 0 || chunkX < 0 || chunkX >= NativeTerrainCacheRecord::ChunksPerAxis ||
            chunkY < 0 || chunkY >= NativeTerrainCacheRecord::ChunksPerAxis) return {Status::InvalidInput};
        std::filesystem::path directory;
        const Status status = GetDirectory(key.SourceState, directory);
        if (status != Status::Hit) return {status};
        const auto file = GetFilePath(directory, key);
        path.End();
        return ReadFile(file, key, chunkX, chunkY, chunk, trace);
    }

    /** Rejects malformed records without deleting, migrating or automatically rewriting them. */
    TerrainService::NativeCacheResult TerrainFileCache::ReadFile(const std::filesystem::path& file,
        const TerrainService::NativeCacheKey& key, int chunkX, int chunkY,
        std::vector<std::uint8_t>& chunk, const TerrainService::NativeCacheTrace* trace)
    {
        using Status = TerrainService::NativeCacheStatus;
        Activity header(trace, TerrainService::NativeCacheActivity::Header);
        std::error_code error;
        if (!std::filesystem::exists(file, error) || error)
        {
            return {error ? Status::ExistenceFailure : Status::Miss, error};
        }
        const auto size = std::filesystem::file_size(file, error);
        if (error || size != NativeTerrainCacheRecord::FileBytes) return {Status::InvalidFileSize};
        std::ifstream stream(file, std::ios::binary);
        if (!stream) return {Status::OpenFailure};
        if (!NativeTerrainCacheRecord::ReadHeader(stream, key)) return {Status::HeaderMismatch};
        header.End();
        Activity read(trace, TerrainService::NativeCacheActivity::Read);
        if (!NativeTerrainCacheRecord::ReadChunk(stream, chunkX, chunkY, chunk)) return {Status::ReadFailure};
        return {Status::Hit};
    }

    /** Validates complete source bytes before any directory creation, preserving the original write contract. */
    TerrainService::NativeCacheResult TerrainFileCache::StoreSourceTile(const TerrainService::NativeCacheKey& key,
        const std::uint8_t* source, std::size_t size, const TerrainService::NativeCacheTrace* trace) const
    {
        using Status = TerrainService::NativeCacheStatus;
        std::lock_guard lock(fileMutex_);
        if (closed_) return {};
        Activity store(trace, TerrainService::NativeCacheActivity::Store);
        if (!NativeTerrainCacheRecord::CanStore(key, source, size)) return {Status::InvalidInput};
        std::filesystem::path directory;
        const Status status = GetDirectory(key.SourceState, directory);
        if (status != Status::Hit) return {status};
        return WriteFile(GetFilePath(directory, key), key, source, trace);
    }

    /** Preserves .tmp naming, close checking, Windows replacement flags and failure cleanup. */
    TerrainService::NativeCacheResult TerrainFileCache::WriteFile(const std::filesystem::path& file,
        const TerrainService::NativeCacheKey& key, const std::uint8_t* source,
        const TerrainService::NativeCacheTrace* trace)
    {
        using Status = TerrainService::NativeCacheStatus;
        const std::filesystem::path temporary = file.wstring() + L".tmp";
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return {Status::OpenFailure};
        const bool written = NativeTerrainCacheRecord::Write(stream, key, source, trace);
        stream.close();
        if (written && stream && MoveFileExW(temporary.c_str(), file.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return {Status::Stored};
        std::error_code removeError;
        std::filesystem::remove(temporary, removeError);
        return {Status::WriteFailure};
    }
}
#endif
