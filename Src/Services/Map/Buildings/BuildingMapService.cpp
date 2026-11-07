#if defined(MODLOADER_CLIENT_BUILD)

// Implements service-owned sections, viewport access, and SectionScheduler lifecycle delegation.
#include "BuildingMapService.h"

#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

#include "BuildingMapSection.h"
#include "BuildingMapViewport.h"
#include "Scheduling/SectionScheduler.h"

namespace Services::Map::Buildings
{
	/** Validates immutable map settings and copies the caller's spline policy span. */
	BuildingMapService::BuildingMapService(
		double sectionSizeMapUnits,
		std::uint32_t sectionResolutionPixels,
		std::filesystem::path buildingsCacheDirectory,
		std::span<const SplinePolicy> splinePolicies)
		: sectionSizeMapUnits_(sectionSizeMapUnits)
		, sectionResolutionPixels_(sectionResolutionPixels)
		, splinePolicies_(splinePolicies.begin(), splinePolicies.end())
	{
		if (!std::isfinite(sectionSizeMapUnits_) || sectionSizeMapUnits_ <= 0.0 ||
			sectionResolutionPixels_ == 0)
		{
			throw std::invalid_argument("Building Map settings require a finite positive section size and positive resolution.");
		}
		if (buildingsCacheDirectory.empty() || !buildingsCacheDirectory.is_absolute())
		{
			throw std::invalid_argument("Building Map requires an absolute Buildings cache directory.");
		}
		sectionScheduler_.reset(new Scheduling::SectionScheduler(
			sectionSizeMapUnits_,
			sectionResolutionPixels_,
			std::move(buildingsCacheDirectory)));
	}

	/** Invalidates all outstanding viewports before releasing service-owned sections. */
	BuildingMapService::~BuildingMapService()
	{
		for (const auto& viewportEntry : viewportsByHandle_)
		{
			viewportEntry.second->InvalidateService();
		}
		viewportsByHandle_.clear();
	}

	/** Starts one renderer session through SectionScheduler without activating runtime callbacks. */
	bool BuildingMapService::BeginCaptureGameWorldSession(std::uint64_t sessionGeneration)
	{
		if (!sectionScheduler_ || sessionGeneration == 0 ||
			sessionGeneration <= lastCaptureSessionGeneration_)
		{
			return false;
		}
		if (!sectionScheduler_->BeginCaptureGameWorldSession(sessionGeneration))
		{
			return false;
		}
		lastCaptureSessionGeneration_ = sessionGeneration;
		return true;
	}

	/** Advances SectionScheduler-owned renderer teardown without releasing pending native owners. */
	bool BuildingMapService::AdvanceCaptureGameWorldSessionShutdown()
	{
		return sectionScheduler_ == nullptr || sectionScheduler_->AdvanceCaptureGameWorldSessionShutdown();
	}

	/** Returns the configured square cell size in map units. */
	double BuildingMapService::GetSectionSizeMapUnits() const noexcept
	{
		return sectionSizeMapUnits_;
	}

	/** Returns the configured square image dimension in pixels. */
	std::uint32_t BuildingMapService::GetSectionResolutionPixels() const noexcept
	{
		return sectionResolutionPixels_;
	}

	/** Resolves direct indexes without selecting a fallback for reserved or missing entries. */
	const SplinePolicy* BuildingMapService::TryGetSplinePolicy(
		std::uint16_t splinePolicyIndex) const noexcept
	{
		if (splinePolicyIndex == 0 || splinePolicyIndex >= splinePolicies_.size())
		{
			return nullptr;
		}
		return &splinePolicies_[splinePolicyIndex];
	}

	/** Derives image scale from the immutable resolution and cell-size settings. */
	double BuildingMapService::GetPixelsPerMapUnit() const noexcept
	{
		return static_cast<double>(sectionResolutionPixels_) / sectionSizeMapUnits_;
	}

	/** Validates the requested rectangle and registers a caller-owned viewport object. */
	std::unique_ptr<BuildingMapViewport> BuildingMapService::OpenViewport(
		IBuildingCollector* buildingCollector,
		double xMapUnits,
		double yMapUnits,
		double widthMapUnits,
		double heightMapUnits)
	{
		if (buildingCollector == nullptr)
		{
			throw std::invalid_argument("A viewport requires a caller-owned building collector.");
		}
		for (const auto& viewportEntry : viewportsByHandle_)
		{
			if (viewportEntry.second->buildingCollector_ == buildingCollector)
			{
				throw std::invalid_argument("Each open viewport requires its own building collector.");
			}
		}
		if (nextViewportHandle_ == 0)
		{
			throw std::overflow_error("Viewport handle space is exhausted.");
		}
		const std::uint64_t viewportHandle = nextViewportHandle_++;
		std::unique_ptr<BuildingMapViewport> viewport(new BuildingMapViewport(
			static_cast<Coordination::IBuildingMapService&>(*this),
			*buildingCollector,
			viewportHandle,
			xMapUnits,
			yMapUnits,
			widthMapUnits,
			heightMapUnits));
		viewportsByHandle_.emplace(viewportHandle, viewport.get());
		if (!UpdateViewportGridRange(
			viewportHandle,
			viewport->gridRange_.firstX,
			viewport->gridRange_.firstY,
			viewport->gridRange_.endX,
			viewport->gridRange_.endY))
		{
			viewport->Close();
			return nullptr;
		}
		return viewport;
	}

	/** Looks up a section without inserting absent coordinates into the sparse grid. */
	const BuildingMapSection* BuildingMapService::FindSectionAt(
		std::int64_t gridX,
		std::int64_t gridY) const
	{
		const auto section = sections_.find(std::make_pair(gridX, gridY));
		return section == sections_.end() ? nullptr : section->second.get();
	}

	/** Removes session registration while preserving all shared section objects. */
	void BuildingMapService::CloseViewport(std::uint64_t viewportHandle)
	{
		const auto viewport = viewportsByHandle_.find(viewportHandle);
		if (viewport == viewportsByHandle_.end())
		{
			return;
		}
		viewportsByHandle_.erase(viewport);
	}

	/** Requests the exact tile-aligned rectangle covered by the supplied half-open grid range. */
	bool BuildingMapService::UpdateViewportGridRange(
		std::uint64_t viewportHandle,
		std::int64_t firstGridX,
		std::int64_t firstGridY,
		std::int64_t endGridX,
		std::int64_t endGridY)
	{
		const auto viewportEntry = viewportsByHandle_.find(viewportHandle);
		if (viewportEntry == viewportsByHandle_.end() || viewportEntry->second->buildingCollector_ == nullptr)
		{
			return false;
		}
		const double sectionSize = sectionSizeMapUnits_;
		const double left = static_cast<double>(firstGridX) * sectionSize;
		const double top = static_cast<double>(firstGridY) * sectionSize;
		const double right = static_cast<double>(endGridX) * sectionSize;
		const double bottom = static_cast<double>(endGridY) * sectionSize;
		const double width = right - left;
		const double height = bottom - top;
		if (!std::isfinite(left) || !std::isfinite(top) ||
			!std::isfinite(right) || !std::isfinite(bottom) ||
			!std::isfinite(width) || !std::isfinite(height) ||
			width <= 0.0 || height <= 0.0 ||
			left + width != right || top + height != bottom)
		{
			return false;
		}
		const BuildingCollectionRegion collectionRegion{
			left,
			top,
			width,
			height};
		return viewportEntry->second->buildingCollector_->SetAcquisitionRegion(collectionRegion);
	}

	/** Observes candidate evidence for the requested grid without updating Section state. */
	bool BuildingMapService::RefreshViewportSections(std::uint64_t viewportHandle)
	{
		const auto viewportEntry = viewportsByHandle_.find(viewportHandle);
		if (viewportEntry == viewportsByHandle_.end() || viewportEntry->second->buildingCollector_ == nullptr)
		{
			return false;
		}
		const BuildingMapViewport::GridRange gridRange = viewportEntry->second->gridRange_;
		const double sectionSize = sectionSizeMapUnits_;
		const std::size_t columnCount = static_cast<std::size_t>(gridRange.endX - gridRange.firstX);
		const std::size_t rowCount = static_cast<std::size_t>(gridRange.endY - gridRange.firstY);
		const BuildingSectionGrid observationGrid{
			static_cast<double>(gridRange.firstX) * sectionSize,
			static_cast<double>(gridRange.firstY) * sectionSize,
			sectionSize,
			columnCount,
			rowCount};
		IBuildingCollector& buildingCollector = *viewportEntry->second->buildingCollector_;
		ObserveCandidateBatch(
			buildingCollector,
			observationGrid,
			columnCount,
			rowCount);
		return false;
	}

	/** Observes one retained candidate publication and never promotes its evidence to Section state. */
	void BuildingMapService::ObserveCandidateBatch(
		IBuildingCollector& buildingCollector,
		const BuildingSectionGrid& observationGrid,
		std::size_t columnCount,
		std::size_t rowCount)
	{
		std::vector<BuildingCandidateObservation> observations;
		if (rowCount != 0 && columnCount > observations.max_size() / rowCount)
		{
			throw std::length_error("Viewport candidate observation grid exceeds supported storage size.");
		}
		observations.resize(columnCount * rowCount);
		const std::shared_ptr<const BuildingCandidatePublication> publication =
			buildingCollector.AcquireCandidatePublication();
		if (publication == nullptr || !publication->IsCurrent())
		{
			return;
		}
		publication->ObserveCandidates(observationGrid, observations);
		if (!publication->IsCurrent())
		{
			return;
		}
	}

	/** Returns the existing section or creates one owned by the sparse service grid. */
	BuildingMapSection& BuildingMapService::GetOrCreateSectionAt(
		std::int64_t gridX,
		std::int64_t gridY)
	{
		const auto coordinate = std::make_pair(gridX, gridY);
		auto section = sections_.find(coordinate);
		if (section == sections_.end())
		{
			section = sections_.emplace(
				coordinate,
				std::unique_ptr<BuildingMapSection>(new BuildingMapSection(gridX, gridY))).first;
		}
		return *section->second;
	}

}

#endif // MODLOADER_CLIENT_BUILD
