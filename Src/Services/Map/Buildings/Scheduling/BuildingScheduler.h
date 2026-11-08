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
	/** Owns active-session status, request/feedback queues and the private sprite renderer.
	 * It borrows the CPU source manager and disk cache, which must outlive this scheduler.
	 * Acquisition and payload borrowing are serialized on the designated game thread.
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

		/** Borrows the sibling cache and resident CPU source manager.
		 * @param cpuSpriteSourceManager Owner of resident processed sprites.
		 * @param spriteDiskCache Cache used for capture persistence and by CPU source retrieval.
		 */
		BuildingScheduler(
			Cache::CpuSpriteSourceManager* cpuSpriteSourceManager,
			Cache::SpriteDiskCache* spriteDiskCache) noexcept;
		/** Releases renderer storage; callers must complete ordered session shutdown first. */
		~BuildingScheduler();

		/** Resolves one requirement through status, CPU source, disk cache, or request FIFO.
		 * A current-session pending or terminal failure suppresses repeat cache work. Temporary
		 * unavailability may be retried on a later call. A missing/invalid disk entry enqueues at
		 * most one owned request for this key in the current session.
		 * @param key Validated owned canonical Blueprint key.
		 * @return Ready, Pending, Failed for a session-local terminal failure, or Unavailable.
		 */
		RequirementStatus EnsureSpriteReady(const Cache::OwnedSpriteKey& key);

		/** Creates a renderer when no renderer session is active.
		 * @param pixelsPerMapUnit Capture scale passed to the renderer.
		 * @return True when a new renderer session is established.
		 */
		bool BeginSession(double pixelsPerMapUnit);

		/** Requests ordered shutdown and advances retirement until the renderer can be released.
		 * @return True when renderer retirement completed and session-local state was cleared.
		 */
		bool AdvanceSessionShutdown();

		/** Advances the renderer once, then drains its available terminal feedback.
		 * This internal acquisition operation is called on the serialized game thread.
		 */
		void AdvanceSpriteAcquisition();

	private:
		/** Enqueues one owned correlated request after a confirmed cache miss or invalid entry. */
		RequirementStatus EnqueueCaptureRequest(const Cache::OwnedSpriteKey& key);
		/** Drains feedback after request values have left the independently locked FIFO. */
		void ProcessCaptureFeedback();
		/** Clears scheduler-only session values after renderer retirement has completed. */
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
		bool sessionShutdownRequested_ = false;
		std::unordered_map<std::string, SpriteStatus> spriteStatusesByKey_;
		SpriteRequestQueue spriteRequestQueue_;
		SpriteFeedbackQueue spriteFeedbackQueue_;
		std::unique_ptr<Capture::BuildingSpriteRenderer> captureRenderer_;
	};
}
