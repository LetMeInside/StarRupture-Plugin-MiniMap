#if defined(MODLOADER_CLIENT_BUILD)

// Implements service-owned Sections and image residency, viewport source selection, image-change routing, and scheduler lifecycle delegation.
#include "BuildingMapService.h"

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include "BuildingMapSection.h"
#include "BuildingMapViewport.h"
#include "Scheduling/SectionSchedulingCoordinator.h"

namespace Services::Map::Buildings
{
	/** Clears borrowed views before releasing the publication that owns their storage. */
	void BuildingMapService::SectionCandidateSource::Reset() noexcept
	{
		Observation = {};
		PublicationOwner.reset();
	}

	/** Validates map/cache settings, copies policies and derives the owned Buildings path. */
	BuildingMapService::BuildingMapService(
		double sectionSizeMapUnits,
		std::uint32_t sectionResolutionPixels,
		std::filesystem::path cacheRoot,
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
		if (cacheRoot.empty())
		{
			throw std::invalid_argument("Building Map requires an absolute MiniMap cache root.");
		}
		cacheRoot.make_preferred();
		cacheRoot = cacheRoot.lexically_normal();
		if (!cacheRoot.is_absolute())
		{
			throw std::invalid_argument("Building Map requires an absolute MiniMap cache root.");
		}
		std::filesystem::path buildingsCacheDirectory = cacheRoot / L"Buildings";
		sectionSchedulingCoordinator_.reset(new Scheduling::SectionSchedulingCoordinator(
			sectionSizeMapUnits_,
			sectionResolutionPixels_,
			std::move(buildingsCacheDirectory)));
	}

	/** Delegates explicit cache invalidation through the owned scheduler hierarchy. */
	bool BuildingMapService::ClearCache()
	{
		return sectionSchedulingCoordinator_ != nullptr &&
			sectionSchedulingCoordinator_->ClearCache();
	}

	/** Starts a gameplay session against the renderer already owned by the service. */
	bool BuildingMapService::StartGameSession(
		double mapWidthMapUnits,
		double mapHeightMapUnits)
	{
		if (!std::isfinite(mapWidthMapUnits) || !std::isfinite(mapHeightMapUnits) ||
			mapWidthMapUnits <= 0.0 || mapHeightMapUnits <= 0.0 ||
			mapWidthMapUnits_ != 0.0 || mapHeightMapUnits_ != 0.0)
		{
			return false;
		}
		if (!BeginCaptureGameWorldSession())
		{
			return false;
		}
		mapWidthMapUnits_ = mapWidthMapUnits;
		mapHeightMapUnits_ = mapHeightMapUnits;
		return true;
	}

	/** Stops one gameplay session and clears its viewport/demand/Section state without waiting. */
	void BuildingMapService::StopGameSession()
	{
		if (mapWidthMapUnits_ == 0.0 && mapHeightMapUnits_ == 0.0)
		{
			return;
		}
		StopCaptureGameWorldSession();
		for (const auto& viewportEntry : viewportsByHandle_)
		{
			viewportEntry.second->InvalidateService();
		}
		viewportsByHandle_.clear();
		if (sectionSchedulingCoordinator_ != nullptr)
		{
			sectionSchedulingCoordinator_->ClearViewportDemand();
		}
		sections_.clear();
		sectionImageResidencyReconciliationPending_ = false;
		mapWidthMapUnits_ = 0.0;
		mapHeightMapUnits_ = 0.0;
	}

	/** Returns the persistent renderer without processing its private lifecycle state. */
	Capture::BuildingSpriteRenderer* BuildingMapService::GetCapturerLifecycleTarget() noexcept
	{
		return sectionSchedulingCoordinator_ != nullptr
			? sectionSchedulingCoordinator_->GetCapturerLifecycleTarget()
			: nullptr;
	}

	/** Returns the active gameplay map width without modifying service state. */
	double BuildingMapService::GetMapWidthMapUnits() const noexcept
	{
		return mapWidthMapUnits_;
	}

	/** Returns the active gameplay map height without modifying service state. */
	double BuildingMapService::GetMapHeightMapUnits() const noexcept
	{
		return mapHeightMapUnits_;
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

	/** Starts gameplay scheduling against the coordinator's persistent renderer. */
	bool BuildingMapService::BeginCaptureGameWorldSession()
	{
		return sectionSchedulingCoordinator_ &&
			sectionSchedulingCoordinator_->BeginCaptureGameWorldSession();
	}

	/** Ends scheduler admission and requests active-request cancellation once. */
	void BuildingMapService::StopCaptureGameWorldSession()
	{
		if (sectionSchedulingCoordinator_ != nullptr)
		{
			sectionSchedulingCoordinator_->StopCaptureGameWorldSession();
		}
	}

	/** Observes one registered viewport and advances the shared coordinator once. */
	bool BuildingMapService::AdvanceViewportScheduling(std::uint64_t viewportHandle)
	{
		const auto viewportEntry = viewportsByHandle_.find(viewportHandle);
		if (viewportEntry == viewportsByHandle_.end())
		{
			return false;
		}
		BuildingMapViewport* viewport = viewportEntry->second;
		if (viewport == nullptr || viewport->service_ != this ||
			viewport->viewportHandle_ != viewportHandle || viewport->buildingCollector_ == nullptr)
		{
			return false;
		}
		if (sectionImageResidencyReconciliationPending_)
		{
			ReconcileSectionImageResidency();
			sectionImageResidencyReconciliationPending_ = false;
		}
		if (sectionSchedulingCoordinator_ != nullptr)
		{
			ObserveViewportCandidatePublicationChanges(*viewport);
			sectionSchedulingCoordinator_->AdvanceWaitingSections(*this);
		}
		return true;
	}

	/** Tests one coordinate against every valid viewport's overflow-safe expanded grid range. */
	bool BuildingMapService::IsSectionWithinAnyViewportResidencyRange(
		std::int64_t gridX,
		std::int64_t gridY) const
	{
		const std::int64_t minimumGridCoordinate = (std::numeric_limits<std::int64_t>::min)();
		for (const auto& viewportEntry : viewportsByHandle_)
		{
			const BuildingMapViewport* viewport = viewportEntry.second;
			if (viewport == nullptr || viewport->service_ != this || viewport->buildingCollector_ == nullptr ||
				viewport->viewportHandle_ != viewportEntry.first)
			{
				continue;
			}
			const BuildingMapViewport::GridRange& range = viewport->gridRange_;
			const std::int64_t firstXWithMargin = range.firstX == minimumGridCoordinate
				? range.firstX
				: range.firstX - 1;
			const std::int64_t firstYWithMargin = range.firstY == minimumGridCoordinate
				? range.firstY
				: range.firstY - 1;
			if (gridX >= firstXWithMargin && gridX <= range.endX &&
				gridY >= firstYWithMargin && gridY <= range.endY)
			{
				return true;
			}
		}
		return false;
	}

	/** Invalidates only completed image bytes for Sections outside the current viewport union. */
	void BuildingMapService::ReconcileSectionImageResidency()
	{
		for (const auto& sectionEntry : sections_)
		{
			BuildingMapSection* section = sectionEntry.second.get();
			if (section == nullptr || !section->HasCompletedImage())
			{
				continue;
			}
			if (!IsSectionWithinAnyViewportResidencyRange(sectionEntry.first.first, sectionEntry.first.second))
			{
				section->InvalidateCompletedImage();
			}
		}
	}

	/** Requeues this viewport's demanded cells when its current publication owner changes. */
	void BuildingMapService::ObserveViewportCandidatePublicationChanges(BuildingMapViewport& viewport)
	{
		std::shared_ptr<const BuildingCandidatePublication> publication =
			viewport.buildingCollector_->AcquireCandidatePublication();
		if (publication != nullptr && !publication->IsCurrent())
		{
			publication.reset();
		}
		const std::weak_ptr<const BuildingCandidatePublication>& previousPublication =
			viewport.observedCandidatePublication_;
		if (!previousPublication.owner_before(publication) &&
			!publication.owner_before(previousPublication))
		{
			return;
		}
		viewport.observedCandidatePublication_ = publication;
		const BuildingMapViewport::GridRange& gridRange = viewport.gridRange_;
		for (std::int64_t gridY = gridRange.firstY; gridY < gridRange.endY; ++gridY)
		{
			for (std::int64_t gridX = gridRange.firstX; gridX < gridRange.endX; ++gridX)
			{
				sectionSchedulingCoordinator_->RequestSectionWork(
					Scheduling::SectionSchedulingCoordinator::SectionCoordinate{gridX, gridY});
			}
		}
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
		if (mapWidthMapUnits_ == 0.0 || mapHeightMapUnits_ == 0.0)
		{
			return nullptr;
		}
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
			viewportsByHandle_.erase(viewportHandle);
			viewport->InvalidateService();
			return nullptr;
		}
		if (sectionSchedulingCoordinator_ == nullptr ||
			!sectionSchedulingCoordinator_->AddViewportDemand(
				viewport->gridRange_.firstX,
				viewport->gridRange_.firstY,
				viewport->gridRange_.endX,
				viewport->gridRange_.endY))
		{
			viewportsByHandle_.erase(viewportHandle);
			viewport->InvalidateService();
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
		if (sectionSchedulingCoordinator_ != nullptr)
		{
			const auto& gridRange = viewport->second->gridRange_;
			sectionSchedulingCoordinator_->WithdrawViewportDemand(
				gridRange.firstX,
				gridRange.firstY,
				gridRange.endX,
				gridRange.endY);
		}
		viewportsByHandle_.erase(viewport);
		ReconcileSectionImageResidency();
		sectionImageResidencyReconciliationPending_ = false;
	}

	/** Delivers one completed-image availability change to each live subscribed viewport in range. */
	void BuildingMapService::NotifySectionImageChangedToViewports(
		std::int64_t gridX,
		std::int64_t gridY)
	{
		for (const auto& viewportEntry : viewportsByHandle_)
		{
			BuildingMapViewport* viewport = viewportEntry.second;
			if (viewport == nullptr || viewport->service_ != this || viewport->buildingCollector_ == nullptr ||
				viewport->viewportHandle_ != viewportEntry.first ||
				!viewport->sectionImageChangedHandler_ ||
				gridX < viewport->gridRange_.firstX || gridX >= viewport->gridRange_.endX ||
				gridY < viewport->gridRange_.firstY || gridY >= viewport->gridRange_.endY)
			{
				continue;
			}
			viewport->NotifySectionImageChanged(gridX, gridY);
		}
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
		if (!viewportEntry->second->buildingCollector_->SetAcquisitionRegion(collectionRegion))
		{
			return false;
		}
		const auto& previousRange = viewportEntry->second->gridRange_;
		if (previousRange.firstX != firstGridX || previousRange.firstY != firstGridY ||
			previousRange.endX != endGridX || previousRange.endY != endGridY)
		{
			if (sectionSchedulingCoordinator_ == nullptr ||
				!sectionSchedulingCoordinator_->ChangeViewportDemand(
					previousRange.firstX,
					previousRange.firstY,
					previousRange.endX,
					previousRange.endY,
					firstGridX,
					firstGridY,
					endGridX,
					endGridY))
			{
				return false;
			}
			sectionImageResidencyReconciliationPending_ = true;
		}
		return true;
	}

	/** Chooses the best current publication among live viewports covering one Section cell. */
	bool BuildingMapService::TrySelectCandidatePublicationForSection(
		std::int64_t gridX,
		std::int64_t gridY,
		SectionCandidateSource& output) const
	{
		output.Reset();
		const double xMapUnits = static_cast<double>(gridX) * sectionSizeMapUnits_;
		const double yMapUnits = static_cast<double>(gridY) * sectionSizeMapUnits_;
		const double rightMapUnits = xMapUnits + sectionSizeMapUnits_;
		const double bottomMapUnits = yMapUnits + sectionSizeMapUnits_;
		if (!std::isfinite(xMapUnits) || !std::isfinite(yMapUnits) ||
			!std::isfinite(rightMapUnits) || !std::isfinite(bottomMapUnits) ||
			rightMapUnits <= xMapUnits || bottomMapUnits <= yMapUnits)
		{
			return false;
		}
		const BuildingCollectionRegion sectionRegion{
			xMapUnits,
			yMapUnits,
			sectionSizeMapUnits_,
			sectionSizeMapUnits_};
		const BuildingSectionGrid observationGrid{xMapUnits, yMapUnits, sectionSizeMapUnits_, 1, 1};
		SectionCandidateSource selectedCandidate;
		int selectedEvidenceRank = -1;
		for (const auto& viewportEntry : viewportsByHandle_)
		{
			ConsiderViewportCandidateForSection(
				viewportEntry.first,
				viewportEntry.second,
				gridX,
				gridY,
				sectionRegion,
				observationGrid,
				selectedCandidate,
				selectedEvidenceRank);
		}
		if (selectedCandidate.PublicationOwner == nullptr || !selectedCandidate.PublicationOwner->IsCurrent())
		{
			return false;
		}
		output = std::move(selectedCandidate);
		return true;
	}

	/** Inspects one current viewport publication and retains it only when it improves evidence quality. */
	void BuildingMapService::ConsiderViewportCandidateForSection(
		std::uint64_t viewportHandle,
		BuildingMapViewport* viewport,
		std::int64_t gridX,
		std::int64_t gridY,
		const BuildingCollectionRegion& sectionRegion,
		const BuildingSectionGrid& observationGrid,
		SectionCandidateSource& selectedCandidate,
		int& selectedEvidenceRank) const
	{
		if (viewport == nullptr || viewport->service_ != this || viewport->buildingCollector_ == nullptr ||
			viewport->viewportHandle_ != viewportHandle || gridX < viewport->gridRange_.firstX ||
			gridX >= viewport->gridRange_.endX || gridY < viewport->gridRange_.firstY ||
			gridY >= viewport->gridRange_.endY)
		{
			return;
		}
		std::shared_ptr<const BuildingCandidatePublication> publication =
			viewport->buildingCollector_->AcquireCandidatePublication();
		if (publication == nullptr || !publication->IsCurrent())
		{
			return;
		}
		const BuildingCandidateData candidateData = publication->BorrowCandidateData();
		if (!HasRequestedCoverageForSection(candidateData, sectionRegion))
		{
			return;
		}
		std::array<BuildingCandidateObservation, 1> observations{};
		publication->ObserveCandidates(observationGrid, observations);
		if (!publication->IsCurrent())
		{
			return;
		}
		const BuildingCandidateObservation& observation = observations.front();
		if (!IsObservationForSection(observation, sectionRegion))
		{
			return;
		}
		const BuildingCandidateEvidence& evidence = observation.evidence;
		const int evidenceRank = evidence.membershipEnumerationComplete && evidence.candidateCoverageComplete
			? (evidence.nativeAcquisitionComplete && !evidence.hasPendingOrUnresolvedOccurrences ? 2 : 1)
			: 0;
		if (evidenceRank <= selectedEvidenceRank)
		{
			return;
		}
		selectedCandidate.Observation = observation;
		selectedCandidate.PublicationOwner = std::move(publication);
		selectedEvidenceRank = evidenceRank;
	}

	/** Requires valid accepted request bounds to contain the entire target cell. */
	bool BuildingMapService::HasRequestedCoverageForSection(
		const BuildingCandidateData& candidateData,
		const BuildingCollectionRegion& sectionRegion) noexcept
	{
		if (!candidateData.hasRequestedCoverage)
		{
			return false;
		}
		const BuildingCollectionRegion& requested = candidateData.requestedCoverage;
		const double requestedRight = requested.xMapUnits + requested.widthMapUnits;
		const double requestedBottom = requested.yMapUnits + requested.heightMapUnits;
		const double sectionRight = sectionRegion.xMapUnits + sectionRegion.widthMapUnits;
		const double sectionBottom = sectionRegion.yMapUnits + sectionRegion.heightMapUnits;
		return std::isfinite(requested.xMapUnits) && std::isfinite(requested.yMapUnits) &&
			std::isfinite(requested.widthMapUnits) && std::isfinite(requested.heightMapUnits) &&
			requested.widthMapUnits > 0.0 && requested.heightMapUnits > 0.0 &&
			std::isfinite(requestedRight) && std::isfinite(requestedBottom) &&
			requestedRight > requested.xMapUnits && requestedBottom > requested.yMapUnits &&
			sectionRegion.xMapUnits >= requested.xMapUnits && sectionRegion.yMapUnits >= requested.yMapUnits &&
			sectionRight <= requestedRight && sectionBottom <= requestedBottom;
	}

	/** Accepts only the exact finite square cell requested from the selected publication. */
	bool BuildingMapService::IsObservationForSection(
		const BuildingCandidateObservation& observation,
		const BuildingCollectionRegion& sectionRegion) noexcept
	{
		return observation.requestedRegion.xMapUnits == sectionRegion.xMapUnits &&
			observation.requestedRegion.yMapUnits == sectionRegion.yMapUnits &&
			observation.requestedRegion.widthMapUnits == sectionRegion.widthMapUnits &&
			observation.requestedRegion.heightMapUnits == sectionRegion.heightMapUnits;
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
