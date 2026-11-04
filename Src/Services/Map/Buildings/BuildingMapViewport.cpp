#if defined(MODLOADER_CLIENT_BUILD)

// Implements 2D map-rectangle movement and read-only access to service-owned
// sparse Building Map sections.
#include "BuildingMapViewport.h"

#include "Coordination/IBuildingMapService.h"
#include "BuildingMapSection.h"

#include <cmath>
#include <stdexcept>

namespace Services::Map::Buildings
{
	/** Registers the viewport's fixed-size rectangle and initial grid coverage. */
	BuildingMapViewport::BuildingMapViewport(
		Coordination::IBuildingMapService& service,
		IBuildingCollector& buildingCollector,
		std::uint64_t viewportHandle,
		double xMapUnits,
		double yMapUnits,
		double widthMapUnits,
		double heightMapUnits)
		: service_(&service)
		, buildingCollector_(&buildingCollector)
		, viewportHandle_(viewportHandle)
		, xMapUnits_(xMapUnits)
		, yMapUnits_(yMapUnits)
		, widthMapUnits_(widthMapUnits)
		, heightMapUnits_(heightMapUnits)
		, gridRange_(CalculateGridRange(
			xMapUnits,
			yMapUnits,
			widthMapUnits,
			heightMapUnits,
			service.GetSectionSizeMapUnits()))
	{
	}

	/** Closes the session so the service can release its registration. */
	BuildingMapViewport::~BuildingMapViewport()
	{
		Close();
	}

	/** Moves after the collector accepts any required grid-aligned region update. */
	bool BuildingMapViewport::Move(double xMapUnits, double yMapUnits)
	{
		if (service_ == nullptr || buildingCollector_ == nullptr)
		{
			return false;
		}
		const GridRange nextRange = CalculateGridRange(
			xMapUnits,
			yMapUnits,
			widthMapUnits_,
			heightMapUnits_,
			service_->GetSectionSizeMapUnits());
		if (nextRange.firstX != gridRange_.firstX || nextRange.firstY != gridRange_.firstY ||
			nextRange.endX != gridRange_.endX || nextRange.endY != gridRange_.endY)
		{
			if (!service_->UpdateViewportGridRange(
				viewportHandle_,
				nextRange.firstX,
				nextRange.firstY,
				nextRange.endX,
				nextRange.endY))
			{
				return false;
			}
		}
		xMapUnits_ = xMapUnits;
		yMapUnits_ = yMapUnits;
		gridRange_ = nextRange;
		return true;
	}

	/** Requests service-owned observations for the current visible grid range. */
	bool BuildingMapViewport::RefreshSections()
	{
		if (service_ == nullptr)
		{
			return false;
		}
		return service_->RefreshViewportSections(viewportHandle_);
	}

	/** Returns the current upper-left map X coordinate. */
	double BuildingMapViewport::GetXMapUnits() const
	{
		return xMapUnits_;
	}

	/** Returns the current upper-left map Y coordinate. */
	double BuildingMapViewport::GetYMapUnits() const
	{
		return yMapUnits_;
	}

	/** Returns the unchanged rectangle width. */
	double BuildingMapViewport::GetWidthMapUnits() const
	{
		return widthMapUnits_;
	}

	/** Returns the unchanged rectangle height. */
	double BuildingMapViewport::GetHeightMapUnits() const
	{
		return heightMapUnits_;
	}

	/** Computes the number of whole grid columns intersected by the rectangle. */
	std::size_t BuildingMapViewport::GetVisibleGridWidth() const
	{
		return static_cast<std::size_t>(gridRange_.endX - gridRange_.firstX);
	}

	/** Computes the number of whole grid rows intersected by the rectangle. */
	std::size_t BuildingMapViewport::GetVisibleGridHeight() const
	{
		return static_cast<std::size_t>(gridRange_.endY - gridRange_.firstY);
	}

	/** Converts valid local offsets to signed world-grid coordinates. */
	bool BuildingMapViewport::GetWorldGridCoordinate(
		std::size_t localX,
		std::size_t localY,
		std::int64_t& gridX,
		std::int64_t& gridY) const
	{
		if (localX >= GetVisibleGridWidth() || localY >= GetVisibleGridHeight())
		{
			return false;
		}
		gridX = gridRange_.firstX + static_cast<std::int64_t>(localX);
		gridY = gridRange_.firstY + static_cast<std::int64_t>(localY);
		return true;
	}

	/** Looks up an existing section without creating or owning one. */
	const BuildingMapSection* BuildingMapViewport::GetSectionAt(
		std::size_t localX,
		std::size_t localY) const
	{
		std::int64_t gridX = 0;
		std::int64_t gridY = 0;
		if (service_ == nullptr || !GetWorldGridCoordinate(localX, localY, gridX, gridY))
		{
			return nullptr;
		}
		return service_->FindSectionAt(gridX, gridY);
	}

	/** Releases the registered handle once and clears the service relationship. */
	void BuildingMapViewport::Close()
	{
		if (service_ == nullptr)
		{
			return;
		}
		Coordination::IBuildingMapService* service = service_;
		service_ = nullptr;
		buildingCollector_ = nullptr;
		service->CloseViewport(viewportHandle_);
	}

	/** Clears the non-owning pointer before its service is destroyed. */
	void BuildingMapViewport::InvalidateService()
	{
		service_ = nullptr;
		buildingCollector_ = nullptr;
	}

	/** Validates map-unit bounds and maps the half-open rectangle to grid columns and rows. */
	BuildingMapViewport::GridRange BuildingMapViewport::CalculateGridRange(
		double xMapUnits,
		double yMapUnits,
		double widthMapUnits,
		double heightMapUnits,
		double sectionSizeMapUnits)
	{
		if (!std::isfinite(xMapUnits) || !std::isfinite(yMapUnits) ||
			!std::isfinite(widthMapUnits) || !std::isfinite(heightMapUnits) ||
			widthMapUnits <= 0.0 || heightMapUnits <= 0.0)
		{
			throw std::invalid_argument("Viewport coordinates and positive dimensions must be finite map units.");
		}
		const double rightMapUnits = xMapUnits + widthMapUnits;
		const double bottomMapUnits = yMapUnits + heightMapUnits;
		const long double cellSize = sectionSizeMapUnits;
		const long double firstX = std::floor(static_cast<long double>(xMapUnits) / cellSize);
		const long double firstY = std::floor(static_cast<long double>(yMapUnits) / cellSize);
		const long double endX = std::ceil(static_cast<long double>(rightMapUnits) / cellSize);
		const long double endY = std::ceil(static_cast<long double>(bottomMapUnits) / cellSize);
		const long double signedCoordinateLimit = std::ldexp(1.0L, 63);
		if (!std::isfinite(rightMapUnits) || !std::isfinite(bottomMapUnits) ||
			firstX < -signedCoordinateLimit || firstY < -signedCoordinateLimit ||
			endX >= signedCoordinateLimit || endY >= signedCoordinateLimit ||
			endX - firstX >= signedCoordinateLimit || endY - firstY >= signedCoordinateLimit)
		{
			throw std::overflow_error("Viewport grid coordinates exceed the supported integer range.");
		}
		return GridRange{
			static_cast<std::int64_t>(firstX),
			static_cast<std::int64_t>(firstY),
			static_cast<std::int64_t>(endX),
			static_cast<std::int64_t>(endY)};
	}
}

#endif // MODLOADER_CLIENT_BUILD
