// Carries the canonical sprite key and scheduler correlation into the
// service-owned independent Building Map capture renderer.
#pragma once

#include "../Cache/OwnedSpriteKey.h"

#include <cstdint>

namespace Services::Map::Buildings::Capture
{
	/** Owns the canonical Blueprint sprite key and correlation for one capture request.
	 * It may be retained by the internal renderer after acceptance; it contains no borrowed
	 * game object, view, or pixel span.
	 */
	struct CaptureRequest
	{
		/** Correlates one request within a scheduler session without identifying a game entity. */
		struct Correlation
		{
			/** Scheduler-owned session generation; stale generations are discarded by the service. */
			std::uint64_t SessionGeneration = 0;
			/** Scheduler-owned request sequence, unique only within its session generation. */
			std::uint64_t RequestSequence = 0;
		};

		/** Session/request identity used for acceptance, cancellation, and stale-result checks. */
		Correlation RequestIdentity;
		/** Canonical Blueprint generated-class path used as the sole per-sprite input. */
		Cache::OwnedSpriteKey SpriteRequirement;
	};
}
