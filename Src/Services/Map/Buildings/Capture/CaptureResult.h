// Defines capture acceptance, terminal outcomes, and immutable processed-payload
// handoff from the internal capture implementation to BuildingMapService.
#pragma once

#include "CaptureRequest.h"
#include "../Cache/ProcessedSpritePayload.h"

#include <cstdint>
#include <memory>

namespace Services::Map::Buildings::Capture
{
	/** Groups the typed acceptance and completion values used by internal capture work. */
	struct CaptureResult final
	{
		/** Reports synchronous acceptance or rejection before the renderer retains request work. */
		struct Acceptance
		{
			/** Why the submitted request was accepted or rejected. */
			enum class Status : std::uint8_t
			{
				Accepted,
				RejectedInvalidRequest,
				RejectedStaleSession,
				RejectedStaleRequest,
				RejectedUnavailable
			};

			/** Correlation copied from the submission so the scheduler can match acceptance. */
			CaptureRequest::Correlation Correlation;
			/** A rejected request is terminal for this submission; accepted work remains pending until completion. */
			Status Result = Status::RejectedInvalidRequest;
		};

		/** Reports pending, completed, or terminally failed work for one accepted request.
		 * Successful results share the immutable BM-02 payload directly with BM-03; the
		 * capture implementation retains no second CPU sprite store after handoff.
		 */
		struct Completion
		{
			/** Records disk persistence independently from CPU payload readiness. */
			enum class PersistenceStatus : std::uint8_t
			{
				NotAttempted,
				Persisted,
				Failed,
				Unavailable
			};

			/** Distinguishes pending work from each terminal outcome. */
			enum class Status : std::uint8_t
			{
				Pending,
				Completed,
				Failed,
				Cancelled
			};

			/** Describes a terminal failure without retaining native or engine diagnostics. */
			enum class Failure : std::uint8_t
			{
				None,
				SubjectUnavailable,
				EngineOperationFailed,
				ReadbackFailed,
				InvalidCapture,
				ProcessingFailed,
				RetirementFailed,
				StaleRequest
			};

			/** Original service/session correlation used to reject stale completion feedback. */
			CaptureRequest::Correlation Correlation;
			/** Current or terminal request state. */
			Status Result = Status::Pending;
			/** Failure category; meaningful only when Result is Failed. */
			Failure Error = Failure::None;
			/** Disk-cache outcome; BM-07 leaves this NotAttempted until service cache initialization exists. */
			PersistenceStatus Persistence = PersistenceStatus::NotAttempted;
			/** Completed immutable sprite, shared with BM-03 without copying its pixel allocation. */
			std::shared_ptr<const Cache::ProcessedSpritePayload> Payload;
		};
	};
}
