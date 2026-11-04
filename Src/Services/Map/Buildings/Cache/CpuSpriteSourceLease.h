// Retains one immutable processed sprite while exposing independent currentness evidence.
#pragma once

#include "ProcessedSpritePayload.h"

#include <memory>
#include <utility>

namespace Services::Map::Buildings::Cache
{
	/** Keeps a processed sprite readable while independently reporting its eligibility.
	 * The lease owns the payload and a small invalidation token; it does not reference
	 * the manager or service, so it remains safe to inspect after either is destroyed.
	 * Calls to IsCurrent are serialized with manager updates on the service calling thread.
	 */
	class CpuSpriteSourceLease final
	{
	public:
		/** Creates an empty lease used to represent an unavailable source. */
		CpuSpriteSourceLease() noexcept = default;

		/** Returns the retained payload, including dimensions, capture geometry and pixels.
		 * The pointer remains readable while this lease remains alive, even after it becomes stale.
		 * @return Retained immutable payload, or null for an empty lease.
		 */
		const ProcessedSpritePayload* Payload() const noexcept
		{
			return payload_.get();
		}

		/** Reports whether this exact publication remains eligible for current use.
		 * A false result is permanent for this lease, even if its key is later republished.
		 * @return True only while the manager has not replaced, withdrawn or reset this source.
		 */
		bool IsCurrent() const noexcept
		{
			return currentness_ && currentness_->Current;
		}

	private:
		friend class CpuSpriteSourceManager;

		/** Shares revocation state with leases without retaining or referencing their manager.
		 * The service's single-calling-thread rule serializes reads and writes to Current.
		 */
		struct CurrentnessState
		{
			bool Current = true;
		};

		/** Creates a lease over an existing payload and its unique publication token.
		 * @param payload Shared immutable processed source.
		 * @param currentness Publication-specific eligibility state.
		 */
		CpuSpriteSourceLease(
			std::shared_ptr<const ProcessedSpritePayload> payload,
			std::shared_ptr<CurrentnessState> currentness) noexcept
			: payload_(std::move(payload))
			, currentness_(std::move(currentness))
		{
		}

		std::shared_ptr<const ProcessedSpritePayload> payload_;
		std::shared_ptr<CurrentnessState> currentness_;
	};
}
