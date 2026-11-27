// Owns native-compatible terrain cache paths, IO and synchronization for its configured subtree only.
#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "../TerrainService.h"
#include <filesystem>
#include <mutex>

namespace Services::Map::Terrain::Cache
{
    /** Service-owned immutable configuration and per-instance IO exclusion, independent of root-cache management.
     * One mutex serializes complete reads, writes, clears and admission changes. Destruction requires caller quiescence.
     */
    class TerrainFileCache final
    {
    public:
        /** Pairs synchronous diagnostic notifications across early returns/exceptions, without retaining state. */
        class Activity final
        {
        public:
            /** Borrows a call-local trace and starts the specified phase if a callback is supplied. */
            Activity(const TerrainService::NativeCacheTrace* trace, TerrainService::NativeCacheActivity phase) noexcept
                : trace_(trace), phase_(phase)
            {
                if (trace_ && trace_->Notify) trace_->Notify(trace_->Context, phase_, true);
            }
            /** Prevents duplicate end notifications. */
            Activity(const Activity&) = delete;
            /** Prevents replacing an active phase. */
            Activity& operator=(const Activity&) = delete;
            /** Ends the phase before its borrowed call-local trace goes out of scope. */
            ~Activity() noexcept { End(); }
            /** Ends this phase once, allowing the next diagnostic phase to start without overlap. */
            void End() noexcept
            {
                if (trace_ && trace_->Notify) trace_->Notify(trace_->Context, phase_, false);
                trace_ = nullptr;
            }
        private:
            const TerrainService::NativeCacheTrace* trace_;
            TerrainService::NativeCacheActivity phase_;
        };

        /** Copies caller naming/root without creating directories or modifying cache authorization. */
        explicit TerrainFileCache(const TerrainService::NativeCacheConfiguration& configuration);
        /** Tests whether a repeated startup association requests identical immutable settings. */
        bool HasConfiguration(const TerrainService::NativeCacheConfiguration& configuration) const;
        /** Reopens this instance at quiescent application startup; never checks root metadata. */
        void Open();
        /** Waits for active IO, then rejects further reads/writes; clearing remains available while closed. */
        void Close();
        /** Borrows the immutable root for the lifetime of this cache owner. */
        const std::filesystem::path& GetRoot() const noexcept;
        /** Exclusively deletes this subtree, waiting for complete IO and preserving open/closed admission state. */
        bool Clear();
        /** Holds instance exclusion through stream destruction; returns unavailable without changing output if closed. */
        TerrainService::NativeCacheResult TryLoadChunk(const TerrainService::NativeCacheKey& key,
            int chunkX, int chunkY, std::vector<std::uint8_t>& chunk,
            const TerrainService::NativeCacheTrace* trace) const;
        /** Holds instance exclusion through temporary cleanup/publication; rejects writes if closed. */
        TerrainService::NativeCacheResult StoreSourceTile(const TerrainService::NativeCacheKey& key,
            const std::uint8_t* source, std::size_t size, const TerrainService::NativeCacheTrace* trace) const;

    private:
        /** Creates only the compatibility directory under the configured subtree, including on lookup. */
        TerrainService::NativeCacheStatus GetDirectory(std::int32_t sourceState,
            std::filesystem::path& directory) const;
        /** Applies literal caller naming to the two numeric tile indices. */
        std::filesystem::path GetFilePath(const std::filesystem::path& directory,
            const TerrainService::NativeCacheKey& key) const;
        /** Performs existence/length/header/chunk checks while its caller holds instance exclusion. */
        static TerrainService::NativeCacheResult ReadFile(const std::filesystem::path& file,
            const TerrainService::NativeCacheKey& key, int chunkX, int chunkY,
            std::vector<std::uint8_t>& chunk, const TerrainService::NativeCacheTrace* trace);
        /** Writes/closes/publishes a .tmp record and removes it on post-open failure. */
        static TerrainService::NativeCacheResult WriteFile(const std::filesystem::path& file,
            const TerrainService::NativeCacheKey& key, const std::uint8_t* source,
            const TerrainService::NativeCacheTrace* trace);

        const TerrainService::NativeCacheConfiguration configuration_;
        mutable std::mutex fileMutex_;
        bool closed_ = false; // Protected by fileMutex_; independent of application root metadata.
    };
}
#endif
