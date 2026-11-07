// Represents one caller-owned viewport over service-owned Building Map cells and owns its optional image-change handler.
// Its unit-agnostic 2D rectangle uses an upper-left origin, positive X rightward,
// positive Y downward, and half-open bounds [x, right) by [y, bottom).
#pragma once

#include "Coordination/IBuildingMapService.h"
#include "IBuildingCollector.h"

#include <cstddef>
#include <cstdint>
#include <functional>

namespace Services::Map::Buildings
{
	class BuildingMapSection;
	class BuildingMapService;
	namespace Coordination
	{
		class IBuildingMapService;
	}

	/** Tracks a 2D map rectangle, exposes nullable references to its Sections, and owns one optional synchronous image-change handler.
	 * Local (0, 0) is the top-left cell; columns increase rightward and rows
	 * increase downward with the corresponding grid coordinates. Its collector
	 * pointer is non-owning and remains valid only while the caller keeps that
	 * collector alive.
	 */
	class BuildingMapViewport
	{
	public:
		/** Receives the signed global grid coordinate of an adopted Section image. */
		using SectionImageChangedHandler = std::function<void(std::int64_t, std::int64_t)>;

		/** Prevents copies from duplicating one service-owned viewport registration. */
		BuildingMapViewport(const BuildingMapViewport&) = delete;
		/** Prevents copies from duplicating one service-owned viewport registration. */
		BuildingMapViewport& operator=(const BuildingMapViewport&) = delete;
		/** Prevents moves from relocating an object tracked by its service. */
		BuildingMapViewport(BuildingMapViewport&&) = delete;
		/** Prevents moves from relocating an object tracked by its service. */
		BuildingMapViewport& operator=(BuildingMapViewport&&) = delete;
		/** Closes this viewport before its object storage is released. */
		~BuildingMapViewport();

		/**
		 * Moves the upper-left corner in the service's map units without changing size.
		 * Returns false and preserves the old position and cell range if the service
		 * or collector cannot apply a changed grid-aligned collection region.
		 *
		 * @param xMapUnits New upper-left map X coordinate.
		 * @param yMapUnits New upper-left map Y coordinate.
		 */
		bool Move(double xMapUnits, double yMapUnits);

		/**
		 * Requests candidate observations for the current cells without changing the
		 * collector's acquisition region. Returns true only when final scheduler-certified
		 * membership is newly certified or changes; candidate signatures do not count.
		 */
		bool RefreshSections();

		/** Returns the current rectangle's upper-left X coordinate in map units. */
		double GetXMapUnits() const;

		/** Returns the current rectangle's upper-left Y coordinate in map units. */
		double GetYMapUnits() const;

		/** Returns the rectangle width in map units. */
		double GetWidthMapUnits() const;

		/** Returns the rectangle height in map units. */
		double GetHeightMapUnits() const;

		/** Returns the number of intersected grid columns. */
		std::size_t GetVisibleGridWidth() const;

		/** Returns the number of intersected grid rows. */
		std::size_t GetVisibleGridHeight() const;

		/** Maps a top-left-origin local column and row to its rightward/downward grid coordinate. */
		bool GetWorldGridCoordinate(
			std::size_t localX,
			std::size_t localY,
			std::int64_t& gridX,
			std::int64_t& gridY) const;

		/** Returns the shared section at a local cell, or null if absent or out of range. */
		const BuildingMapSection* GetSectionAt(std::size_t localX, std::size_t localY) const;

		/** Ends this viewport session; repeated calls have no effect. */
		void Close();

		/**
		 * Installs this viewport's sole synchronous same-module image-change handler;
		 * passing an empty handler removes it. The handler remains owned by this
		 * viewport only while it is associated with its service. It should do immediate
		 * consumer-local work, such as marking a Section dirty or recording its
		 * coordinate for later processing. During dispatch it must not destroy the
		 * service, change viewport registrations, or recursively advance scheduling.
		 * @param handler Replacement callback receiving signed global Section X and Y.
		 */
		void SetSectionImageChangedHandler(SectionImageChangedHandler handler);

	private:
		friend class BuildingMapService;

		/** Stores inclusive first and exclusive end column and row coordinates. */
		struct GridRange
		{
			std::int64_t firstX;
			std::int64_t firstY;
			std::int64_t endX;
			std::int64_t endY;
		};

		/** Creates a viewport after the service has registered its handle. */
		BuildingMapViewport(
			Coordination::IBuildingMapService& service,
			IBuildingCollector& buildingCollector,
			std::uint64_t viewportHandle,
			double xMapUnits,
			double yMapUnits,
			double widthMapUnits,
			double heightMapUnits);

		/** Validates bounds and converts them to a half-open grid range using the service cell size.
		 *
		 * @param xMapUnits Upper-left map X coordinate.
		 * @param yMapUnits Upper-left map Y coordinate.
		 * @param widthMapUnits Positive viewport width.
		 * @param heightMapUnits Positive viewport height.
		 * @param sectionSizeMapUnits Positive configured square cell size.
		 * @return Inclusive first and exclusive end grid coordinates.
		 */
		static GridRange CalculateGridRange(
			double xMapUnits,
			double yMapUnits,
			double widthMapUnits,
			double heightMapUnits,
			double sectionSizeMapUnits);

		/** Invalidates the non-owning service relationship during service destruction. */
		void InvalidateService();

		/** Delivers one coordinate-only event when a complete Section image is adopted. */
		void NotifySectionImageChanged(std::int64_t gridX, std::int64_t gridY);

		Coordination::IBuildingMapService* service_;
		IBuildingCollector* buildingCollector_;
		std::uint64_t viewportHandle_;
		double xMapUnits_;
		double yMapUnits_;
		double widthMapUnits_;
		double heightMapUnits_;
		GridRange gridRange_;
		SectionImageChangedHandler sectionImageChangedHandler_;
	};
}
