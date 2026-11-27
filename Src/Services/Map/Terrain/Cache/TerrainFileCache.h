// Owns generic terrain page cache paths, IO and synchronization for its configured subtree only.
#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "../TerrainService.h"
#include "TerrainCacheIdentity.h"
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
            Activity(const TerrainService::CacheTrace* trace, TerrainService::CacheActivity phase) noexcept
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                : trace_(trace), phase_(phase)
#endif
            {
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                if (trace_ && trace_->Notify) trace_->Notify(trace_->Context, phase_, true);
#else
                (void)trace;
                (void)phase;
#endif
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
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
                if (trace_ && trace_->Notify) trace_->Notify(trace_->Context, phase_, false);
                trace_ = nullptr;
#endif
            }
        private:
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
            const TerrainService::CacheTrace* trace_;
            TerrainService::CacheActivity phase_;
#endif
        };

        /** Copies the existing Terrain root without creating directories or modifying cache authorization. */
        explicit TerrainFileCache(const TerrainService::CacheConfiguration& configuration);
        /** Tests whether a repeated startup association requests identical immutable settings. */
        bool HasConfiguration(const TerrainService::CacheConfiguration& configuration) const;
        /** Reopens this instance at quiescent application startup; never checks root metadata. */
        void Open();
        /** Waits for active IO, then rejects further reads/writes; clearing remains available while closed. */
        void Close();
        /** Borrows the immutable root for the lifetime of this cache owner. */
        const std::filesystem::path& GetRoot() const noexcept;
        /** Exclusively deletes this subtree, waiting for complete IO and preserving open/closed admission state. */
        bool Clear();
        /** Holds IO exclusion through validated reading and failed-lookup directory repair.
         * Successful reads need no directory creation check; closed admission leaves output unchanged.
         */
        TerrainService::CacheResult TryLoadChunk(const TerrainCacheIdentity& key,
            int chunkX, int chunkY, std::vector<std::uint8_t>& chunk,
            const TerrainService::CacheTrace* trace) const;
        /** Holds instance exclusion through temporary cleanup/publication; rejects writes if closed. */
        TerrainService::CacheResult StoreSourceTile(const TerrainCacheIdentity& key,
            const std::uint8_t* source, std::size_t size, const TerrainService::CacheTrace* trace) const;

    private:
        /** Creates only the configured Terrain root, including on lookup. */
        TerrainService::CacheStatus GetDirectory(std::filesystem::path& directory) const;
        /** Constructs a validated provider page filename directly inside the configured Terrain root. */
        bool GetFilePath(const std::filesystem::path& directory,
            const TerrainPageIdentity& page, std::filesystem::path& file) const;
        /** Performs existence/length/header/chunk checks while its caller holds instance exclusion. */
        static TerrainService::CacheResult ReadFile(const std::filesystem::path& file,
            const TerrainCacheIdentity& key, const std::vector<std::uint8_t>& metadata,
            std::uint64_t fileBytes, int chunkX, int chunkY, std::vector<std::uint8_t>& chunk,
            const TerrainService::CacheTrace* trace);
        /** Writes/closes/publishes a .tmp record and removes it on post-open failure. */
        static TerrainService::CacheResult WriteFile(const std::filesystem::path& file,
            const TerrainCacheIdentity& key, const std::vector<std::uint8_t>& metadata,
            const std::uint8_t* source,
            const TerrainService::CacheTrace* trace);

        const TerrainService::CacheConfiguration configuration_;
        mutable std::mutex fileMutex_;
        bool closed_ = true; // Protected by fileMutex_; independent of application root metadata.
    };
}
#endif
