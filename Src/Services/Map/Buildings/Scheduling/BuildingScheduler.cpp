#if defined(MODLOADER_CLIENT_BUILD)

// Implements CPU-first sprite lookup and minimal one-request capture scheduling.
#include "BuildingScheduler.h"

#include <string>
#include <utility>

#include "../Cache/SpriteDiskCache.h"

namespace Services::Map::Buildings::Scheduling
{
	/** Retains references to the CPU source manager and configured disk cache. */
	BuildingScheduler::BuildingScheduler(
		Cache::CpuSpriteSourceManager& cpuSpriteSourceManager,
		Cache::SpriteDiskCache& spriteDiskCache) noexcept
		: cpuSpriteSourceManager_(cpuSpriteSourceManager)
		, spriteDiskCache_(spriteDiskCache)
	{
	}

	/** Returns CPU sources first, then loads and publishes a usable disk payload. */
	BuildingScheduler::LookupStatus BuildingScheduler::Lookup(const Cache::OwnedSpriteKey& key)
	{
		std::string fileName;
		if (!key.TryGetFileName(fileName))
		{
			return LookupStatus::Unavailable;
		}
		if (cpuSpriteSourceManager_.Lookup(key).Status == Cache::CpuSpriteSourceManager::Availability::Ready)
		{
			return LookupStatus::Ready;
		}

		const Cache::SpriteDiskCache::LoadResult diskResult = spriteDiskCache_.TryLoad(key);
		if (diskResult.Status == Cache::SpriteDiskCache::LoadStatus::Loaded)
		{
			if (!diskResult.Payload || !diskResult.Payload->Pixels())
			{
				return LookupStatus::Unavailable;
			}
			return cpuSpriteSourceManager_.Publish(key, diskResult.Payload)
				? LookupStatus::Ready
				: LookupStatus::Unavailable;
		}
		if (diskResult.Status == Cache::SpriteDiskCache::LoadStatus::Missing ||
			diskResult.Status == Cache::SpriteDiskCache::LoadStatus::Invalid)
		{
			return LookupStatus::CaptureRequired;
		}
		return LookupStatus::Unavailable;
	}

	/** Starts a new renderer-correlated session and resets its first request sequence. */
	void BuildingScheduler::BeginSession(std::uint64_t sessionGeneration) noexcept
	{
		sessionGeneration_ = sessionGeneration;
		nextRequestSequence_ = 1;
		activeSpriteKey_ = {};
		activeCorrelation_ = {};
	}

	/** Clears the current session after renderer shutdown and native retirement complete. */
	void BuildingScheduler::EndSession() noexcept
	{
		sessionGeneration_ = 0;
		nextRequestSequence_ = 1;
		activeSpriteKey_ = {};
		activeCorrelation_ = {};
	}

	/** Prepares the canonical key and next session sequence without recording active work. */
	BuildingScheduler::PreparationStatus BuildingScheduler::PrepareCapture(
		const Cache::OwnedSpriteKey& key,
		Capture::CaptureRequest& request)
	{
		if (sessionGeneration_ == 0 || nextRequestSequence_ == 0)
		{
			return PreparationStatus::Unavailable;
		}
		if (activeCorrelation_.RequestSequence != 0)
		{
			return PreparationStatus::Busy;
		}

		request.RequestIdentity.SessionGeneration = sessionGeneration_;
		request.RequestIdentity.RequestSequence = nextRequestSequence_++;
		request.SpriteRequirement = key;
		return PreparationStatus::Prepared;
	}

	/** Retains one accepted key and correlation after the renderer confirms acceptance. */
	void BuildingScheduler::RecordAccepted(Capture::CaptureRequest&& request) noexcept
	{
		activeSpriteKey_ = std::move(request.SpriteRequirement);
		activeCorrelation_ = request.RequestIdentity;
	}

	/** Copies active request identity for the service to advance and match with renderer progress. */
	bool BuildingScheduler::GetActiveRequest(
		Cache::OwnedSpriteKey& key,
		Capture::CaptureRequest::Correlation& correlation) const
	{
		if (activeCorrelation_.RequestSequence == 0)
		{
			return false;
		}
		key = activeSpriteKey_;
		correlation = activeCorrelation_;
		return true;
	}

	/** Clears state only when the supplied terminal result belongs to the accepted request. */
	void BuildingScheduler::ClearActiveRequest(Capture::CaptureRequest::Correlation correlation) noexcept
	{
		if (correlation.SessionGeneration != activeCorrelation_.SessionGeneration ||
			correlation.RequestSequence != activeCorrelation_.RequestSequence)
		{
			return;
		}
		activeSpriteKey_ = {};
		activeCorrelation_ = {};
	}
}

#endif // MODLOADER_CLIENT_BUILD
