#if defined(MODLOADER_CLIENT_BUILD)

// Implements status-aware resident/cache retrieval, FIFO capture and terminal payload publication.
#include "BuildingScheduler.h"

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
#include <chrono>
#endif
#include <utility>

#include "../Cache/SpriteDiskCache.h"
#include "../Capture/BuildingSpriteRenderer.h"

namespace Services::Map::Buildings::Scheduling
{
	/** Retains references to the CPU source manager and configured disk cache. */
	BuildingScheduler::BuildingScheduler(
		Cache::SpriteSourceManager* spriteSourceManager,
		Cache::SpriteDiskCache* spriteDiskCache,
		double spriteCaptureDensityPixelsPerMapUnit)
		: spriteSourceManager_(spriteSourceManager)
		, spriteDiskCache_(spriteDiskCache)
		, captureRenderer_(new Capture::BuildingSpriteRenderer(spriteCaptureDensityPixelsPerMapUnit))
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		ResetDiagnostics();
#endif
	}

	/** Destroys the plugin-lifetime renderer object without implicit native cleanup. */
	BuildingScheduler::~BuildingScheduler() = default;

	/** Returns current readiness or queues one owned request after a confirmed disk miss. */
	BuildingScheduler::RequirementStatus BuildingScheduler::EnsureSpriteReady(
		const Cache::OwnedSpriteKey& key)
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		DiagnosticsDurationScope ensureTimer(diagnostics_.EnsureSpriteReady);
		const RequirementStatus result = EnsureSpriteReadyWithDiagnostics(key);
		RecordRequirementStatus(result);
		return result;
#else
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

		if (spriteSourceManager_ == nullptr)
		{
			return RequirementStatus::Unavailable;
		}

		const Cache::SpriteSourceManager::SpriteResult spriteResult =
			spriteSourceManager_->GetSprite(key);
		if (spriteResult.Result == Cache::SpriteSourceManager::Status::Ready &&
			spriteResult.Payload != nullptr)
		{
			if (captureRenderer_)
			{
				spriteStatusesByKey_.insert_or_assign(key.Bytes(), SpriteStatus(SpriteStatus::State::Ready));
			}
			return RequirementStatus::Ready;
		}
		if (spriteResult.Result == Cache::SpriteSourceManager::Status::Unavailable)
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
#endif
	}

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
	/** Preserves acquisition decisions while recording short-circuit and lookup evidence. */
	BuildingScheduler::RequirementStatus BuildingScheduler::EnsureSpriteReadyWithDiagnostics(
		const Cache::OwnedSpriteKey& key)
	{
		const auto statusEntry = spriteStatusesByKey_.find(key.Bytes());
		if (statusEntry != spriteStatusesByKey_.end())
		{
			if (statusEntry->second.GetState() == SpriteStatus::State::CapturePending)
			{
				++diagnostics_.CapturePendingShortCircuits;
				return RequirementStatus::Pending;
			}
			if (statusEntry->second.GetState() == SpriteStatus::State::TerminalFailure)
			{
				return RequirementStatus::Failed;
			}
		}
		if (spriteSourceManager_ == nullptr)
		{
			++diagnostics_.MissingCpuSourceManager;
			return RequirementStatus::Unavailable;
		}
		const Cache::SpriteSourceManager::SpriteResult spriteResult =
			spriteSourceManager_->GetSprite(key);
		RecordDemandSpriteLookup(spriteResult);
		if (spriteResult.Result == Cache::SpriteSourceManager::Status::Ready &&
			spriteResult.Payload != nullptr)
		{
			if (captureRenderer_)
			{
				spriteStatusesByKey_.insert_or_assign(key.Bytes(), SpriteStatus(SpriteStatus::State::Ready));
			}
			return RequirementStatus::Ready;
		}
		if (spriteResult.Result == Cache::SpriteSourceManager::Status::Unavailable)
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
#endif

	/** Starts gameplay scheduling without replacing the persistent renderer. */
	bool BuildingScheduler::BeginSession()
	{
		if (sessionActive_ || captureRenderer_ == nullptr || !captureRenderer_->RequestStart())
		{
			return false;
		}
		ClearSessionState();
		sessionActive_ = true;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		ResetDiagnostics();
#endif
		return true;
	}

	/** Stops session admission and signals the renderer's atomic Stop command. */
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
			captureRenderer_->RequestStop();
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		ResetDiagnostics();
#endif
	}

	/** Returns the persistent renderer for direct plugin engine-callback execution. */
	Capture::BuildingSpriteRenderer* BuildingScheduler::GetCapturerLifecycleTarget() noexcept
	{
		return captureRenderer_.get();
	}

	/** Advances ordinary capture only while gameplay scheduling remains active. */
	void BuildingScheduler::AdvanceSpriteAcquisition()
	{
		if (!sessionActive_ || captureRenderer_ == nullptr)
		{
			return;
		}

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		DiagnosticsDurationScope rendererAdvanceTimer(diagnostics_.CaptureRendererAdvance);
#endif
		captureRenderer_->AdvanceQueueDrivenCapture(spriteRequestQueue_, spriteFeedbackQueue_);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		rendererAdvanceTimer.Finish();
#endif
		ProcessCaptureFeedback();
	}

	/** Drains feedback in FIFO order; each queue lock ends before message processing starts. */
	void BuildingScheduler::ProcessCaptureFeedback()
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		DiagnosticsDurationScope feedbackDrainTimer(diagnostics_.CaptureFeedbackDrain);
#endif
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
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		DiagnosticsDurationScope feedbackTimer(diagnostics_.CaptureFeedbackProcessing);
#endif
		SpriteStatus* const status = FindMatchingPendingStatus(feedback);
		if (status == nullptr)
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			++diagnostics_.UnmatchedFeedback;
#endif
			return;
		}

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const auto outcomeIndex = static_cast<std::size_t>(feedback.Result);
		if (outcomeIndex < diagnostics_.MatchedFeedbackOutcomeCounts.size())
		{
			++diagnostics_.MatchedFeedbackOutcomeCounts[outcomeIndex];
		}
#endif
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
		if (spriteSourceManager_ == nullptr || !feedback.Payload || feedback.Payload->Pixels().empty())
		{
			status.MarkTemporarilyUnavailable();
			return;
		}

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		DiagnosticsDurationScope adoptionTimer(diagnostics_.CapturePayloadAdoption);
		if (!spriteSourceManager_->Adopt(feedback.SpriteRequirement, std::move(feedback.Payload)))
		{
			adoptionTimer.Finish();
			++diagnostics_.CaptureAdoptionOutcomeCounts[1];
			status.MarkTemporarilyUnavailable();
			return;
		}
		adoptionTimer.Finish();
		++diagnostics_.CaptureAdoptionOutcomeCounts[0];
#else
		if (!spriteSourceManager_->Adopt(feedback.SpriteRequirement, std::move(feedback.Payload)))
		{
			status.MarkTemporarilyUnavailable();
			return;
		}
#endif

		const Cache::SpriteSourceManager::SpriteResult publishedSource =
			spriteSourceManager_->GetSprite(feedback.SpriteRequirement);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		diagnostics_.PostAdoptionLookup.Add(publishedSource.Diagnostics.TotalMs);
#endif
		if (publishedSource.Result != Cache::SpriteSourceManager::Status::Ready ||
			publishedSource.Payload == nullptr)
		{
			status.MarkTemporarilyUnavailable();
			return;
		}

		if (!status.IsPendingFor(feedback.RequestIdentity))
		{
			return;
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		status.MarkReady();
		++diagnostics_.ReadinessPublications;
#else
		status.MarkReady();
#endif
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		PersistReadySprite(feedback.SpriteRequirement, *publishedSource.Payload);
#else
		if (spriteDiskCache_ != nullptr)
		{
			(void)spriteDiskCache_->TryWrite(feedback.SpriteRequirement, *publishedSource.Payload);
		}
#endif
	}

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
	/** Persists a published CPU payload independently from readiness and capture status. */
	void BuildingScheduler::PersistReadySprite(
		const Cache::OwnedSpriteKey& key,
		const Cache::ProcessedSpritePayload& payload)
	{
		if (spriteDiskCache_ == nullptr)
		{
			return;
		}
		DiagnosticsDurationScope persistenceTimer(diagnostics_.SpritePersistence);
		const Cache::SpriteDiskCache::WriteStatus persistenceStatus =
			spriteDiskCache_->TryWrite(key, payload);
		persistenceTimer.Finish();
		const auto persistenceIndex = static_cast<std::size_t>(persistenceStatus);
		if (persistenceIndex < diagnostics_.SpritePersistenceStatusCounts.size())
		{
			++diagnostics_.SpritePersistenceStatusCounts[persistenceIndex];
		}
	}
#endif

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
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		DiagnosticsDurationScope enqueueTimer(diagnostics_.EnqueueCaptureRequest);
#endif
		if (!sessionActive_ || captureRenderer_ == nullptr || nextRequestSequence_ == 0)
		{
			if (captureRenderer_)
			{
				spriteStatusesByKey_.insert_or_assign(
					key.Bytes(),
					SpriteStatus(SpriteStatus::State::TemporarilyUnavailable));
			}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			++diagnostics_.UnavailableEnqueueReturns;
#endif
			return RequirementStatus::Unavailable;
		}

		Capture::CaptureRequest request;
		request.RequestIdentity.RequestSequence = nextRequestSequence_;
		request.SpriteRequirement = key;

		SpriteStatus pendingStatus(SpriteStatus::State::CapturePending);
		pendingStatus.MarkPending(request.RequestIdentity);
		spriteStatusesByKey_.insert_or_assign(key.Bytes(), std::move(pendingStatus));
		spriteRequestQueue_.Enqueue(std::move(request));
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		++diagnostics_.QueuedCaptureRequests;
#endif
		++nextRequestSequence_;
		return RequirementStatus::Pending;
	}

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
	/** Starts one scheduler-owned duration measurement at its existing operation boundary. */
	BuildingScheduler::DiagnosticsDurationScope::DiagnosticsDurationScope(
		DiagnosticsSnapshot::Duration& aggregate) noexcept
		: aggregate_(aggregate)
		, started_(std::chrono::steady_clock::now())
	{
	}

	/** Completes a duration measurement when its operation scope exits. */
	BuildingScheduler::DiagnosticsDurationScope::~DiagnosticsDurationScope() noexcept
	{
		Finish();
	}

	/** Stores one elapsed steady-clock sample; repeated completion is harmless. */
	void BuildingScheduler::DiagnosticsDurationScope::Finish() noexcept
	{
		if (!active_)
		{
			return;
		}
		const auto elapsedMs = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - started_).count();
		aggregate_.Add(elapsedMs);
		active_ = false;
	}

	/** Counts one existing readiness result without changing the returned status. */
	void BuildingScheduler::RecordRequirementStatus(RequirementStatus status) noexcept
	{
		const auto statusIndex = static_cast<std::size_t>(status);
		if (statusIndex < diagnostics_.RequirementStatusCounts.size())
		{
			++diagnostics_.RequirementStatusCounts[statusIndex];
		}
	}

	/** Aggregates demand-side source evidence without counting post-adoption validation as a hit. */
	void BuildingScheduler::RecordDemandSpriteLookup(
		const Cache::SpriteSourceManager::SpriteResult& result) noexcept
	{
		using LookupRoute = Cache::SpriteSourceManager::SpriteResult::LookupEvidence::LookupRoute;
		const LookupRoute route = result.Diagnostics.Route;
		const auto routeIndex = static_cast<std::size_t>(route);
		if (routeIndex < diagnostics_.SpriteLookupRouteCounts.size())
		{
			++diagnostics_.SpriteLookupRouteCounts[routeIndex];
		}
		diagnostics_.DemandSpriteLookup.Add(result.Diagnostics.TotalMs);
		switch (route)
		{
		case LookupRoute::DiskLoadedAndPayloadAdopted:
		case LookupRoute::DiskEntryMissing:
		case LookupRoute::DiskEntryInvalid:
		case LookupRoute::DiskReadIOError:
		case LookupRoute::DiskReadUnavailable:
		case LookupRoute::LoadedPayloadCouldNotBeAdopted:
		case LookupRoute::AdoptedPayloadNotFound:
		case LookupRoute::OtherDiskLoadFailure:
			diagnostics_.DemandDiskLoad.Add(result.Diagnostics.DiskLoadMs);
			break;
		case LookupRoute::Unknown:
		case LookupRoute::ResidentPayloadFound:
		case LookupRoute::NoDiskCacheProvider:
			break;
		}
	}

	/** Resets only numeric observations and starts a fresh ten-second diagnostic interval. */
	void BuildingScheduler::ResetDiagnostics() noexcept
	{
		diagnostics_ = {};
		diagnosticsIntervalStart_ = std::chrono::steady_clock::now();
	}

	/** Hands off one due interval only when acquisition work was actually recorded. */
	bool BuildingScheduler::TakeDiagnosticsSnapshot(DiagnosticsSnapshot& output) noexcept
	{
		const auto now = std::chrono::steady_clock::now();
		const double intervalMs = std::chrono::duration<double, std::milli>(
			now - diagnosticsIntervalStart_).count();
		if (intervalMs < 10000.0 ||
			(diagnostics_.EnsureSpriteReady.Samples == 0 && diagnostics_.CaptureRendererAdvance.Samples == 0))
		{
			return false;
		}
		diagnostics_.IntervalMs = intervalMs;
		output = diagnostics_;
		ResetDiagnostics();
		return true;
	}
#endif
}

#endif // MODLOADER_CLIENT_BUILD
