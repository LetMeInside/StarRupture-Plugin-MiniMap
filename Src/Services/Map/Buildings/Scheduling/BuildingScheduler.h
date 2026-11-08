// Coordinates cache-aware sprite retrieval, per-key status, FIFO capture and renderer retirement.
#pragma once

#include "../Cache/CpuSpriteSourceManager.h"
#include "../Capture/CaptureFeedback.h"
#include "../Capture/CaptureRequest.h"
#include "SpriteFeedbackQueue.h"
#include "SpriteRequestQueue.h"
#include "SpriteStatus.h"

#include <cstdint>
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
#include <array>
#include <chrono>
#endif
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
	#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		/** Fixed-size numeric observations for one ten-second acquisition interval. */
		struct DiagnosticsSnapshot
		{
			/** Repeated elapsed CPU wall-time measurements; the reporter derives means. */
			struct Duration
			{
				std::uint64_t Samples = 0;
				double TotalMs = 0.0;
				double MaxMs = 0.0;

				/** Adds one existing operation duration to the aggregate. */
				void Add(double elapsedMs) noexcept
				{
					++Samples;
					TotalMs += elapsedMs;
					if (elapsedMs > MaxMs)
					{
						MaxMs = elapsedMs;
					}
				}
			};

			double IntervalMs = 0.0;
			Duration EnsureSpriteReady;
			Duration DemandSpriteLookup;
			Duration DemandDiskLoad;
			Duration EnqueueCaptureRequest;
			Duration CaptureRendererAdvance;
			Duration CaptureFeedbackDrain;
			Duration CaptureFeedbackProcessing;
			Duration CapturePayloadAdoption;
			Duration PostAdoptionLookup;
			Duration SpritePersistence;
			/** Entries follow the existing RequirementStatus enum declaration order. */
			std::array<std::uint64_t, 4> RequirementStatusCounts{};
			/** Entries follow CpuSpriteSourceManager::SpriteResult::LookupRoute order. */
			std::array<std::uint64_t, 11> SpriteLookupRouteCounts{};
			std::uint64_t CapturePendingShortCircuits = 0;
			std::uint64_t MissingCpuSourceManager = 0;
			std::uint64_t QueuedCaptureRequests = 0;
			std::uint64_t UnavailableEnqueueReturns = 0;
			std::uint64_t UnmatchedFeedback = 0;
			/** Entries follow the existing CaptureFeedback::Outcome declaration order. */
			std::array<std::uint64_t, 3> MatchedFeedbackOutcomeCounts{};
			/** Entry zero is Adopt success; entry one is Adopt failure. */
			std::array<std::uint64_t, 2> CaptureAdoptionOutcomeCounts{};
			std::uint64_t ReadinessPublications = 0;
			/** Entries follow the existing SpriteDiskCache::WriteStatus declaration order. */
			std::array<std::uint64_t, 4> SpritePersistenceStatusCounts{};
		};
	#endif

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

		/** Requests Start and begins scheduling against the existing plugin-lifetime renderer.
		 * @return True when this scheduler transitions into an active gameplay session.
		 */
		bool BeginSession();

		/** Ends gameplay admission and requests renderer-owned Stop processing. */
		void StopSession();
		/** Returns the persistent capturer used by the plugin's direct engine callback. */
		Capture::BuildingSpriteRenderer* GetCapturerLifecycleTarget() noexcept;

		/** Advances one ordinary capture operation and drains terminal feedback. */
		void AdvanceSpriteAcquisition();
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		/** Hands off and resets one due interval without advancing functional work. */
		bool TakeDiagnosticsSnapshot(DiagnosticsSnapshot& output) noexcept;
#endif

	private:
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		/** Adds elapsed time to one scheduler-owned aggregate when a scope exits. */
		struct DiagnosticsDurationScope final
		{
			explicit DiagnosticsDurationScope(DiagnosticsSnapshot::Duration& aggregate) noexcept;
			~DiagnosticsDurationScope() noexcept;
			void Finish() noexcept;

		private:
			DiagnosticsSnapshot::Duration& aggregate_;
			std::chrono::steady_clock::time_point started_;
			bool active_ = true;
		};

		/** Counts one actual EnsureSpriteReady result category. */
		void RecordRequirementStatus(RequirementStatus status) noexcept;
		/** Runs the existing acquisition path while recording path-specific evidence. */
		RequirementStatus EnsureSpriteReadyWithDiagnostics(const Cache::OwnedSpriteKey& key);
		/** Consumes one demand-side GetSprite route and its existing timing evidence. */
		void RecordDemandSpriteLookup(const Cache::CpuSpriteSourceManager::SpriteResult& result) noexcept;
		/** Resets only scheduler diagnostic observations and restarts their interval. */
		void ResetDiagnostics() noexcept;
#endif

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
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		/** Persists one already-published CPU sprite when the configured cache exists. */
		void PersistReadySprite(
			const Cache::OwnedSpriteKey& key,
			const Cache::ProcessedSpritePayload& payload);
#endif

		Cache::CpuSpriteSourceManager* cpuSpriteSourceManager_ = nullptr;
		Cache::SpriteDiskCache* spriteDiskCache_ = nullptr;
		std::uint64_t nextRequestSequence_ = 1;
		bool sessionActive_ = false;
		std::unordered_map<std::string, SpriteStatus> spriteStatusesByKey_;
		SpriteRequestQueue spriteRequestQueue_;
		SpriteFeedbackQueue spriteFeedbackQueue_;
		std::unique_ptr<Capture::BuildingSpriteRenderer> captureRenderer_;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		DiagnosticsSnapshot diagnostics_{};
		std::chrono::steady_clock::time_point diagnosticsIntervalStart_{};
#endif
	};
}
