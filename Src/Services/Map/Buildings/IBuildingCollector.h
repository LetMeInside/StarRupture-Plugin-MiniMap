// Defines game-independent collector updates, exact region requests and retained candidate access.
#pragma once

#include "BuildingMapRegion.h"
#include "BuildingCandidatePublication.h"

#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace Services::Map::Buildings
{
	/** Reports that a collector lacks a successful committed acquisition for a Section request. */
	class SectionPublicationUnavailable final : public std::runtime_error
	{
	public:
		/** Preserves the collector's diagnostic while distinguishing unavailable data from bad requests.
		 * @param message Diagnostic describing why committed acquisition data cannot be supplied.
		 */
		explicit SectionPublicationUnavailable(const char* message)
			: std::runtime_error(message)
		{
		}
	};

	/** Advances acquisition and exposes its hash plus immutable exact Section publications. */
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
		 * preserving contributors that can affect requested Sections. Discovery strategy belongs
		 * to the concrete collector. Returns false without changing collector state when
		 * the requested region cannot be accepted.
		 * @param region Exact requested map-space rectangle in the owning service's map units.
		 */
		virtual bool SetAcquisitionRegion(const BuildingCollectionRegion& region) noexcept = 0;

		/** Returns the content hash of the committed regional dataset; zero is a valid hash.
		 * This read has no acquisition or progress side effect.
		 */
		virtual std::uint64_t AcquisitionHash() const noexcept = 0;

		/** Obtains one immutable Section publication per exact requested rectangle, in input order.
		 * An empty Section is a valid publication containing no contributors. A failed request,
		 * unavailable acquisition result or construction failure must be reported as failure,
		 * never as a successful empty Section. A collector without a successful committed acquisition
		 * throws SectionPublicationUnavailable; invalid rectangles or incompatible requests throw
		 * std::invalid_argument, and construction failures propagate normally. A successful request
		 * returns exactly one non-null publication per rectangle. This operation uses the committed
		 * acquired dataset and performs no native acquisition. The input span is borrowed for this call.
		 * @param sectionRegions Exact Section rectangles in the owning service's map units.
		 * @return Owned immutable publications corresponding one-for-one with the input batch.
		 * @throws SectionPublicationUnavailable when successful committed acquisition is unavailable.
		 * @throws std::invalid_argument when the requested rectangles are invalid or incompatible.
		 */
		virtual std::vector<std::shared_ptr<const BuildingCandidatePublication>>
		AcquireSectionPublications(
			std::span<const BuildingCollectionRegion> sectionRegions) const = 0;
	};
}
