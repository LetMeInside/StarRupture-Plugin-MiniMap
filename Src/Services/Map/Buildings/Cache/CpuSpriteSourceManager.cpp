#if defined(MODLOADER_CLIENT_BUILD)

// Implements serialized resident sprite ownership and transparent cache retrieval.
#include "CpuSpriteSourceManager.h"

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
