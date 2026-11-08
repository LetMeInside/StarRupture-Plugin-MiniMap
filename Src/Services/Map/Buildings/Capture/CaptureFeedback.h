// Defines the move-only terminal result handed from sprite capture to acquisition scheduling.
#pragma once

#include "CaptureRequest.h"
#include "CaptureResult.h"
#include "../Cache/OwnedSpriteKey.h"
#include "../Cache/ProcessedSpritePayload.h"

#include <memory>

namespace Services::Map::Buildings::Capture
{
	/** Owns terminal capture identity and optional immutable CPU pixels without native resources.
	 * The value transfers exclusively through the feedback FIFO; no capture, render-target,
	 * readback, engine, or borrowed storage is retained.
	 */
	struct CaptureFeedback final
	{
		/** Restricts feedback to outcomes that finish a request; intermediate states are excluded. */
		enum class Outcome
		{
			Succeeded,
			Failed,
			Cancelled
		};

		/** Canonical owned Blueprint key corresponding to the originating request. */
		Cache::OwnedSpriteKey SpriteRequirement;
		/** Request sequence copied from the originating request for stale-result checks. */
		CaptureRequest::Correlation RequestIdentity;
		/** Terminal success, failure, or cancellation outcome. */
		Outcome Result = Outcome::Failed;
		/** Existing capture failure classification; meaningful when Result is Failed. */
		CaptureResult::Completion::Failure Error = CaptureResult::Completion::Failure::None;
		/** Non-null immutable processed pixels on success, moved without copying the allocation. */
		std::unique_ptr<const Cache::ProcessedSpritePayload> Payload;
	};
}
