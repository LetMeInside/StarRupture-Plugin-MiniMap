// Defines game-independent collector updates, exact region requests and retained candidate access.
#pragma once

#include "BuildingMapRegion.h"
#include "BuildingCandidatePublication.h"

#include <memory>

namespace Services::Map::Buildings
{
	/** Advances ordinary acquisition progress and exposes exact requests and retained candidates. */
	class IBuildingCollector
	{
	public:
		/** Supports destruction through the game-independent collector boundary. */
		virtual ~IBuildingCollector() = default;
		/** Requests ordinary acquisition progress without supplying game-specific context.
		 * The concrete collector chooses its acquisition frequency and internal policy.
		 * This operation neither establishes nor terminates gameplay-session ownership;
		 * native resource initialization and shutdown remain the concrete owner's responsibility.
		 */
		virtual void Update() = 0;

		/** Requests acquisition for one exact map-space rectangle.
		 * The collector is responsible for discovering contributors whose geometry overlaps the
		 * requested area, including contributors whose reference positions lie outside it, and for
		 * conservatively accounting for them in candidate observations. Discovery strategy and
		 * expansion policy belong to the concrete collector. Returns false without changing collector state when
		 * the requested region cannot be accepted.
		 * @param region Exact requested map-space rectangle in the owning service's map units.
		 */
		virtual bool SetAcquisitionRegion(const BuildingCollectionRegion& region) noexcept = 0;

		/** Retains one candidate source; null means no current candidate source is available. */
		virtual std::shared_ptr<const BuildingCandidatePublication> AcquireCandidatePublication() const = 0;
	};
}
