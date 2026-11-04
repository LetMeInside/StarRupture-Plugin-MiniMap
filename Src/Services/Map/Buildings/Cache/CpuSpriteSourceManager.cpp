#if defined(MODLOADER_CLIENT_BUILD)

// Implements keyed immutable sprite retention and permanent lease invalidation.
#include "CpuSpriteSourceManager.h"

#include <utility>

namespace Services::Map::Buildings::Cache
{
	/** Invalidates leases before releasing the manager's current source entries. */
	CpuSpriteSourceManager::~CpuSpriteSourceManager()
	{
		InvalidateAll();
	}

	/** Publishes a validated payload and permanently revokes any prior publication for its key. */
	bool CpuSpriteSourceManager::Publish(
		const OwnedSpriteKey& key,
		std::shared_ptr<const ProcessedSpritePayload> payload)
	{
		if (key.Bytes().empty() || !payload || !payload->Pixels())
		{
			return false;
		}

		std::shared_ptr<CpuSpriteSourceLease::CurrentnessState> currentness =
			std::make_shared<CpuSpriteSourceLease::CurrentnessState>();
		const auto source = sourcesByKey_.find(key.Bytes());
		if (source != sourcesByKey_.end())
		{
			source->second.Currentness->Current = false;
			source->second = {std::move(payload), std::move(currentness)};
			return true;
		}

		sourcesByKey_.emplace(key.Bytes(), SourceEntry{std::move(payload), std::move(currentness)});
		return true;
	}

	/** Returns a lease to the current payload without copying its image allocation. */
	CpuSpriteSourceManager::LookupResult CpuSpriteSourceManager::Lookup(const OwnedSpriteKey& key) const noexcept
	{
		const auto source = sourcesByKey_.find(key.Bytes());
		if (source == sourcesByKey_.end())
		{
			return {};
		}

		return {
			Availability::Ready,
			CpuSpriteSourceLease(source->second.Payload, source->second.Currentness)};
	}

	/** Permanently revokes a source before removing the manager's ownership. */
	bool CpuSpriteSourceManager::Withdraw(const OwnedSpriteKey& key) noexcept
	{
		const auto source = sourcesByKey_.find(key.Bytes());
		if (source == sourcesByKey_.end())
		{
			return false;
		}

		source->second.Currentness->Current = false;
		sourcesByKey_.erase(source);
		return true;
	}

	/** Revokes every outstanding publication token and releases current-entry ownership. */
	void CpuSpriteSourceManager::InvalidateAll() noexcept
	{
		for (auto& source : sourcesByKey_)
		{
			source.second.Currentness->Current = false;
		}
		sourcesByKey_.clear();
	}
}

#endif // MODLOADER_CLIENT_BUILD
