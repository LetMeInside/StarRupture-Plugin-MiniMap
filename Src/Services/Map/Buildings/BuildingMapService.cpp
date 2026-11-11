#if defined(MODLOADER_CLIENT_BUILD)

// Implements service-owned Sections and image residency, viewport source selection, image-change routing, and scheduler lifecycle delegation.
#include "BuildingMapService.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include "BuildingMapSection.h"
#include "BuildingMapViewport.h"
#include "Scheduling/SectionSchedulingCoordinator.h"
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
#include "Services/Log.h"
#endif

namespace Services::Map::Buildings
{
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
		observedAcquisitionHashes_.clear();
		appliedSectionHashes_.clear();
		pendingSectionPublications_.clear();
		sectionHashRechecksRequested_.clear();
		if (sectionSchedulingCoordinator_ != nullptr)
		{
			sectionSchedulingCoordinator_->ClearViewportDemand(*this);
		}
		sections_.clear();
		sectionImageResidencyReconciliationPending_ = false;
		mapWidthMapUnits_ = 0.0;
		mapHeightMapUnits_ = 0.0;
	}

	/** Advances the game-thread tick estimator and emits one completed Debug aggregate when due.
	 * @param engineDeltaSeconds Engine-provided delta recorded only as diagnostic input.
	 */
	void BuildingMapService::OnGameTick(float engineDeltaSeconds) noexcept
	{
		frameBudgetController_.OnTick(static_cast<double>(engineDeltaSeconds));
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		Coordination::FrameBudgetController::DiagnosticsSnapshot diagnostics;
		if (frameBudgetController_.TakeDiagnosticsSnapshot(diagnostics))
		{
			if (MiniMap::Services::ILog* log = MiniMap::Services::TryLog())
			{
				MiniMap::Services::Logf(
					*log,
					MiniMap::Services::Level::Info,
					"MiniMap: BuildingMap tick_budget intervalMs=%.1f ticks=%llu intervals={valid:%llu rejected:%llu meanMs:%.3f maxMs:%.3f} engineDeltaMeanSec=%.6f smoothedTickMs=%.3f observedTPS=%.2f nominalBudgetMs={mean:%.3f max:%.3f latest:%.3f} schedulingAdvancements={maxPerTick:%llu multipleTicks:%llu}",
					diagnostics.IntervalDurationMs,
					static_cast<unsigned long long>(diagnostics.ProcessingTickCount),
					static_cast<unsigned long long>(diagnostics.ValidObservedIntervalCount),
					static_cast<unsigned long long>(diagnostics.RejectedIntervalCount),
					diagnostics.MeanValidTickIntervalMs,
					diagnostics.MaximumValidTickIntervalMs,
					diagnostics.MeanEngineDeltaSeconds,
					diagnostics.LatestSmoothedTickIntervalMs,
					diagnostics.LatestObservedTPS,
					diagnostics.MeanNominalBudgetMs,
					diagnostics.MaximumNominalBudgetMs,
					diagnostics.LatestNominalBudgetMs,
					static_cast<unsigned long long>(diagnostics.MaximumSchedulingAdvancementsInOneTick),
					static_cast<unsigned long long>(diagnostics.TicksWithMultipleAdvancements));
			}
		}
#endif
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

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
	/** Delegates one completed numeric interval to the coordinator that owns its aggregates. */
	bool BuildingMapService::TakeSectionSchedulingDiagnosticsSnapshot(
		Scheduling::SectionSchedulingDiagnosticsSnapshot& output) noexcept
	{
		return sectionSchedulingCoordinator_ != nullptr &&
			sectionSchedulingCoordinator_->TakeDiagnosticsSnapshot(output);
	}

	/** Delegates the independent sprite-acquisition interval without owning its measurements. */
	bool BuildingMapService::TakeSpriteAcquisitionDiagnosticsSnapshot(
		Scheduling::BuildingScheduler::DiagnosticsSnapshot& output) noexcept
	{
		return sectionSchedulingCoordinator_ != nullptr &&
			sectionSchedulingCoordinator_->TakeSpriteAcquisitionDiagnosticsSnapshot(output);
	}
#endif

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
			sectionSchedulingCoordinator_->StopCaptureGameWorldSession(*this);
		}
	}

	/** Observes acquisition content and supplies batched Section publications to one shared advancement. */
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
		frameBudgetController_.RecordSchedulingAdvancement();
		if (sectionImageResidencyReconciliationPending_)
		{
			ReconcileSectionImageResidency();
			sectionImageResidencyReconciliationPending_ = false;
		}
		if (sectionSchedulingCoordinator_ != nullptr)
		{
			const bool compareDemandedSectionHashes = ObserveViewportAcquisitionHash(
				viewportHandle,
				*viewport->buildingCollector_);
			const std::set<SectionCoordinate> requestedCoordinates =
				GetSectionRequestCoordinates(*viewport, compareDemandedSectionHashes);
			AcquireSectionPublicationBatches(
				requestedCoordinates,
				viewportHandle,
				compareDemandedSectionHashes);
			ReleasePendingPublicationsAlreadyApplied();
			sectionSchedulingCoordinator_->AdvanceWaitingSections(*this, pendingSectionPublications_);
		}
		return true;
	}

	/** Selects changed viewport cells or waiting cells without a selected publication. */
	std::set<BuildingMapService::SectionCoordinate> BuildingMapService::GetSectionRequestCoordinates(
		const BuildingMapViewport& viewport,
		bool compareViewportDemand)
	{
		std::set<SectionCoordinate> coordinates;
		if (compareViewportDemand)
		{
			AddViewportGridCoordinates(viewport, coordinates);
			for (const SectionCoordinate& coordinate : coordinates)
			{
				sectionSchedulingCoordinator_->RequestSectionWork(
					Scheduling::SectionSchedulingCoordinator::SectionCoordinate{
						coordinate.first,
						coordinate.second});
			}
		}
		for (const auto& coordinate : sectionSchedulingCoordinator_->waitingSections_)
		{
			const SectionCoordinate key{coordinate.GridX, coordinate.GridY};
			if (pendingSectionPublications_.find(key) == pendingSectionPublications_.end())
			{
				coordinates.insert(key);
			}
		}
		return coordinates;
	}

	/** Observes hash zero as ordinary content by using map presence for initialization. */
	bool BuildingMapService::ObserveViewportAcquisitionHash(
		std::uint64_t viewportHandle,
		const IBuildingCollector& buildingCollector)
	{
		const std::uint64_t acquisitionHash = buildingCollector.AcquisitionHash();
		const auto previousHash = observedAcquisitionHashes_.find(viewportHandle);
		const bool changed = previousHash == observedAcquisitionHashes_.end() || previousHash->second != acquisitionHash;
		observedAcquisitionHashes_[viewportHandle] = acquisitionHash;
		return changed || sectionHashRechecksRequested_.find(viewportHandle) != sectionHashRechecksRequested_.end();
	}

	/** Retains a narrow policy-triggered request until its affected Section batch succeeds. */
	void BuildingMapService::RequestViewportSectionHashRecheck(std::uint64_t viewportHandle)
	{
		const auto viewport = viewportsByHandle_.find(viewportHandle);
		if (viewport != viewportsByHandle_.end() && viewport->second != nullptr)
		{
			sectionHashRechecksRequested_.insert(viewportHandle);
			const auto& range = viewport->second->gridRange_;
			InvalidatePendingPublicationsInRange(
				range.firstX,
				range.firstY,
				range.endX,
				range.endY);
		}
	}

	/** Adds every cell in one viewport's current demanded grid range. */
	void BuildingMapService::AddViewportGridCoordinates(
		const BuildingMapViewport& viewport,
		std::set<SectionCoordinate>& coordinates) const
	{
		for (std::int64_t gridY = viewport.gridRange_.firstY; gridY < viewport.gridRange_.endY; ++gridY)
		{
			for (std::int64_t gridX = viewport.gridRange_.firstX; gridX < viewport.gridRange_.endX; ++gridX)
			{
				coordinates.emplace(gridX, gridY);
			}
		}
	}

	/** Requests sources in viewport order, keeps equal-content owners, and clears unverified entries. */
	void BuildingMapService::AcquireSectionPublicationBatches(
		const std::set<SectionCoordinate>& coordinates,
		std::uint64_t recheckedViewportHandle,
		bool clearRecheckWhenCovered)
	{
		std::set<SectionCoordinate> selectedCoordinates;
		for (const auto& viewportEntry : viewportsByHandle_)
		{
			BuildingMapViewport* viewport = viewportEntry.second;
			if (viewport == nullptr || viewport->service_ != this || viewport->buildingCollector_ == nullptr ||
				viewport->viewportHandle_ != viewportEntry.first)
			{
				continue;
			}
			AcquireViewportSectionPublicationBatch(
				viewportEntry.first,
				*viewport,
				coordinates,
				selectedCoordinates,
				clearRecheckWhenCovered && viewportEntry.first == recheckedViewportHandle);
		}
		for (const SectionCoordinate& coordinate : coordinates)
		{
			if (selectedCoordinates.find(coordinate) == selectedCoordinates.end())
			{
				pendingSectionPublications_.erase(coordinate);
			}
		}
	}

	/** Builds exact ordered requests, retains successful immutable results, and preserves failed rechecks. */
	void BuildingMapService::AcquireViewportSectionPublicationBatch(
		std::uint64_t viewportHandle,
		BuildingMapViewport& viewport,
		const std::set<SectionCoordinate>& coordinates,
		std::set<SectionCoordinate>& selectedCoordinates,
		bool clearRecheckWhenCovered)
	{
		std::vector<SectionCoordinate> requestedCoordinates;
		std::vector<BuildingCollectionRegion> requestedRegions;
		const bool completeRequestBatch = BuildViewportSectionRequest(
			viewport,
			coordinates,
			selectedCoordinates,
			requestedCoordinates,
			requestedRegions);
		if (requestedRegions.empty())
		{
			if (!completeRequestBatch)
			{
				sectionHashRechecksRequested_.insert(viewportHandle);
			}
			else if (clearRecheckWhenCovered &&
				AreAllViewportSectionsSelected(viewport, selectedCoordinates))
			{
				sectionHashRechecksRequested_.erase(viewportHandle);
			}
			return;
		}
		std::vector<std::shared_ptr<const BuildingCandidatePublication>> publications;
		if (!TryAcquireSectionPublicationBatch(viewport, requestedRegions, publications))
		{
			sectionHashRechecksRequested_.insert(viewportHandle);
			return;
		}
		RetainSectionPublicationBatch(
			viewportHandle,
			requestedCoordinates,
			requestedRegions,
			publications,
			selectedCoordinates,
			completeRequestBatch,
			clearRecheckWhenCovered);
	}

	/** Converts expected rejected/unavailable collector results into retryable lack of a publication. */
	bool BuildingMapService::TryAcquireSectionPublicationBatch(
		BuildingMapViewport& viewport,
		const std::vector<BuildingCollectionRegion>& requestedRegions,
		std::vector<std::shared_ptr<const BuildingCandidatePublication>>& publications)
	{
		try
		{
			publications = viewport.buildingCollector_->AcquireSectionPublications(requestedRegions);
			return true;
		}
		catch (const SectionPublicationUnavailable&)
		{
			return false;
		}
	}

	/** Rejects broken output correspondence and retains the first valid registered publication per cell. */
	void BuildingMapService::RetainSectionPublicationBatch(
		std::uint64_t viewportHandle,
		const std::vector<SectionCoordinate>& requestedCoordinates,
		const std::vector<BuildingCollectionRegion>& requestedRegions,
		const std::vector<std::shared_ptr<const BuildingCandidatePublication>>& publications,
		std::set<SectionCoordinate>& selectedCoordinates,
		bool completeRequestBatch,
		bool clearRecheckWhenCovered)
	{
		if (publications.size() != requestedCoordinates.size())
		{
			throw std::logic_error("Section publication batch did not preserve request cardinality.");
		}
		for (std::size_t index = 0; index < publications.size(); ++index)
		{
			const auto& publication = publications[index];
			const BuildingCollectionRegion& expectedRegion = requestedRegions[index];
			const BuildingCollectionRegion& actualRegion = publication != nullptr
				? publication->SectionRegion()
				: expectedRegion;
			if (publication == nullptr || actualRegion.xMapUnits != expectedRegion.xMapUnits ||
				actualRegion.yMapUnits != expectedRegion.yMapUnits ||
				actualRegion.widthMapUnits != expectedRegion.widthMapUnits ||
				actualRegion.heightMapUnits != expectedRegion.heightMapUnits)
			{
				throw std::logic_error("Section publication batch returned a null or mismatched Section.");
			}
			const SectionCoordinate& coordinate = requestedCoordinates[index];
			if (!selectedCoordinates.insert(coordinate).second)
			{
				continue;
			}
			auto existing = pendingSectionPublications_.find(coordinate);
			if (existing == pendingSectionPublications_.end())
			{
				pendingSectionPublications_.emplace(coordinate, publication);
			}
			else if (existing->second->ContentHash() != publication->ContentHash())
			{
				existing->second = publication;
			}
		}
		if (!completeRequestBatch)
		{
			sectionHashRechecksRequested_.insert(viewportHandle);
		}
		else if (clearRecheckWhenCovered && AreAllViewportSectionsSelected(
			*viewportsByHandle_.at(viewportHandle), selectedCoordinates))
		{
			sectionHashRechecksRequested_.erase(viewportHandle);
		}
	}

	/** Clears waiting work and its retained publication when committed content already matches. */
	void BuildingMapService::ReleasePendingPublicationsAlreadyApplied()
	{
		for (auto pending = pendingSectionPublications_.begin(); pending != pendingSectionPublications_.end();)
		{
			const auto applied = appliedSectionHashes_.find(pending->first);
			if (applied != appliedSectionHashes_.end() && applied->second == pending->second->ContentHash())
			{
				sectionSchedulingCoordinator_->RecordSectionCompleted(
					*this,
					Scheduling::SectionSchedulingCoordinator::SectionCoordinate{
						pending->first.first,
						pending->first.second});
				pending = pendingSectionPublications_.erase(pending);
				continue;
			}
			++pending;
		}
	}

	/** Releases pending immutable values in a viewport range whose source selection may change. */
	void BuildingMapService::InvalidatePendingPublicationsInRange(
		std::int64_t firstGridX,
		std::int64_t firstGridY,
		std::int64_t endGridX,
		std::int64_t endGridY)
	{
		for (std::int64_t gridY = firstGridY; gridY < endGridY; ++gridY)
		{
			for (std::int64_t gridX = firstGridX; gridX < endGridX; ++gridX)
			{
				const SectionCoordinate coordinate{gridX, gridY};
				pendingSectionPublications_.erase(coordinate);
				if (sectionSchedulingCoordinator_ != nullptr)
				{
					sectionSchedulingCoordinator_->RequestSectionWork(
						Scheduling::SectionSchedulingCoordinator::SectionCoordinate{gridX, gridY});
				}
			}
		}
	}

	/** Invalidates cells leaving one viewport without touching the old/new intersection. */
	void BuildingMapService::InvalidateDepartedViewportPublications(
		std::int64_t previousFirstGridX,
		std::int64_t previousFirstGridY,
		std::int64_t previousEndGridX,
		std::int64_t previousEndGridY,
		std::int64_t nextFirstGridX,
		std::int64_t nextFirstGridY,
		std::int64_t nextEndGridX,
		std::int64_t nextEndGridY)
	{
		for (std::int64_t gridY = previousFirstGridY; gridY < previousEndGridY; ++gridY)
		{
			for (std::int64_t gridX = previousFirstGridX; gridX < previousEndGridX; ++gridX)
			{
				const SectionCoordinate coordinate{gridX, gridY};
				if (coordinate.first >= nextFirstGridX && coordinate.first < nextEndGridX &&
					coordinate.second >= nextFirstGridY && coordinate.second < nextEndGridY)
				{
					continue;
				}
				pendingSectionPublications_.erase(coordinate);
				if (sectionSchedulingCoordinator_ != nullptr)
				{
					sectionSchedulingCoordinator_->RequestSectionWork(
						Scheduling::SectionSchedulingCoordinator::SectionCoordinate{
							coordinate.first,
							coordinate.second});
				}
			}
		}
	}

	/** Preserves request/output alignment while rejecting unrepresentable grid rectangles. */
	bool BuildingMapService::BuildViewportSectionRequest(
		const BuildingMapViewport& viewport,
		const std::set<SectionCoordinate>& coordinates,
		const std::set<SectionCoordinate>& selectedCoordinates,
		std::vector<SectionCoordinate>& requestedCoordinates,
		std::vector<BuildingCollectionRegion>& requestedRegions) const
	{
		bool completeRequestBatch = true;
		for (const SectionCoordinate& coordinate : coordinates)
		{
			if (coordinate.first < viewport.gridRange_.firstX || coordinate.first >= viewport.gridRange_.endX ||
				coordinate.second < viewport.gridRange_.firstY || coordinate.second >= viewport.gridRange_.endY)
			{
				continue;
			}
			if (selectedCoordinates.find(coordinate) != selectedCoordinates.end())
			{
				continue;
			}
			BuildingCollectionRegion region{};
			if (!TryCreateSectionRegion(coordinate.first, coordinate.second, region))
			{
				completeRequestBatch = false;
				continue;
			}
			requestedCoordinates.push_back(coordinate);
			requestedRegions.push_back(region);
		}
		return completeRequestBatch;
	}

	/** Checks whether this acquisition pass selected every Section in one viewport range. */
	bool BuildingMapService::AreAllViewportSectionsSelected(
		const BuildingMapViewport& viewport,
		const std::set<SectionCoordinate>& selectedCoordinates) const
	{
		for (std::int64_t gridY = viewport.gridRange_.firstY; gridY < viewport.gridRange_.endY; ++gridY)
		{
			for (std::int64_t gridX = viewport.gridRange_.firstX; gridX < viewport.gridRange_.endX; ++gridX)
			{
				if (selectedCoordinates.find(SectionCoordinate{gridX, gridY}) == selectedCoordinates.end())
				{
					return false;
				}
			}
		}
		return true;
	}

	/** Creates the exact finite square Section rectangle used by collector batch requests. */
	bool BuildingMapService::TryCreateSectionRegion(
		std::int64_t gridX,
		std::int64_t gridY,
		BuildingCollectionRegion& output) const noexcept
	{
		const double x = static_cast<double>(gridX) * sectionSizeMapUnits_;
		const double y = static_cast<double>(gridY) * sectionSizeMapUnits_;
		const double right = x + sectionSizeMapUnits_;
		const double bottom = y + sectionSizeMapUnits_;
		if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(right) || !std::isfinite(bottom) ||
			right <= x || bottom <= y || right - x != sectionSizeMapUnits_ || bottom - y != sectionSizeMapUnits_)
		{
			return false;
		}
		output = BuildingCollectionRegion{x, y, sectionSizeMapUnits_, sectionSizeMapUnits_};
		return true;
	}

	/** Stores the Section hash only after SectionScheduler reports a successful image or empty commit. */
	void BuildingMapService::RecordSectionAppliedHash(
		std::int64_t gridX,
		std::int64_t gridY,
		std::uint64_t contentHash)
	{
		const SectionCoordinate coordinate{gridX, gridY};
		appliedSectionHashes_[coordinate] = contentHash;
		pendingSectionPublications_.erase(coordinate);
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
		for (auto appliedHash = appliedSectionHashes_.begin(); appliedHash != appliedSectionHashes_.end();)
		{
			if (!IsSectionWithinAnyViewportResidencyRange(appliedHash->first.first, appliedHash->first.second))
			{
				appliedHash = appliedSectionHashes_.erase(appliedHash);
			}
			else
			{
				++appliedHash;
			}
		}
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
				*this,
				gridRange.firstX,
				gridRange.firstY,
				gridRange.endX,
				gridRange.endY);
		}
		const auto& gridRange = viewport->second->gridRange_;
		InvalidatePendingPublicationsInRange(
			gridRange.firstX,
			gridRange.firstY,
			gridRange.endX,
			gridRange.endY);
		observedAcquisitionHashes_.erase(viewportHandle);
		sectionHashRechecksRequested_.erase(viewportHandle);
		viewportsByHandle_.erase(viewport);
		ReconcileSectionImageResidency();
		sectionImageResidencyReconciliationPending_ = false;
	}

	/** Delivers one completed-image availability change to each live subscribed viewport in range. */
	void BuildingMapService::NotifySectionImageChangedToViewports(
		std::int64_t gridX,
		std::int64_t gridY
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		, std::size_t* diagnosticNotifications
#endif
	)
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
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			if (diagnosticNotifications != nullptr)
			{
				++(*diagnosticNotifications);
			}
#endif
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
					*this,
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
			InvalidateDepartedViewportPublications(
				previousRange.firstX,
				previousRange.firstY,
				previousRange.endX,
				previousRange.endY,
				firstGridX,
				firstGridY,
				endGridX,
				endGridY);
			sectionHashRechecksRequested_.insert(viewportHandle);
			sectionImageResidencyReconciliationPending_ = true;
		}
		return true;
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
