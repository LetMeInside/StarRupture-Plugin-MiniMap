// Defines queue-admission outcomes and private terminal capture state for the renderer.
#pragma once

#include "CaptureRequest.h"
#include "../Cache/ProcessedSpritePayload.h"

#include <cstdint>
#include <memory>

namespace Services::Map::Buildings::Capture
{
	/** Groups renderer queue-admission and retained terminal-state values. */
	struct CaptureResult final
	{
		/** Classifies FIFO request admission before the renderer retains request work. */
		struct Acceptance
		{
			/** Why the submitted request was accepted or rejected. */
			enum class Status : std::uint8_t
			{
				Accepted,
				RejectedInvalidRequest,
				RejectedStaleRequest,
				RejectedUnavailable
			};

			/** Correlation copied from the submission so the scheduler can match acceptance. */
			CaptureRequest::Correlation Correlation;
			/** A rejected request is terminal for this submission; accepted work remains pending until completion. */
			Status Result = Status::RejectedInvalidRequest;
		};

		/** Retains internal pending or terminal state until safe feedback handoff.
		 * The payload is move-only and remains renderer-owned until it enters CaptureFeedback.
		 */
		struct Completion
		{
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

			/** Original request correlation used to reject stale completion feedback. */
			CaptureRequest::Correlation Correlation;
			/** Current or terminal request state. */
			Status Result = Status::Pending;
			/** Failure category; meaningful only when Result is Failed. */
			Failure Error = Failure::None;
			/** Completed immutable sprite, owned until moved into terminal feedback. */
			std::unique_ptr<const Cache::ProcessedSpritePayload> Payload;
		};
	};
}
