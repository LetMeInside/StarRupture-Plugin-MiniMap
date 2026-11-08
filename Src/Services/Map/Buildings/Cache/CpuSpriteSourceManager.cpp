#if defined(MODLOADER_CLIENT_BUILD)

// Implements serialized resident sprite ownership and transparent cache retrieval.
#include "CpuSpriteSourceManager.h"

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
#include <chrono>
#endif
#include <utility>

#include "SpriteDiskCache.h"

namespace Services::Map::Buildings::Cache
{
	/** Retains the cache pointer used for cache-aware lookup without taking ownership. */
	CpuSpriteSourceManager::CpuSpriteSourceManager(SpriteDiskCache* spriteDiskCache) noexcept
		: spriteDiskCache_(spriteDiskCache)
	{
	}

	/** Returns a resident payload or loads it once from the configured disk cache. */
	CpuSpriteSourceManager::SpriteResult CpuSpriteSourceManager::GetSprite(const OwnedSpriteKey& key)
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		using Clock = std::chrono::steady_clock;
		const auto lookupStarted = Clock::now();
		SpriteResult result = [&]()
		{
			SpriteResult lookupResult;
			const auto resident = payloadsByKey_.find(key.Bytes());
			if (resident != payloadsByKey_.end())
			{
				lookupResult.Result = Status::Ready;
				lookupResult.Payload = resident->second.get();
				lookupResult.Diagnostics.Route = SpriteResult::LookupEvidence::LookupRoute::ResidentPayloadFound;
				return lookupResult;
			}

			if (spriteDiskCache_ == nullptr)
			{
				lookupResult.Result = Status::Unavailable;
				lookupResult.Diagnostics.Route = SpriteResult::LookupEvidence::LookupRoute::NoDiskCacheProvider;
				return lookupResult;
			}

			const auto diskLoadStarted = Clock::now();
			SpriteDiskCache::LoadResult loaded = spriteDiskCache_->TryLoad(key);
			lookupResult.Diagnostics.DiskLoadMs = std::chrono::duration<double, std::milli>(
				Clock::now() - diskLoadStarted).count();
			if (loaded.Status == SpriteDiskCache::LoadStatus::Loaded && loaded.Payload)
			{
				if (!Adopt(key, std::move(loaded.Payload)))
				{
					lookupResult.Result = Status::Unavailable;
					lookupResult.Diagnostics.Route = SpriteResult::LookupEvidence::LookupRoute::LoadedPayloadCouldNotBeAdopted;
					return lookupResult;
				}
				const auto adopted = payloadsByKey_.find(key.Bytes());
				if (adopted == payloadsByKey_.end())
				{
					lookupResult.Result = Status::Unavailable;
					lookupResult.Diagnostics.Route = SpriteResult::LookupEvidence::LookupRoute::AdoptedPayloadNotFound;
					return lookupResult;
				}
				lookupResult.Result = Status::Ready;
				lookupResult.Payload = adopted->second.get();
				lookupResult.Diagnostics.Route = SpriteResult::LookupEvidence::LookupRoute::DiskLoadedAndPayloadAdopted;
				return lookupResult;
			}

			if (loaded.Status == SpriteDiskCache::LoadStatus::Missing)
			{
				lookupResult.Result = Status::CaptureRequired;
				lookupResult.Diagnostics.Route = SpriteResult::LookupEvidence::LookupRoute::DiskEntryMissing;
			}
			else if (loaded.Status == SpriteDiskCache::LoadStatus::Invalid)
			{
				lookupResult.Result = Status::CaptureRequired;
				lookupResult.Diagnostics.Route = SpriteResult::LookupEvidence::LookupRoute::DiskEntryInvalid;
			}
			else if (loaded.Status == SpriteDiskCache::LoadStatus::IOError)
			{
				lookupResult.Result = Status::Unavailable;
				lookupResult.Diagnostics.Route = SpriteResult::LookupEvidence::LookupRoute::DiskReadIOError;
			}
			else if (loaded.Status == SpriteDiskCache::LoadStatus::Unavailable)
			{
				lookupResult.Result = Status::Unavailable;
				lookupResult.Diagnostics.Route = SpriteResult::LookupEvidence::LookupRoute::DiskReadUnavailable;
			}
			else
			{
				lookupResult.Result = Status::Unavailable;
				lookupResult.Diagnostics.Route = SpriteResult::LookupEvidence::LookupRoute::OtherDiskLoadFailure;
			}
			return lookupResult;
		}();
		result.Diagnostics.TotalMs = std::chrono::duration<double, std::milli>(
			Clock::now() - lookupStarted).count();
		return result;
#else
		const auto resident = payloadsByKey_.find(key.Bytes());
		if (resident != payloadsByKey_.end())
		{
			return {Status::Ready, resident->second.get()};
		}

		if (spriteDiskCache_ == nullptr)
		{
			return {Status::Unavailable, nullptr};
		}

		SpriteDiskCache::LoadResult loaded = spriteDiskCache_->TryLoad(key);
		if (loaded.Status == SpriteDiskCache::LoadStatus::Loaded && loaded.Payload)
		{
			if (!Adopt(key, std::move(loaded.Payload)))
			{
				return {Status::Unavailable, nullptr};
			}
			const auto adopted = payloadsByKey_.find(key.Bytes());
			return adopted == payloadsByKey_.end()
				? SpriteResult{Status::Unavailable, nullptr}
				: SpriteResult{Status::Ready, adopted->second.get()};
		}

		if (loaded.Status == SpriteDiskCache::LoadStatus::Missing ||
			loaded.Status == SpriteDiskCache::LoadStatus::Invalid)
		{
			return {Status::CaptureRequired, nullptr};
		}
		return {Status::Unavailable, nullptr};
#endif
	}

	/** Transfers a captured payload into the map without copying its image allocation. */
	bool CpuSpriteSourceManager::Adopt(
		const OwnedSpriteKey& key,
		std::unique_ptr<const ProcessedSpritePayload>&& payload)
	{
		if (key.Bytes().empty() || !payload || payload->Pixels().empty())
		{
			return false;
		}
		payloadsByKey_.insert_or_assign(key.Bytes(), std::move(payload));
		return true;
	}

	/** Erases one resident payload and expires pointers borrowed from that entry. */
	bool CpuSpriteSourceManager::Withdraw(const OwnedSpriteKey& key) noexcept
	{
		return payloadsByKey_.erase(key.Bytes()) != 0;
	}

	/** Releases all resident payloads and expires every borrowed payload pointer. */
	void CpuSpriteSourceManager::Clear() noexcept
	{
		payloadsByKey_.clear();
	}
}

#endif // MODLOADER_CLIENT_BUILD
