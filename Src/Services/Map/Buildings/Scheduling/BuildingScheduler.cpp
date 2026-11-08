#if defined(MODLOADER_CLIENT_BUILD)

// Implements status-aware resident/cache retrieval, FIFO capture and terminal payload publication.
#include "BuildingScheduler.h"

#include <utility>

#include "../Cache/SpriteDiskCache.h"
#include "../Capture/BuildingSpriteRenderer.h"

namespace Services::Map::Buildings::Scheduling
{
	/** Retains references to the CPU source manager and configured disk cache. */
	BuildingScheduler::BuildingScheduler(
		Cache::CpuSpriteSourceManager* cpuSpriteSourceManager,
		Cache::SpriteDiskCache* spriteDiskCache,
		double pixelsPerMapUnit)
		: cpuSpriteSourceManager_(cpuSpriteSourceManager)
		, spriteDiskCache_(spriteDiskCache)
		, captureRenderer_(new Capture::BuildingSpriteRenderer(pixelsPerMapUnit))
	{
	}

	/** Destroys the plugin-lifetime renderer object without implicit native cleanup. */
	BuildingScheduler::~BuildingScheduler() = default;

	/** Returns current readiness or queues one owned request after a confirmed disk miss. */
	BuildingScheduler::RequirementStatus BuildingScheduler::EnsureSpriteReady(
		const Cache::OwnedSpriteKey& key)
	{
		const auto statusEntry = spriteStatusesByKey_.find(key.Bytes());
		if (statusEntry != spriteStatusesByKey_.end())
		{
			if (statusEntry->second.GetState() == SpriteStatus::State::CapturePending)
			{
				return RequirementStatus::Pending;
			}
			if (statusEntry->second.GetState() == SpriteStatus::State::TerminalFailure)
			{
				return RequirementStatus::Failed;
			}
		}

		if (cpuSpriteSourceManager_ == nullptr)
		{
			return RequirementStatus::Unavailable;
		}

		const Cache::CpuSpriteSourceManager::SpriteResult spriteResult =
			cpuSpriteSourceManager_->GetSprite(key);
		if (spriteResult.Result == Cache::CpuSpriteSourceManager::Status::Ready &&
			spriteResult.Payload != nullptr)
		{
			if (captureRenderer_)
			{
				spriteStatusesByKey_.insert_or_assign(key.Bytes(), SpriteStatus(SpriteStatus::State::Ready));
			}
			return RequirementStatus::Ready;
		}
		if (spriteResult.Result == Cache::CpuSpriteSourceManager::Status::Unavailable)
		{
			if (captureRenderer_)
			{
				spriteStatusesByKey_.insert_or_assign(
					key.Bytes(),
					SpriteStatus(SpriteStatus::State::TemporarilyUnavailable));
			}
			return RequirementStatus::Unavailable;
		}

		return EnqueueCaptureRequest(key);
	}

	/** Starts gameplay scheduling without replacing the persistent renderer. */
	bool BuildingScheduler::BeginSession()
	{
		if (sessionActive_)
		{
			return false;
		}
		ClearSessionState();
		sessionActive_ = true;
		return true;
	}

	/** Stops session admission and requests cancellation without waiting for native retirement. */
	void BuildingScheduler::StopSession()
	{
		if (!sessionActive_)
		{
			return;
		}
		sessionActive_ = false;
		ClearSessionState();
		if (captureRenderer_ != nullptr)
		{
			sessionRetirementPending_ = !captureRenderer_->RequestSessionStop();
		}
	}

	/** Advances cancellation retirement after stop and preserves the shared renderer world. */
	void BuildingScheduler::AdvancePendingSessionRetirement()
	{
		if (!sessionRetirementPending_ || captureRenderer_ == nullptr)
		{
			return;
		}
		sessionRetirementPending_ = !captureRenderer_->AdvanceSessionRetirement(spriteFeedbackQueue_);
		spriteFeedbackQueue_.Clear();
	}

	/** Advances one capture operation or pending stop retirement, then drains terminal feedback. */
	void BuildingScheduler::AdvanceSpriteAcquisition()
	{
		if (!sessionActive_ || captureRenderer_ == nullptr)
		{
			return;
		}
		if (sessionRetirementPending_)
		{
			AdvancePendingSessionRetirement();
			return;
		}

		captureRenderer_->AdvanceQueueDrivenCapture(spriteRequestQueue_, spriteFeedbackQueue_);
		ProcessCaptureFeedback();
	}

	/** Drains feedback in FIFO order; each queue lock ends before message processing starts. */
	void BuildingScheduler::ProcessCaptureFeedback()
	{
		Capture::CaptureFeedback feedback;
		while (spriteFeedbackQueue_.TryDequeue(feedback))
		{
			ProcessOneCaptureFeedback(std::move(feedback));
		}
	}

	/** Returns status only for an exact pending request in an active, non-shutting-down renderer session. */
	SpriteStatus* BuildingScheduler::FindMatchingPendingStatus(
		const Capture::CaptureFeedback& feedback) noexcept
	{
		if (!sessionActive_ || captureRenderer_ == nullptr)
		{
			return nullptr;
		}

		const auto statusEntry = spriteStatusesByKey_.find(feedback.SpriteRequirement.Bytes());
		if (statusEntry == spriteStatusesByKey_.end() ||
			!statusEntry->second.IsPendingFor(feedback.RequestIdentity))
		{
			return nullptr;
		}

		return &statusEntry->second;
	}

	/** Discards stale results and applies only a message matching current per-key pending state. */
	void BuildingScheduler::ProcessOneCaptureFeedback(Capture::CaptureFeedback&& feedback)
	{
		SpriteStatus* const status = FindMatchingPendingStatus(feedback);
		if (status == nullptr)
		{
			return;
		}

		switch (feedback.Result)
		{
		case Capture::CaptureFeedback::Outcome::Succeeded:
			ProcessSuccessfulCapture(std::move(feedback), *status);
			return;
		case Capture::CaptureFeedback::Outcome::Failed:
			if (feedback.Error == Capture::CaptureResult::Completion::Failure::StaleRequest ||
				feedback.Error == Capture::CaptureResult::Completion::Failure::None)
			{
				status->MarkTemporarilyUnavailable();
				return;
			}
			status->MarkTerminalFailure(feedback.RequestIdentity);
			return;
		case Capture::CaptureFeedback::Outcome::Cancelled:
			status->MarkTemporarilyUnavailable();
			return;
		}
	}

	/** Publishes the exact shared payload, confirms current eligibility, then attempts independent persistence. */
	void BuildingScheduler::ProcessSuccessfulCapture(
		Capture::CaptureFeedback&& feedback,
		SpriteStatus& status)
	{
		if (cpuSpriteSourceManager_ == nullptr || !feedback.Payload || feedback.Payload->Pixels().empty())
		{
			status.MarkTemporarilyUnavailable();
			return;
		}

		if (!cpuSpriteSourceManager_->Adopt(feedback.SpriteRequirement, std::move(feedback.Payload)))
		{
			status.MarkTemporarilyUnavailable();
			return;
		}

		const Cache::CpuSpriteSourceManager::SpriteResult publishedSource =
			cpuSpriteSourceManager_->GetSprite(feedback.SpriteRequirement);
		if (publishedSource.Result != Cache::CpuSpriteSourceManager::Status::Ready ||
			publishedSource.Payload == nullptr)
		{
			status.MarkTemporarilyUnavailable();
			return;
		}

		if (!status.IsPendingFor(feedback.RequestIdentity))
		{
			return;
		}
		status.MarkReady();
		if (spriteDiskCache_ != nullptr)
		{
			(void)spriteDiskCache_->TryWrite(feedback.SpriteRequirement, *publishedSource.Payload);
		}
	}

	/** Clears session-local requests, feedback and status without touching native renderer state. */
	void BuildingScheduler::ClearSessionState() noexcept
	{
		spriteRequestQueue_.Clear();
		spriteFeedbackQueue_.Clear();
		spriteStatusesByKey_.clear();
	}

	/** Creates one owned request after a confirmed cache miss and records it as pending.
	 * @param key Canonical owned Blueprint key to enqueue.
	 * @return Pending when queued, or Unavailable without an active session/sequence.
	 */
	BuildingScheduler::RequirementStatus BuildingScheduler::EnqueueCaptureRequest(
		const Cache::OwnedSpriteKey& key)
	{
		if (!sessionActive_ || captureRenderer_ == nullptr || nextRequestSequence_ == 0)
		{
			if (captureRenderer_)
			{
				spriteStatusesByKey_.insert_or_assign(
					key.Bytes(),
					SpriteStatus(SpriteStatus::State::TemporarilyUnavailable));
			}
			return RequirementStatus::Unavailable;
		}

		Capture::CaptureRequest request;
		request.RequestIdentity.RequestSequence = nextRequestSequence_;
		request.SpriteRequirement = key;

		SpriteStatus pendingStatus(SpriteStatus::State::CapturePending);
		pendingStatus.MarkPending(request.RequestIdentity);
		spriteStatusesByKey_.insert_or_assign(key.Bytes(), std::move(pendingStatus));
		spriteRequestQueue_.Enqueue(std::move(request));
		++nextRequestSequence_;
		return RequirementStatus::Pending;
	}
}

#endif // MODLOADER_CLIENT_BUILD
