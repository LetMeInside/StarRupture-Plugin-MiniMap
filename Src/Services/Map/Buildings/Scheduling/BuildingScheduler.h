// Coordinates cache-first Blueprint sprite retrieval and one accepted capture request.
#pragma once

#include "../Cache/CpuSpriteSourceManager.h"
#include "../Capture/CaptureRequest.h"

#include <cstdint>

namespace Services::Map::Buildings::Cache
{
	class SpriteDiskCache;
}

namespace Services::Map::Buildings::Scheduling
{
	/** Uses service-owned caches and tracks at most one accepted Blueprint capture.
	 * Cache and request operations are serialized by the owning BuildingMapService.
	 * The service owns this scheduler and both referenced cache objects for its lifetime.
	 */
	class BuildingScheduler final
	{
	public:
		/** Distinguishes a usable source, a confirmed cache miss, and an uncertain cache outcome. */
		enum class LookupStatus
		{
			Ready,
			CaptureRequired,
			Unavailable
		};

		/** Distinguishes a prepared request from missing session or already active work. */
		enum class PreparationStatus
		{
			Prepared,
			Busy,
			Unavailable
		};

		/** Retains service-owned cache references without resolving paths.
		 * @param cpuSpriteSourceManager Service-owned in-memory source manager.
		 * @param spriteDiskCache Service-owned disk cache using the injected Buildings directory.
		 */
		BuildingScheduler(
			Cache::CpuSpriteSourceManager& cpuSpriteSourceManager,
			Cache::SpriteDiskCache& spriteDiskCache) noexcept;

		/** Checks CPU first, then loads and publishes a disk payload when available.
		 * @param key Validated owned canonical Blueprint key.
		 * @return Ready, CaptureRequired for a confirmed miss, or Unavailable for uncertain cache outcomes.
		 */
		LookupStatus Lookup(const Cache::OwnedSpriteKey& key);

		/** Starts a scheduler session and resets request sequencing to one.
		 * @param sessionGeneration Renderer generation already accepted by BuildingMapService.
		 */
		void BeginSession(std::uint64_t sessionGeneration) noexcept;

		/** Clears session and request state after renderer retirement completes. */
		void EndSession() noexcept;

		/** Prepares a correlated request unless the session is absent or work is active.
		 * @param key Canonical Blueprint key copied into the request.
		 * @param request Receives the session generation, next sequence, and sprite requirement.
		 * @return Prepared, Busy when work is active, or Unavailable without a usable session.
		 */
		PreparationStatus PrepareCapture(
			const Cache::OwnedSpriteKey& key,
			Capture::CaptureRequest& request);

		/** Moves the accepted request's key and correlation into the one active slot.
		 * @param request Prepared request whose submission the renderer accepted.
		 */
		void RecordAccepted(Capture::CaptureRequest&& request) noexcept;

		/** Copies active request identity for service-owned advancement.
		 * @param key Receives the accepted canonical Blueprint key.
		 * @param correlation Receives its accepted renderer correlation.
		 * @return True when an accepted request is active.
		 */
		bool GetActiveRequest(
			Cache::OwnedSpriteKey& key,
			Capture::CaptureRequest::Correlation& correlation) const;

		/** Clears only the active request matching the terminal correlation.
		 * @param correlation Correlation reported by the terminal renderer result.
		 */
		void ClearActiveRequest(Capture::CaptureRequest::Correlation correlation) noexcept;

private:
		Cache::CpuSpriteSourceManager& cpuSpriteSourceManager_;
		Cache::SpriteDiskCache& spriteDiskCache_;
		std::uint64_t sessionGeneration_ = 0;
		std::uint64_t nextRequestSequence_ = 1;
		Cache::OwnedSpriteKey activeSpriteKey_;
		Capture::CaptureRequest::Correlation activeCorrelation_;
	};
}
