// Defines game-independent acquisition regions, observation grids and results,
// plus the collector operations currently required by BuildingMapService.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace Services::Map::Buildings
{
	/** Describes a unit-agnostic 2D map rectangle with an upper-left origin and downward-positive Y. */
	struct BuildingCollectionRegion
	{
		/** Upper-left map X coordinate in the owning service's map units. */
		double xMapUnits;
		/** Upper-left map Y coordinate in the owning service's map units. */
		double yMapUnits;
		/** Positive width in the owning service's map units. */
		double widthMapUnits;
		/** Positive height in the owning service's map units. */
		double heightMapUnits;
	};

	/** Describes a regular grid of square map-space sections in row-major order. */
	struct BuildingSectionGrid
	{
		/** Upper-left map X coordinate of the top-left section. */
		double xMapUnits;
		/** Upper-left map Y coordinate of the top-left section. */
		double yMapUnits;
		/** Positive width and height of each square section in map units. */
		double sectionSizeMapUnits;
		/** Number of sections in each rightward-increasing row. */
		std::size_t columnCount;
		/** Number of sections in each downward-increasing column. */
		std::size_t rowCount;
	};

	/** Reports coverage certification and the membership fingerprint for one section. */
	struct BuildingSectionObservation
	{
		/** True only when source acquisition and contributor coverage are complete and reliable. */
		bool isComplete = false;
		/** Deterministic source-membership fingerprint; meaningful only when isComplete is true. */
		std::uint64_t membershipFingerprint = 0;
	};

	/** Exposes region management and read-only observations from a caller-owned collector. */
	class IBuildingCollector
	{
	public:
		/** Supports destruction through the game-independent collector boundary. */
		virtual ~IBuildingCollector() = default;

		/**
		 * Sets the 2D map acquisition rectangle in the service's map units. Returns false without changing
		 * collector state when the requested region cannot be accepted.
		 *
		 * @param region Upper-left map position and positive dimensions in the owning service's map units.
		 */
		virtual bool SetAcquisitionRegion(const BuildingCollectionRegion& region) noexcept = 0;

		/**
		 * Reads the requested regular section grid without changing the collector's
		 * acquisition region, incremental state, or service grid. The output span has
		 * one result per cell, exactly in row-major order (row * columnCount + column).
		 * A complete result certifies all contributors whose visual support affects
		 * that cell, including contributors whose reference points lie outside it.
		 * Complete fingerprints are deterministic for unchanged membership and
		 * independent of enumeration order; incomplete fingerprints are ignored,
		 * and zero is a valid complete fingerprint. Each cell uses half-open bounds;
		 * columns increase rightward and rows increase downward.
		 *
		 * @param grid Top-left map position, square section size and grid dimensions.
		 * @param observations Caller-owned storage containing exactly columnCount * rowCount results.
		 */
		virtual void ObserveSections(
			const BuildingSectionGrid& grid,
			std::span<BuildingSectionObservation> observations) const = 0;
	};
}
