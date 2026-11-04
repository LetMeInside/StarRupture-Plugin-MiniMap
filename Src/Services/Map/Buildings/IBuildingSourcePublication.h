// Defines one retained immutable source-publication boundary so observations and
// borrowed numerical inputs cannot silently refer to different acquisitions.
#pragma once

#include "BuildingSourceView.h"

#include <span>

namespace Services::Map::Buildings
{
	/** Owns or shares one immutable source publication and its complete borrowed-storage graph.
	 * Acquire through IBuildingCollector::AcquireSourcePublication and retain its shared
	 * owner while observing/borrowing. Implementations must pin existing immutable source
	 * and adaptation owners, not expose temporary scratch or copy an authoritative database.
	 * Every method describes this fixed publication, never the collector's latest snapshot.
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
		 * Known sprite keys may be enumerated while readiness/observations are incomplete;
		 * empty spans in incomplete data never establish that a section has no contributors.
		 * This operation acquires no assets/pixels, prepares no geometry and copies no arrays.
		 * @return Neutral borrowed source view; caller retains the publication during use.
		 */
		virtual BuildingSourceView BorrowSourceView() const noexcept = 0;

		/** Observes sections from exactly the immutable source used by BorrowSourceView.
		 * Preserve IBuildingCollector::ObserveSections batch semantics: caller supplies
		 * exactly columnCount * rowCount slots in row-major order, with half-open cells,
		 * rightward columns and downward rows. Unsupported/invalid grids, unknown coverage
		 * or uncertified support yield incomplete results, never inferred empty membership.
		 * Fingerprint zero remains valid when complete; incomplete fingerprints are ignored.
		 * A fingerprint is neither a publication identity nor numerical rendering evidence.
		 *
		 * This method must not fetch a newer collector publication or mutate acquisition.
		 * Results certify this publication's source only: even a complete historical result
		 * requires IsCurrent before current use. Keep the owner alongside the grid/results
		 * in any later transaction; do not pair them with a newly acquired source owner.
		 * @param grid Top-left map position, square section size and grid dimensions.
		 * @param observations Caller-owned output, completely filled for this publication.
		 */
		virtual void ObserveSections(
			const BuildingSectionGrid& grid,
			std::span<BuildingSectionObservation> observations) const = 0;
	};
}
