// Coordinates cache-aware sprite retrieval, per-key status, FIFO capture and renderer retirement.
#pragma once

#include "../Cache/CpuSpriteSourceManager.h"
#include "../Capture/CaptureFeedback.h"
#include "../Capture/CaptureRequest.h"
#include "SpriteFeedbackQueue.h"
#include "SpriteRequestQueue.h"
#include "SpriteStatus.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace Services::Map::Buildings::Cache
{
	class SpriteDiskCache;
}
namespace Services::Map::Buildings::Capture
{
	class BuildingSpriteRenderer;
}

namespace Services::Map::Buildings::Scheduling
{
	/** Owns session status and request queues plus one plugin-lifetime private sprite renderer.
	 * It borrows the CPU source manager and disk cache, which must outlive this scheduler.
	 * The renderer survives gameplay sessions; acquisition and payload borrowing are
	 * serialized on the designated game thread.
	 */
	class BuildingScheduler final
	{
	public:
		/** Reports per-key acquisition readiness without exposing SpriteStatus storage. */
		enum class RequirementStatus
		{
			Ready,
			Pending,
			Failed,
			Unavailable
		};

		/** Creates the persistent renderer and borrows the sibling cache/source manager.
		 * @param cpuSpriteSourceManager Owner of resident processed sprites.
		 * @param spriteDiskCache Cache used for capture persistence and by CPU source retrieval.
		 * @param pixelsPerMapUnit Capture scale derived from service Section settings.
		 */
		BuildingScheduler(
			Cache::CpuSpriteSourceManager* cpuSpriteSourceManager,
			Cache::SpriteDiskCache* spriteDiskCache,
			double pixelsPerMapUnit);
		/** Releases the plugin-lifetime renderer object without implicit native cleanup. */
		~BuildingScheduler();

		/** Resolves one requirement through status, CPU source, disk cache, or request FIFO.
		 * A current-session pending or terminal failure suppresses repeat cache work. Temporary
		 * unavailability may be retried on a later call. A missing/invalid disk entry enqueues at
		 * most one owned request for this key in the current session.
		 * @param key Validated owned canonical Blueprint key.
		 * @return Ready, Pending, Failed for a session-local terminal failure, or Unavailable.
		 */
		RequirementStatus EnsureSpriteReady(const Cache::OwnedSpriteKey& key);

		/** Starts scheduling against the existing plugin-lifetime renderer.
		 * Pending cancellation retirement from a previous session does not reject start.
		 * @return True when this scheduler transitions into an active gameplay session.
		 */
		bool BeginSession();

		/** Ends gameplay admission and cancels any active request without waiting.
		 * Native request ownership remains in the renderer until tick-driven retirement.
		 */
		void StopSession();
		/** Advances only pending request retirement, without admitting new requests.
		 * This may run after gameplay ends and while a later gameplay session is active.
		 */
		void AdvancePendingSessionRetirement();

		/** Advances one capture operation, prioritizing prior-session retirement when pending.
		 * Otherwise it uses the normal active-session path and drains terminal feedback.
		 * This internal operation is called on the serialized game thread.
		 */
		void AdvanceSpriteAcquisition();

	private:
		/** Enqueues one owned correlated request after a confirmed cache miss or invalid entry. */
		RequirementStatus EnqueueCaptureRequest(const Cache::OwnedSpriteKey& key);
		/** Drains feedback after request values have left the independently locked FIFO. */
		void ProcessCaptureFeedback();
		/** Clears queued requests, feedback and session-local status without resetting correlation. */
		void ClearSessionState() noexcept;

		/** Finds status only when feedback exactly matches its pending request in the active session.
		 * @param feedback Dequeued terminal message to correlate by request sequence and canonical key.
		 * @return Mutable status entry, or null when the message is stale or unrelated.
		 */
		SpriteStatus* FindMatchingPendingStatus(
			const Capture::CaptureFeedback& feedback) noexcept;

		/** Applies one validated terminal message without retaining it in a second collection.
		 * @param feedback Owned message removed from the feedback FIFO.
		 */
		void ProcessOneCaptureFeedback(Capture::CaptureFeedback&& feedback);

		/** Adopts a validated payload, marks readiness, then attempts independent persistence.
		 * @param feedback Matching successful message whose payload ownership transfers to the CPU manager.
		 * @param status Matching pending per-key status updated after publication eligibility is confirmed.
		 */
		void ProcessSuccessfulCapture(
			Capture::CaptureFeedback&& feedback,
			SpriteStatus& status);

		Cache::CpuSpriteSourceManager* cpuSpriteSourceManager_ = nullptr;
		Cache::SpriteDiskCache* spriteDiskCache_ = nullptr;
		std::uint64_t nextRequestSequence_ = 1;
		bool sessionActive_ = false;
		bool sessionRetirementPending_ = false;
		std::unordered_map<std::string, SpriteStatus> spriteStatusesByKey_;
		SpriteRequestQueue spriteRequestQueue_;
		SpriteFeedbackQueue spriteFeedbackQueue_;
		std::unique_ptr<Capture::BuildingSpriteRenderer> captureRenderer_;
	};
}
