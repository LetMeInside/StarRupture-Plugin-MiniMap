#if defined(MODLOADER_CLIENT_BUILD)

// Implements immutable map settings, sparse section ownership, viewport
// registration and invalidation of non-owning viewport relationships.
#include "BuildingMapService.h"

#include "BuildingMapSection.h"
#include "BuildingMapViewport.h"
#include "Capture/BuildingSpriteRenderer.h"

#include <cmath>
#include <stdexcept>
#include <vector>

namespace Services::Map::Buildings
{
	/** Validates and stores immutable map-grid and image-resolution settings. */
	BuildingMapService::BuildingMapService(
		double sectionSizeMapUnits,
		std::uint32_t sectionResolutionPixels,
		double collectionMarginMapUnits)
		: sectionSizeMapUnits_(sectionSizeMapUnits)
		, sectionResolutionPixels_(sectionResolutionPixels)
		, collectionMarginMapUnits_(collectionMarginMapUnits)
	{
		if (!std::isfinite(sectionSizeMapUnits_) || sectionSizeMapUnits_ <= 0.0 ||
			sectionResolutionPixels_ == 0 || !std::isfinite(collectionMarginMapUnits_) ||
			collectionMarginMapUnits_ < 0.0)
		{
			throw std::invalid_argument("Building Map settings require a finite positive section size, positive resolution, and finite nonnegative margin.");
		}
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

	/** Drives private renderer teardown without releasing its owner while NativeApi work is pending. */
	bool BuildingMapService::AdvanceCaptureRendererShutdown()
	{
		if (!captureRenderer_)
		{
			return true;
		}
		captureRenderer_->RequestServiceShutdown();
		if (!captureRenderer_->AdvanceServiceShutdown())
		{
			return false;
		}
		captureRenderer_.reset();
		return true;
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

	/** Returns the configured acquisition margin in map units. */
	double BuildingMapService::GetCollectionMarginMapUnits() const noexcept
	{
		return collectionMarginMapUnits_;
	}

	/** Derives image scale from the immutable resolution and cell-size settings. */
	double BuildingMapService::GetPixelsPerMapUnit() const noexcept
	{
		return static_cast<double>(sectionResolutionPixels_) / sectionSizeMapUnits_;
	}

	/** Exposes the service-owned CPU source manager without initializing it. */
	Cache::CpuSpriteSourceManager& BuildingMapService::GetCpuSpriteSourceManager() noexcept
	{
		return cpuSpriteSourceManager_;
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

	/** Updates the collector to full map cells plus the support margin in every direction. */
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
		const double supportMargin = collectionMarginMapUnits_;
		const BuildingCollectionRegion collectionRegion{
			static_cast<double>(firstGridX) * sectionSize - supportMargin,
			static_cast<double>(firstGridY) * sectionSize - supportMargin,
			static_cast<double>(endGridX - firstGridX) * sectionSize + supportMargin * 2.0,
			static_cast<double>(endGridY - firstGridY) * sectionSize + supportMargin * 2.0};
		return viewportEntry->second->buildingCollector_->SetAcquisitionRegion(collectionRegion);
	}

	/** Batch-observes the requested cells and updates their shared section state. */
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
		std::vector<BuildingSectionObservation> observations;
		if (rowCount != 0 && columnCount > observations.max_size() / rowCount)
		{
			throw std::length_error("Viewport section observation grid exceeds supported storage size.");
		}
		observations.resize(columnCount * rowCount);
		const BuildingSectionGrid observationGrid{
			static_cast<double>(gridRange.firstX) * sectionSize,
			static_cast<double>(gridRange.firstY) * sectionSize,
			sectionSize,
			columnCount,
			rowCount};
		viewportEntry->second->buildingCollector_->ObserveSections(observationGrid, observations);
		bool anyMembershipFingerprintChanged = false;
		std::size_t observationIndex = 0;
		for (std::int64_t gridY = gridRange.firstY; gridY < gridRange.endY; ++gridY)
		{
			for (std::int64_t gridX = gridRange.firstX; gridX < gridRange.endX; ++gridX)
			{
				if (ApplySectionObservation(gridX, gridY, observations[observationIndex]))
				{
					anyMembershipFingerprintChanged = true;
				}
				++observationIndex;
			}
		}
		return anyMembershipFingerprintChanged;
	}

	/** Applies one observation without creating a section for incomplete coverage. */
	bool BuildingMapService::ApplySectionObservation(
		std::int64_t gridX,
		std::int64_t gridY,
		const BuildingSectionObservation& observation)
	{
		if (!observation.isComplete)
		{
			const auto section = sections_.find(std::make_pair(gridX, gridY));
			if (section != sections_.end())
			{
				section->second->RecordMembershipObservation(false, observation.membershipFingerprint);
			}
			return false;
		}
		BuildingMapSection& section = GetOrCreateSectionAt(gridX, gridY);
		return section.RecordMembershipObservation(true, observation.membershipFingerprint);
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
