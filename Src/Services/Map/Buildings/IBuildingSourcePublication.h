// Defines retained adapted rendering inputs for downstream sprite preparation.
#pragma once

#include "BuildingSourceView.h"

namespace Services::Map::Buildings
{
	/** Owns or shares one immutable adapted rendering input graph for downstream consumers.
	 * This is not the collector's candidate publication. Implementations retain the
	 * adapted storage and any borrowed source owners for this publication's lifetime.
	 *
	 * Calls are serialized with source invalidation on the collector's calling thread;
	 * this contract introduces no concurrent service or lifecycle synchronization.
	 * Retained previous/current owners permit future producer-private matching, but
	 * this interface supplies no contributor IDs, geometry revisions or reuse logic.
	 */
	class IBuildingSourcePublication
	{
	public:
		/** Releases publication-owned CPU storage when its final shared owner is released.
		 * Destruction must not depend on the collector still existing or perform native cleanup.
		 */
		virtual ~IBuildingSourcePublication() = default;

		/** Reports current eligibility independently of retained memory lifetime.
		 * The producer must revoke eligibility on world/session end, destruction or
		 * supersession that invalidates this source; false must never become true again.
		 * Keep the needed validity state owned independently of the collector, with
		 * no dangling collector pointer or native-object probing. Retention keeps all
		 * source bytes readable after revocation, but does not authorize current use.
		 * Callers check before use and revalidate at later certification/commit boundaries,
		 * serialized with invalidation; a prior true result is not a lifetime lease.
		 * @return Whether this fixed publication remains eligible as current source.
		 */
		virtual bool IsCurrent() const noexcept = 0;

		/** Returns descriptors borrowing this publication's immutable source and nested storage.
		 * All borrowed memory remains valid until the publication's final owner is released,
		 * including after another view/observation call or currentness revocation. Readiness
		 * and coverage remain facts of this publication, not live acquisition progress.
		 * Known sprite keys may be enumerated while source readiness is incomplete; empty
		 * spans in incomplete adapted data never establish that a section has no contributors.
		 * This operation acquires no assets/pixels, prepares no geometry and copies no arrays.
		 * @return Neutral borrowed source view; caller retains the publication during use.
		 */
		virtual BuildingSourceView BorrowSourceView() const noexcept = 0;

	};
}
