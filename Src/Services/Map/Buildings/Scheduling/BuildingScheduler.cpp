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
		Cache::SpriteDiskCache* spriteDiskCache) noexcept
		: cpuSpriteSourceManager_(cpuSpriteSourceManager)
		, spriteDiskCache_(spriteDiskCache)
	{
	}

	/** Requires session shutdown to have retired every native renderer owner. */
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
			if (sessionGeneration_ != 0)
			{
				spriteStatusesByKey_.insert_or_assign(key.Bytes(), SpriteStatus(SpriteStatus::State::Ready));
			}
			return RequirementStatus::Ready;
		}
		if (spriteResult.Result == Cache::CpuSpriteSourceManager::Status::Unavailable)
		{
			if (sessionGeneration_ != 0)
			{
				spriteStatusesByKey_.insert_or_assign(
					key.Bytes(),
					SpriteStatus(SpriteStatus::State::TemporarilyUnavailable));
			}
			return RequirementStatus::Unavailable;
		}

		return EnqueueCaptureRequest(key);
	}

	/** Creates an inert private renderer for a strictly increasing session generation. */
	bool BuildingScheduler::BeginSession(std::uint64_t sessionGeneration, double pixelsPerMapUnit)
	{
		if (captureRenderer_ || sessionGeneration == 0 || sessionGeneration <= lastSessionGeneration_)
		{
			return false;
		}
		ClearSessionState();
		captureRenderer_.reset(new Capture::BuildingSpriteRenderer(sessionGeneration, pixelsPerMapUnit));
		sessionGeneration_ = sessionGeneration;
		lastSessionGeneration_ = sessionGeneration;
		return true;
	}

	/** Advances ordered native shutdown and clears session values only after renderer retirement. */
	bool BuildingScheduler::AdvanceSessionShutdown()
	{
		if (!captureRenderer_)
		{
			return true;
		}

		if (!sessionShutdownRequested_)
		{
			captureRenderer_->RequestServiceShutdown();
			sessionShutdownRequested_ = true;
		}

		if (!captureRenderer_->AdvanceServiceShutdown())
		{
			return false;
		}
		captureRenderer_.reset();
		ClearSessionState();
		return true;
	}

	/** Advances one renderer operation, then drains all terminal feedback now available. */
	void BuildingScheduler::AdvanceSpriteAcquisition()
	{
		if (!captureRenderer_ || sessionShutdownRequested_)
		{
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

	/** Returns status only for an exact pending request in the active, non-shutting-down session. */
	SpriteStatus* BuildingScheduler::FindMatchingPendingStatus(
		const Capture::CaptureFeedback& feedback) noexcept
	{
		if (sessionGeneration_ == 0 || sessionShutdownRequested_ ||
			feedback.RequestIdentity.SessionGeneration != sessionGeneration_)
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
		sessionGeneration_ = 0;
		nextRequestSequence_ = 1;
		sessionShutdownRequested_ = false;
	}

	/** Creates one owned request after a confirmed cache miss and records it as pending.
	 * @param key Canonical owned Blueprint key to enqueue.
	 * @return Pending when queued, or Unavailable without an active session/sequence.
	 */
	BuildingScheduler::RequirementStatus BuildingScheduler::EnqueueCaptureRequest(
		const Cache::OwnedSpriteKey& key)
	{
		if (sessionGeneration_ == 0 || nextRequestSequence_ == 0 || sessionShutdownRequested_)
		{
			if (sessionGeneration_ != 0)
			{
				spriteStatusesByKey_.insert_or_assign(
					key.Bytes(),
					SpriteStatus(SpriteStatus::State::TemporarilyUnavailable));
			}
			return RequirementStatus::Unavailable;
		}

		Capture::CaptureRequest request;
		request.RequestIdentity.SessionGeneration = sessionGeneration_;
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
