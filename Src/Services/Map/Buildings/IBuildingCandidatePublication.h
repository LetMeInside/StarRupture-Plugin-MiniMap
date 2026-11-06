// Retains one immutable candidate source and observes batches against that same owner.
#pragma once

#include "BuildingCandidateData.h"

#include <span>

namespace Services::Map::Buildings
{
	/** Owns candidate values and their borrowed storage independently of collector lifetime. */
	class IBuildingCandidatePublication
	{
	public:
		/** Releases retained CPU candidate storage without native cleanup. */
		virtual ~IBuildingCandidatePublication() = default;

		/** Reports independently revocable eligibility of this fixed retained source. */
		virtual bool IsCurrent() const noexcept = 0;

		/** Borrows candidate facts; the caller retains this publication while using the data. */
		virtual BuildingCandidateData BorrowCandidateData() const noexcept = 0;

		/** Fills exactly one fully initialized output per row-major cell and preserves each exact requested cell.
		 * Output index is row * columnCount + column. Cells are half-open, upper-left-origin,
		 * with X rightward and Y downward. Invalid or unsupported grids yield unknown evidence.
		 * Unknown or unlocalized occurrences must remain conservatively relevant to each cell
		 * until spatial exclusion is proven. Signatures are deterministic and independent of
		 * enumeration order. Each output ordinal span borrows publication-owned occurrence
		 * storage and remains valid while this publication remains retained. Candidate membership
		 * evidence never certifies final Section membership or rendering revisions. An incomplete
		 * membership scan cannot certify that previously known occurrences have disappeared.
		 */
		virtual void ObserveCandidates(
			const BuildingSectionGrid& grid,
			std::span<BuildingCandidateObservation> observations) const = 0;
	};
}
