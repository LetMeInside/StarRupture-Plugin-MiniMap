#if defined(MODLOADER_CLIENT_BUILD)

// Owns the V2 viewport presentation lifecycle and draws published Section textures.
#include "BuildingLayer.h"

#include "../Map/Buildings/BuildingCollectorV2.h"
#include "../Services/Map/Buildings/BuildingMapSection.h"
#include "../Services/Map/Buildings/BuildingMapService.h"
#include "../Services/Map/Buildings/BuildingMapViewport.h"
#include "../Map/MapTransform.h"

#include <algorithm>
#include <array>
#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
#include <chrono>
#endif
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace MiniMapUI
{
	/** Creates inert collector state, borrows textures/service and validates Section size. */
	BuildingLayer::BuildingLayer(
		MiniMap::Services::ITextures& textures,
		Services::Map::Buildings::BuildingMapService& service)
		: textures_(&textures)
		, service_(&service)
		, sectionSizeMapUnits_(service.GetSectionSizeMapUnits())
		, buildingCollector_(std::make_unique<MiniMap::BuildingAcquisition::BuildingCollectorV2>())
	{
#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		ResetDiagnosticsInterval();
		ResetRenderDiagnosticsInterval();
#endif
		if (!std::isfinite(sectionSizeMapUnits_) || sectionSizeMapUnits_ <= 0.0)
		{
			throw std::invalid_argument("BuildingLayer Section size must be positive finite map units.");
		}
	}

#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
	/** Starts one presentation-operation measurement using the steady clock. */
	BuildingLayer::DurationScope::DurationScope(DiagnosticsSnapshot::Duration& duration) noexcept
		: duration_(duration)
		, started_(std::chrono::steady_clock::now())
	{
	}

	/** Adds elapsed time to its game-thread aggregate when the measured scope exits. */
	BuildingLayer::DurationScope::~DurationScope() noexcept
	{
		const double elapsedMs = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - started_).count();
		duration_.Add(elapsedMs);
	}

	/** Hands off a due nonempty interval without touching presentation ownership. */
	bool BuildingLayer::TakeDiagnosticsSnapshot(DiagnosticsSnapshot& output) noexcept
	{
		const auto now = std::chrono::steady_clock::now();
		const double intervalMs = std::chrono::duration<double, std::milli>(
			now - diagnosticsIntervalStart_).count();
		if (intervalMs < 10000.0 || !HasDiagnosticsActivity())
		{
			return false;
		}
		diagnostics_.IntervalMs = intervalMs;
		output = diagnostics_;
		ResetDiagnosticsInterval();
		return true;
	}

	/** Copies one due render interval while excluding concurrent UI callback updates. */
	bool BuildingLayer::TakeRenderDiagnosticsSnapshot(RenderDiagnosticsSnapshot& output) noexcept
	{
		std::lock_guard<std::mutex> lock(presentationMutex_);
		const auto now = std::chrono::steady_clock::now();
		const double intervalMs = std::chrono::duration<double, std::milli>(
			now - renderDiagnosticsIntervalStart_).count();
		if (intervalMs < 10000.0 || renderDiagnostics_.RenderMutexWait.Samples == 0)
		{
			return false;
		}
		renderDiagnostics_.IntervalMs = intervalMs;
		output = renderDiagnostics_;
		ResetRenderDiagnosticsInterval();
		return true;
	}

	/** Clears only interval measurements and restarts their per-layer clock. */
	void BuildingLayer::ResetDiagnosticsInterval() noexcept
	{
		diagnostics_ = {};
		diagnosticsIntervalStart_ = std::chrono::steady_clock::now();
	}

	/** Clears only UI render aggregates and starts a fresh ten-second window. */
	void BuildingLayer::ResetRenderDiagnosticsInterval() noexcept
	{
		renderDiagnostics_ = {};
		renderDiagnosticsIntervalStart_ = std::chrono::steady_clock::now();
	}

	/** Returns true when any measured method or classified presentation outcome occurred. */
	bool BuildingLayer::HasDiagnosticsActivity() const noexcept
	{
		if (diagnostics_.OpenViewport.Samples || diagnostics_.ViewportUpdate.Samples ||
			diagnostics_.PrepareCpuImages.Samples ||
			diagnostics_.GpuPublicationPass.Samples || diagnostics_.TextureUploadCall.Samples ||
			diagnostics_.TextureReleaseCall.Samples || diagnostics_.SuccessfulOpenViewports ||
			diagnostics_.SuccessfulViewportUpdates || diagnostics_.RejectedViewportUpdates ||
			diagnostics_.PreparedCoordinates || diagnostics_.PublicationPassAlreadyActive ||
			diagnostics_.OutstandingTextureOperationRejections ||
			diagnostics_.ValidatedTexturePublications || diagnostics_.ProcessedRemovalUpdates ||
			diagnostics_.DischargedTextureReceipts)
		{
			return true;
		}
		for (const auto count : diagnostics_.CoordinateResolutionCounts)
		{
			if (count != 0) return true;
		}
		for (const auto count : diagnostics_.TextureUploadStateCounts)
		{
			if (count != 0) return true;
		}
		for (const auto count : diagnostics_.TextureReleaseStateCounts)
		{
			if (count != 0) return true;
		}
		return false;
	}
#endif

	/** Initializes the owned collector and opens this layer's only viewport when gameplay is active. */
	bool BuildingLayer::OpenViewport(
		double xMapUnits,
		double yMapUnits,
		double widthMapUnits,
		double heightMapUnits)
	{
#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		DurationScope durationScope(diagnostics_.OpenViewport);
#endif
		{
			std::lock_guard<std::mutex> lock(presentationMutex_);
			if (shutdownRequested_)
			{
				return false;
			}
		}
		if (buildingCollector_ == nullptr || viewport_ != nullptr)
		{
			return false;
		}
		if (service_->GetMapWidthMapUnits() == 0.0 || service_->GetMapHeightMapUnits() == 0.0)
		{
			return false;
		}
		if (!buildingCollector_->InitializeResources())
		{
			return false;
		}
		std::unique_ptr<Services::Map::Buildings::BuildingMapViewport> viewport =
			service_->OpenViewport(
				buildingCollector_.get(),
				xMapUnits,
				yMapUnits,
				widthMapUnits,
				heightMapUnits);
		if (viewport == nullptr)
		{
			return false;
		}
		SetViewport(std::move(viewport));
#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		++diagnostics_.SuccessfulOpenViewports;
#endif
		return true;
	}

	/** Removes the callback and closes the viewport without backend resource calls. */
	BuildingLayer::~BuildingLayer()
	{
		DetachViewportAndClearCpuState();
	}

	/** Replaces the viewport subscription and discovers its current cells. */
	void BuildingLayer::SetViewport(
		std::unique_ptr<Services::Map::Buildings::BuildingMapViewport> viewport)
	{
		{
			std::lock_guard<std::mutex> lock(presentationMutex_);
			if (shutdownRequested_)
			{
				return;
			}
		}
		if (viewport_ != nullptr)
		{
			viewport_->SetSectionImageChangedHandler({});
		}
		WithdrawAllPublishedTextures();
		dirtyCoordinates_.clear();
		pendingUpdates_.clear();
		viewport_.reset();
		if (viewport == nullptr)
		{
			ReleasePendingTextures();
			return;
		}
		viewport_ = std::move(viewport);
		viewport_->SetSectionImageChangedHandler(
			[this](std::int64_t gridX, std::int64_t gridY)
			{
				dirtyCoordinates_.emplace(gridX, gridY);
			});
		MarkVisibleCoordinatesDirty();
		ReleasePendingTextures();
	}

	/** Updates the viewport and reconciles CPU work only after acceptance. */
	bool BuildingLayer::Update(double x1, double y1, double x2, double y2)
	{
#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		DurationScope durationScope(diagnostics_.ViewportUpdate);
#endif
#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		bool shuttingDown = false;
		{
			std::lock_guard<std::mutex> lock(presentationMutex_);
			shuttingDown = shutdownRequested_;
		}
		if (shuttingDown)
		{
			++diagnostics_.RejectedViewportUpdates;
			return false;
		}
#else
		{
			std::lock_guard<std::mutex> lock(presentationMutex_);
			if (shutdownRequested_)
			{
				return false;
			}
		}
#endif
		if (viewport_ == nullptr)
		{
#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
			++diagnostics_.RejectedViewportUpdates;
#endif
			return false;
		}
		const std::set<Coordinate> previousCoordinates = CollectVisibleCoordinates();
		if (!viewport_->Update(x1, y1, x2, y2))
		{
#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
			++diagnostics_.RejectedViewportUpdates;
#endif
			return false;
		}
		const std::set<Coordinate> visibleCoordinates = CollectVisibleCoordinates();
		for (const Coordinate& coordinate : visibleCoordinates)
		{
			if (previousCoordinates.find(coordinate) == previousCoordinates.end())
			{
				dirtyCoordinates_.insert(coordinate);
			}
		}
		DiscardCoordinatesOutside(visibleCoordinates);
		WithdrawDepartedTextures(visibleCoordinates);
		ReleasePendingTextures();
#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		++diagnostics_.SuccessfulViewportUpdates;
#endif
		return true;
	}

	/** Applies changed rail policy and requests Section-hash comparison on the next viewport update. */
	void BuildingLayer::SetRenderDroneRails(bool enabled)
	{
		if (renderDroneRailsEnabled_ == enabled)
		{
			return;
		}
		renderDroneRailsEnabled_ = enabled;
		if (buildingCollector_ != nullptr)
		{
			buildingCollector_->SetRenderDroneRails(enabled);
		}
		if (viewport_ != nullptr)
		{
			viewport_->RequestSectionHashRecheck();
		}
	}

	/** Uploads one snapshot of pending updates and releases withdrawn receipts. */
	void BuildingLayer::PublishGpuImages()
	{
#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		DurationScope durationScope(diagnostics_.GpuPublicationPass);
#endif
		if (publishPassActive_)
		{
#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
			++diagnostics_.PublicationPassAlreadyActive;
#endif
			return;
		}
#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		bool textureOperationOutstanding = false;
		{
			std::lock_guard<std::mutex> lock(presentationMutex_);
			textureOperationOutstanding = activeTextureOperations_ != 0;
		}
		if (textureOperationOutstanding)
		{
			++diagnostics_.OutstandingTextureOperationRejections;
			return;
		}
#else
		{
			std::lock_guard<std::mutex> lock(presentationMutex_);
			if (activeTextureOperations_ != 0)
			{
				return;
			}
		}
#endif
		publishPassActive_ = true;
		bool shuttingDown = false;
		{
			std::lock_guard<std::mutex> lock(presentationMutex_);
			shuttingDown = shutdownRequested_;
		}
		if (!shuttingDown)
		{
			std::vector<std::pair<Coordinate, std::uint64_t>> updateSnapshot;
			updateSnapshot.reserve(pendingUpdates_.size());
			for (const auto& [coordinate, update] : pendingUpdates_)
			{
				updateSnapshot.emplace_back(coordinate, update.Revision);
			}
			for (const auto& [coordinate, revision] : updateSnapshot)
			{
				PublishPendingUpdate(coordinate, revision);
			}
		}
		ReleasePendingTextures();
		publishPassActive_ = false;
	}

	/** Draws only coherent, currently published texture identities. */
	void BuildingLayer::Render(
		MiniMap::Services::ICanvas& ui,
		const MiniMapMap::Transform& transform,
		bool showLayer)
	{
	#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const auto mutexWaitStarted = std::chrono::steady_clock::now();
	#endif
		std::lock_guard<std::mutex> lock(presentationMutex_);
	#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		DurationScope lockedExecutionScope(renderDiagnostics_.RenderLockedExecution);
		const double mutexWaitMs = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - mutexWaitStarted).count();
		renderDiagnostics_.RenderMutexWait.Add(mutexWaitMs);
	#endif
		MiniMap::Services::Vec2 clipMinimum{};
		MiniMap::Services::Vec2 clipMaximum{};
	#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		if (!showLayer)
		{
			++renderDiagnostics_.RenderOutcomeCounts[
				static_cast<std::size_t>(RenderOutcome::HiddenBySettings)];
			return;
		}
		if (shutdownRequested_)
		{
			++renderDiagnostics_.RenderOutcomeCounts[
				static_cast<std::size_t>(RenderOutcome::ShutdownSuppressed)];
			return;
		}
		if (!TryGetClipRectangle(transform, clipMinimum, clipMaximum))
		{
			++renderDiagnostics_.RenderOutcomeCounts[
				static_cast<std::size_t>(RenderOutcome::InvalidClip)];
			return;
		}
		++renderDiagnostics_.RenderOutcomeCounts[
			static_cast<std::size_t>(RenderOutcome::RenderedWithValidClip)];
	#else
		if (!showLayer || shutdownRequested_ ||
			!TryGetClipRectangle(transform, clipMinimum, clipMaximum))
		{
			return;
		}
	#endif
		ui.PushClip(MiniMap::Services::DrawLayer::Window, clipMinimum, clipMaximum, true);
		const MiniMap::Services::Quad textureCoordinates{{
			{0.0f, 0.0f},
			{1.0f, 0.0f},
			{1.0f, 1.0f},
			{0.0f, 1.0f}}};
		for (const auto& [coordinate, entry] : publishedTextures_)
		{
		#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
			++renderDiagnostics_.PublishedEntriesExamined;
		#endif
			if (entry == nullptr || entry->DrawIdentity.id == 0)
			{
			#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
				++renderDiagnostics_.InvalidDrawIdentitySkipped;
			#endif
				continue;
			}
			MiniMap::Services::Quad positions{};
		#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
			if (!TryProjectSectionQuad(coordinate, sectionSizeMapUnits_, transform, positions))
			{
				++renderDiagnostics_.QuadProjectionFailed;
				continue;
			}
			if (!IntersectsClipRectangle(positions, clipMinimum, clipMaximum))
			{
				++renderDiagnostics_.QuadOutsideClip;
				continue;
			}
		#else
			if (!TryProjectSectionQuad(coordinate, sectionSizeMapUnits_, transform, positions) ||
				!IntersectsClipRectangle(positions, clipMinimum, clipMaximum))
			{
				continue;
			}
		#endif
			ui.ImageQuad(
				MiniMap::Services::DrawLayer::Window,
				entry->DrawIdentity,
				positions,
				textureCoordinates,
				0xFFFFFFFFu);
		#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
			++renderDiagnostics_.ImageQuadSubmissions;
		#endif
		}
		ui.PopClip(MiniMap::Services::DrawLayer::Window);
	}

	/** Closes the viewport, shuts down the collector, and retires published textures. */
	bool BuildingLayer::Shutdown()
	{
		{
			std::lock_guard<std::mutex> lock(presentationMutex_);
			shutdownRequested_ = true;
		}
		DetachViewportAndClearCpuState();
		if (buildingCollector_)
		{
			buildingCollector_->ShutdownResources();
			buildingCollector_.reset();
		}
		WithdrawAllPublishedTextures();
		ReleasePendingTextures();
		const bool complete = HasCompletedResourceShutdown();
		return complete;
	}

	/** Converts each accumulated coordinate against the viewport's latest image state. */
	void BuildingLayer::PrepareCpuImages()
	{
#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		DurationScope durationScope(diagnostics_.PrepareCpuImages);
#endif
		{
			std::lock_guard<std::mutex> lock(presentationMutex_);
			if (shutdownRequested_)
			{
				return;
			}
		}
		if (viewport_ == nullptr)
		{
			return;
		}
		while (!dirtyCoordinates_.empty())
		{
			const Coordinate coordinate = *dirtyCoordinates_.begin();
			dirtyCoordinates_.erase(dirtyCoordinates_.begin());
#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
			++diagnostics_.PreparedCoordinates;
#endif
			PrepareCoordinate(coordinate);
		}
	}

	/** Enumerates current local cells into signed global coordinates. */
	std::set<BuildingLayer::Coordinate> BuildingLayer::CollectVisibleCoordinates() const
	{
		std::set<Coordinate> coordinates;
		if (viewport_ == nullptr)
		{
			return coordinates;
		}
		const std::size_t width = viewport_->GetVisibleGridWidth();
		const std::size_t height = viewport_->GetVisibleGridHeight();
		for (std::size_t localY = 0; localY < height; ++localY)
		{
			for (std::size_t localX = 0; localX < width; ++localX)
			{
				std::int64_t gridX = 0;
				std::int64_t gridY = 0;
				if (viewport_->GetWorldGridCoordinate(localX, localY, gridX, gridY))
				{
					coordinates.emplace(gridX, gridY);
				}
			}
		}
		return coordinates;
	}

	/** Marks every local viewport cell for initial or replacement discovery. */
	void BuildingLayer::MarkVisibleCoordinatesDirty()
	{
		if (viewport_ == nullptr)
		{
			return;
		}
		const std::size_t width = viewport_->GetVisibleGridWidth();
		const std::size_t height = viewport_->GetVisibleGridHeight();
		for (std::size_t localY = 0; localY < height; ++localY)
		{
			for (std::size_t localX = 0; localX < width; ++localX)
			{
				std::int64_t gridX = 0;
				std::int64_t gridY = 0;
				if (viewport_->GetWorldGridCoordinate(localX, localY, gridX, gridY))
				{
					dirtyCoordinates_.emplace(gridX, gridY);
				}
			}
		}
	}

	/** Assigns a new supersession revision before replacing a coordinate's CPU update. */
	void BuildingLayer::StorePendingUpdate(
		const Coordinate& coordinate,
		PendingPresentationUpdate update)
	{
		update.Revision = nextPendingRevision_++;
		if (nextPendingRevision_ == 0)
		{
			nextPendingRevision_ = 1;
		}
		pendingUpdates_.insert_or_assign(coordinate, std::move(update));
	}

	/** Resolves one dirty coordinate and records its current image or absence. */
	void BuildingLayer::PrepareCoordinate(const Coordinate& coordinate)
	{
		std::size_t localX = 0;
		std::size_t localY = 0;
		if (!TryFindLocalCoordinate(coordinate, localX, localY))
		{
#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
			++diagnostics_.CoordinateResolutionCounts[
				static_cast<std::size_t>(CoordinateResolution::NotCovered)];
#endif
			pendingUpdates_.erase(coordinate);
			return;
		}
		const Services::Map::Buildings::BuildingMapSection* section =
			viewport_->GetSectionAt(localX, localY);
		if (section == nullptr)
		{
#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
			++diagnostics_.CoordinateResolutionCounts[
				static_cast<std::size_t>(CoordinateResolution::SectionUnavailable)];
#endif
			RecordImageRemoval(coordinate);
			return;
		}
		Services::Map::Buildings::BuildingMapSection::CompletedImageView image;
		if (!section->TryBorrowCompletedImage(image))
		{
#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
			++diagnostics_.CoordinateResolutionCounts[
				static_cast<std::size_t>(CoordinateResolution::ImageUnavailable)];
#endif
			RecordImageRemoval(coordinate);
			return;
		}
		PendingPresentationUpdate replacement;
		replacement.Kind = PendingUpdateKind::Replacement;
#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		++diagnostics_.CoordinateResolutionCounts[
			static_cast<std::size_t>(CoordinateResolution::ReplacementReady)];
#endif
		StorePendingUpdate(coordinate, std::move(replacement));
	}

	/** Records image absence as a deferred removal update. */
	void BuildingLayer::RecordImageRemoval(const Coordinate& coordinate)
	{
		PendingPresentationUpdate removal;
		StorePendingUpdate(coordinate, std::move(removal));
	}

	/** Uploads one pending replacement with the fresh-name RGBA8 service operation. */
	void BuildingLayer::UploadPendingReplacement(
		const Coordinate& coordinate,
		PendingPresentationUpdate update)
	{
		if (!IsCurrentUploadEligible(coordinate))
		{
			return;
		}
		MiniMap::Services::TextureSize size{};
		TextureEntry* entry = nullptr;
		MiniMap::Services::UploadState result = MiniMap::Services::UploadState::Unavailable;
		{
			std::size_t localX = 0;
			std::size_t localY = 0;
			if (!TryFindLocalCoordinate(coordinate, localX, localY))
			{
				return;
			}
			const Services::Map::Buildings::BuildingMapSection* section =
				viewport_->GetSectionAt(localX, localY);
			if (section == nullptr)
			{
				RecordImageRemoval(coordinate);
				return;
			}
			Services::Map::Buildings::BuildingMapSection::CompletedImageView image;
			if (!section->TryBorrowCompletedImage(image))
			{
				RecordImageRemoval(coordinate);
				return;
			}
			size = MiniMap::Services::TextureSize{image.PixelWidth, image.PixelHeight};
			const MiniMap::Services::TextureRequest request{
				std::string_view{},
				std::optional<MiniMap::Services::TextureSize>{size}};
			{
				std::lock_guard<std::mutex> lock(presentationMutex_);
				textureResources_.emplace_back();
				entry = &textureResources_.back();
				entry->GridCoordinate = coordinate;
				entry->State = TextureResourceState::Acquiring;
				++activeTextureOperations_;
			}
#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
			{
				DurationScope durationScope(diagnostics_.TextureUploadCall);
				result = textures_->UploadRGBA(
					image.Rgba8Bytes,
					size,
					request,
					entry->Owner);
			}
			++diagnostics_.TextureUploadStateCounts[static_cast<std::size_t>(result)];
#else
			result = textures_->UploadRGBA(
				image.Rgba8Bytes,
				size,
				request,
				entry->Owner);
#endif
		}
		{
			std::lock_guard<std::mutex> lock(presentationMutex_);
			--activeTextureOperations_;
		}
		HandleUploadResult(coordinate, std::move(update), size, result, *entry);
	}

	/** Revalidates a returned receipt and preserves retryable replacement work on failure. */
	void BuildingLayer::HandleUploadResult(
		const Coordinate& coordinate,
		PendingPresentationUpdate update,
		MiniMap::Services::TextureSize expectedSize,
		MiniMap::Services::UploadState result,
		TextureEntry& entry)
	{
		if (result == MiniMap::Services::UploadState::Complete &&
			IsUsableTextureReceipt(entry.Owner, expectedSize))
		{
			if (!IsCurrentUploadEligible(coordinate))
			{
				std::lock_guard<std::mutex> lock(presentationMutex_);
				entry.State = TextureResourceState::PendingRelease;
				return;
			}
#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
			const bool published = PublishValidatedTexture(coordinate, expectedSize, entry);
			if (published)
			{
				++diagnostics_.ValidatedTexturePublications;
			}
			if (!published)
#else
			if (!PublishValidatedTexture(coordinate, expectedSize, entry))
#endif
			{
				if (entry.State == TextureResourceState::Acquiring)
				{
					std::lock_guard<std::mutex> lock(presentationMutex_);
					entry.State = TextureResourceState::PendingRelease;
				}
				RestorePendingUpdateIfEligible(coordinate, std::move(update));
			}
			return;
		}
		if (entry.Owner.HasRecord())
		{
			std::lock_guard<std::mutex> lock(presentationMutex_);
			entry.State = TextureResourceState::Unresolved;
			return;
		}
		if (result == MiniMap::Services::UploadState::Rejected ||
			result == MiniMap::Services::UploadState::Unavailable)
		{
			{
				std::lock_guard<std::mutex> lock(presentationMutex_);
				auto owned = std::find_if(
					textureResources_.begin(), textureResources_.end(),
					[&entry](TextureEntry& candidate) { return &candidate == &entry; });
				if (owned != textureResources_.end())
				{
					textureResources_.erase(owned);
				}
			}
			RestorePendingUpdateIfEligible(coordinate, std::move(update));
			return;
		}
		std::lock_guard<std::mutex> lock(presentationMutex_);
		entry.State = TextureResourceState::Unresolved;
	}

	/** Requires a complete validated receipt whose actual size matches the CPU image. */
	bool BuildingLayer::IsUsableTextureReceipt(
		const MiniMap::Services::OwnedTexture& owner,
		MiniMap::Services::TextureSize expectedSize) noexcept
	{
		if (!owner.HasRecord() || !owner.IsValidated())
		{
			return false;
		}
		const MiniMap::Services::Texture identity = owner.View();
		const auto actualSize = owner.Size();
		return identity.id != 0 && actualSize.has_value() &&
			actualSize->width == expectedSize.width && actualSize->height == expectedSize.height;
	}

	/** Rechecks current viewport coverage and pending changes after backend re-entry. */
	bool BuildingLayer::IsCurrentUploadEligible(const Coordinate& coordinate) const
	{
		{
			std::lock_guard<std::mutex> lock(presentationMutex_);
			if (shutdownRequested_)
			{
				return false;
			}
		}
		if (viewport_ == nullptr || dirtyCoordinates_.find(coordinate) != dirtyCoordinates_.end() ||
			pendingUpdates_.find(coordinate) != pendingUpdates_.end())
		{
			return false;
		}
		std::size_t localX = 0;
		std::size_t localY = 0;
		return TryFindLocalCoordinate(coordinate, localX, localY);
	}

	/** Restores a failed replacement marker only while its coordinate remains current and visible. */
	void BuildingLayer::RestorePendingUpdateIfEligible(
		const Coordinate& coordinate,
		PendingPresentationUpdate update)
	{
		if (viewport_ == nullptr || dirtyCoordinates_.find(coordinate) != dirtyCoordinates_.end() ||
			pendingUpdates_.find(coordinate) != pendingUpdates_.end())
		{
			return;
		}
		std::size_t localX = 0;
		std::size_t localY = 0;
		if (!TryFindLocalCoordinate(coordinate, localX, localY))
		{
			return;
		}
		{
			std::lock_guard<std::mutex> lock(presentationMutex_);
			if (shutdownRequested_)
			{
				return;
			}
		}
		pendingUpdates_.emplace(coordinate, std::move(update));
	}

	/** Maps a signed grid coordinate to a local offset in the contiguous viewport range. */
	bool BuildingLayer::TryFindLocalCoordinate(
		const Coordinate& coordinate,
		std::size_t& localX,
		std::size_t& localY) const
	{
		std::int64_t firstGridX = 0;
		std::int64_t firstGridY = 0;
		if (viewport_ == nullptr ||
			!viewport_->GetWorldGridCoordinate(0, 0, firstGridX, firstGridY) ||
			coordinate.first < firstGridX || coordinate.second < firstGridY)
		{
			return false;
		}
		const std::uint64_t offsetX = static_cast<std::uint64_t>(coordinate.first) -
			static_cast<std::uint64_t>(firstGridX);
		const std::uint64_t offsetY = static_cast<std::uint64_t>(coordinate.second) -
			static_cast<std::uint64_t>(firstGridY);
		if (offsetX >= viewport_->GetVisibleGridWidth() || offsetY >= viewport_->GetVisibleGridHeight())
		{
			return false;
		}
		localX = static_cast<std::size_t>(offsetX);
		localY = static_cast<std::size_t>(offsetY);
		return true;
	}

	/** Erases coalesced work for cells that left the accepted viewport range. */
	void BuildingLayer::DiscardCoordinatesOutside(
		const std::set<Coordinate>& visibleCoordinates)
	{
		for (auto iterator = dirtyCoordinates_.begin(); iterator != dirtyCoordinates_.end();)
		{
			if (visibleCoordinates.find(*iterator) == visibleCoordinates.end())
			{
				iterator = dirtyCoordinates_.erase(iterator);
			}
			else
			{
				++iterator;
			}
		}
		for (auto iterator = pendingUpdates_.begin(); iterator != pendingUpdates_.end();)
		{
			if (visibleCoordinates.find(iterator->first) == visibleCoordinates.end())
			{
				iterator = pendingUpdates_.erase(iterator);
			}
			else
			{
				++iterator;
			}
		}
	}

	/** Removes one matching snapshot and applies its latest replacement or removal. */
	void BuildingLayer::PublishPendingUpdate(const Coordinate& coordinate, std::uint64_t revision)
	{
		auto pending = pendingUpdates_.find(coordinate);
		if (pending == pendingUpdates_.end() || pending->second.Revision != revision)
		{
			return;
		}
		PendingPresentationUpdate update = std::move(pending->second);
		pendingUpdates_.erase(pending);
		if (!IsCurrentUploadEligible(coordinate))
		{
			return;
		}
		if (update.Kind == PendingUpdateKind::Removal)
		{
			WithdrawPublishedTexture(coordinate);
#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
			++diagnostics_.ProcessedRemovalUpdates;
#endif
			return;
		}
		UploadPendingReplacement(coordinate, std::move(update));
	}

	/** Withdraws every draw identity while retaining each receipt for backend release. */
	void BuildingLayer::WithdrawAllPublishedTextures()
	{
		std::lock_guard<std::mutex> lock(presentationMutex_);
		for (const auto& [coordinate, entry] : publishedTextures_)
		{
			static_cast<void>(coordinate);
			if (entry != nullptr)
			{
				entry->State = TextureResourceState::PendingRelease;
			}
		}
		publishedTextures_.clear();
	}

	/** Withdraws only published Sections outside the newly accepted viewport grid. */
	void BuildingLayer::WithdrawDepartedTextures(const std::set<Coordinate>& visibleCoordinates)
	{
		std::lock_guard<std::mutex> lock(presentationMutex_);
		for (auto iterator = publishedTextures_.begin(); iterator != publishedTextures_.end();)
		{
			if (visibleCoordinates.find(iterator->first) == visibleCoordinates.end())
			{
				TextureEntry* entry = iterator->second;
				if (entry != nullptr)
				{
					entry->State = TextureResourceState::PendingRelease;
				}
				iterator = publishedTextures_.erase(iterator);
			}
			else
			{
				++iterator;
			}
		}
	}

	/** Withdraws one coordinate while retaining its receipt for deferred release. */
	void BuildingLayer::WithdrawPublishedTexture(const Coordinate& coordinate)
	{
		std::lock_guard<std::mutex> lock(presentationMutex_);
		auto published = publishedTextures_.find(coordinate);
		if (published == publishedTextures_.end())
		{
			return;
		}
		TextureEntry* entry = published->second;
		if (entry != nullptr)
		{
			entry->State = TextureResourceState::PendingRelease;
		}
		publishedTextures_.erase(published);
	}

	/** Releases withdrawn receipts outside the mutex and retains unresolved ownership. */
	void BuildingLayer::ReleasePendingTextures()
	{
		std::vector<TextureEntry*> eligible;
		{
			std::lock_guard<std::mutex> lock(presentationMutex_);
			if (activeTextureOperations_ != 0)
			{
				return;
			}
			eligible.reserve(textureResources_.size());
			for (TextureEntry& entry : textureResources_)
			{
				if (entry.State == TextureResourceState::PendingRelease)
				{
					entry.State = TextureResourceState::Releasing;
					eligible.push_back(&entry);
				}
			}
			activeTextureOperations_ += eligible.size();
		}
		for (TextureEntry* entry : eligible)
		{
#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
			MiniMap::Services::ReleaseState result;
			{
				DurationScope durationScope(diagnostics_.TextureReleaseCall);
				result = textures_->Release(entry->Owner);
			}
			++diagnostics_.TextureReleaseStateCounts[static_cast<std::size_t>(result)];
			bool discharged = false;
			{
				std::lock_guard<std::mutex> lock(presentationMutex_);
				--activeTextureOperations_;
				discharged = (result == MiniMap::Services::ReleaseState::Empty ||
					result == MiniMap::Services::ReleaseState::Returned) && !entry->Owner.HasRecord();
				if (discharged)
				{
					auto owned = std::find_if(
						textureResources_.begin(), textureResources_.end(),
						[entry](TextureEntry& candidate) { return &candidate == entry; });
					if (owned != textureResources_.end())
					{
						textureResources_.erase(owned);
					}
				}
				else
				{
					entry->State = result == MiniMap::Services::ReleaseState::UnavailableRetained &&
						entry->Owner.HasRecord() ? TextureResourceState::PendingRelease : TextureResourceState::Unresolved;
				}
			}
			if (discharged)
			{
				++diagnostics_.DischargedTextureReceipts;
				continue;
			}
#else
			const MiniMap::Services::ReleaseState result = textures_->Release(entry->Owner);
			std::lock_guard<std::mutex> lock(presentationMutex_);
			--activeTextureOperations_;
			if ((result == MiniMap::Services::ReleaseState::Empty ||
				result == MiniMap::Services::ReleaseState::Returned) && !entry->Owner.HasRecord())
			{
				auto owned = std::find_if(
					textureResources_.begin(), textureResources_.end(),
					[entry](TextureEntry& candidate) { return &candidate == entry; });
				if (owned != textureResources_.end())
				{
					textureResources_.erase(owned);
				}
				continue;
			}
			entry->State = result == MiniMap::Services::ReleaseState::UnavailableRetained &&
				entry->Owner.HasRecord() ? TextureResourceState::PendingRelease : TextureResourceState::Unresolved;
#endif
		}
	}

	/** Publishes a complete receipt atomically and then defers its predecessor's retirement. */
	bool BuildingLayer::PublishValidatedTexture(
		const Coordinate& coordinate,
		MiniMap::Services::TextureSize size,
		TextureEntry& replacement)
	{
		if (!IsCurrentUploadEligible(coordinate) || !IsUsableTextureReceipt(replacement.Owner, size))
		{
			return false;
		}
		std::lock_guard<std::mutex> lock(presentationMutex_);
		if (shutdownRequested_ || dirtyCoordinates_.find(coordinate) != dirtyCoordinates_.end() ||
			pendingUpdates_.find(coordinate) != pendingUpdates_.end())
		{
			return false;
		}
		std::size_t localX = 0;
		std::size_t localY = 0;
		if (!TryFindLocalCoordinate(coordinate, localX, localY))
		{
			return false;
		}
		replacement.DrawIdentity = replacement.Owner.View();
		replacement.State = TextureResourceState::Published;
		auto previous = publishedTextures_.find(coordinate);
		if (previous == publishedTextures_.end())
		{
			try
			{
				publishedTextures_.emplace(coordinate, &replacement);
			}
			catch (...)
			{
				replacement.State = TextureResourceState::PendingRelease;
				return false;
			}
			return true;
		}
		TextureEntry* oldEntry = previous->second;
		previous->second = &replacement;
		if (oldEntry != nullptr)
		{
			oldEntry->State = TextureResourceState::PendingRelease;
		}
		return true;
	}

	/** Removes viewport callbacks and discards only CPU-side preparation state. */
	void BuildingLayer::DetachViewportAndClearCpuState()
	{
		if (viewport_ != nullptr)
		{
			viewport_->SetSectionImageChangedHandler({});
		}
		viewport_.reset();
		dirtyCoordinates_.clear();
		pendingUpdates_.clear();
	}

	/** Checks that no publication, receipt or backend operation remains outstanding. */
	bool BuildingLayer::HasCompletedResourceShutdown() const
	{
		std::lock_guard<std::mutex> lock(presentationMutex_);
		return publishedTextures_.empty() && textureResources_.empty() &&
			activeTextureOperations_ == 0;
	}

	/** Computes finite Section bounds without overflowing signed grid multiplication. */
	bool BuildingLayer::TryGetSectionBounds(
		const Coordinate& coordinate,
		double sectionSizeMapUnits,
		double& left,
		double& top,
		double& right,
		double& bottom) noexcept
	{
		const long double size = static_cast<long double>(sectionSizeMapUnits);
		const long double wideLeft = static_cast<long double>(coordinate.first) * size;
		const long double wideTop = static_cast<long double>(coordinate.second) * size;
		const long double wideRight = wideLeft + size;
		const long double wideBottom = wideTop + size;
		const long double limit = static_cast<long double>((std::numeric_limits<double>::max)());
		if (!std::isfinite(wideLeft) || !std::isfinite(wideTop) ||
			!std::isfinite(wideRight) || !std::isfinite(wideBottom) ||
			std::abs(wideLeft) > limit || std::abs(wideTop) > limit ||
			std::abs(wideRight) > limit || std::abs(wideBottom) > limit)
		{
			return false;
		}
		left = static_cast<double>(wideLeft);
		top = static_cast<double>(wideTop);
		right = static_cast<double>(wideRight);
		bottom = static_cast<double>(wideBottom);
		return std::isfinite(left) && std::isfinite(top) && std::isfinite(right) &&
			std::isfinite(bottom) && right > left && bottom > top;
	}

	/** Projects Section corners through the established map transform and validates float output. */
	bool BuildingLayer::TryProjectSectionQuad(
		const Coordinate& coordinate,
		double sectionSizeMapUnits,
		const MiniMapMap::Transform& transform,
		MiniMap::Services::Quad& positions) noexcept
	{
		double left = 0.0;
		double top = 0.0;
		double right = 0.0;
		double bottom = 0.0;
		if (!TryGetSectionBounds(coordinate, sectionSizeMapUnits, left, top, right, bottom))
		{
			return false;
		}
		const std::array<std::pair<double, double>, 4> corners{{
			{left, top}, {right, top}, {right, bottom}, {left, bottom}}};
		for (std::size_t index = 0; index < corners.size(); ++index)
		{
			MiniMapMap::ScreenPoint point{};
			if (!transform.WorldToScreen(corners[index].first, corners[index].second, point) ||
				!std::isfinite(point.X) || !std::isfinite(point.Y))
			{
				return false;
			}
			positions[index] = {point.X, point.Y};
		}
		return true;
	}

	/** Derives the positive visible map rectangle used by Terrain's canvas clip. */
	bool BuildingLayer::TryGetClipRectangle(
		const MiniMapMap::Transform& transform,
		MiniMap::Services::Vec2& clipMinimum,
		MiniMap::Services::Vec2& clipMaximum) noexcept
	{
		if (!transform.Valid || !std::isfinite(transform.CenterX) || !std::isfinite(transform.CenterY) ||
			!std::isfinite(transform.Width) || !std::isfinite(transform.Height) ||
			transform.Width <= 0.0f || transform.Height <= 0.0f)
		{
			return false;
		}
		const double left = static_cast<double>(transform.CenterX) - transform.Width * 0.5;
		const double top = static_cast<double>(transform.CenterY) - transform.Height * 0.5;
		const double right = static_cast<double>(transform.CenterX) + transform.Width * 0.5;
		const double bottom = static_cast<double>(transform.CenterY) + transform.Height * 0.5;
		if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) || !std::isfinite(bottom))
		{
			return false;
		}
		clipMinimum = {static_cast<float>(left), static_cast<float>(top)};
		clipMaximum = {static_cast<float>(right), static_cast<float>(bottom)};
		return std::isfinite(clipMinimum.x) && std::isfinite(clipMinimum.y) &&
			std::isfinite(clipMaximum.x) && std::isfinite(clipMaximum.y);
	}

	/** Skips a quad whose axis-aligned projected bounds miss the visible clipping rectangle. */
	bool BuildingLayer::IntersectsClipRectangle(
		const MiniMap::Services::Quad& positions,
		MiniMap::Services::Vec2 clipMinimum,
		MiniMap::Services::Vec2 clipMaximum) noexcept
	{
		float left = positions[0].x;
		float top = positions[0].y;
		float right = positions[0].x;
		float bottom = positions[0].y;
		for (const MiniMap::Services::Vec2 point : positions)
		{
			left = (std::min)(left, point.x);
			top = (std::min)(top, point.y);
			right = (std::max)(right, point.x);
			bottom = (std::max)(bottom, point.y);
		}
		return right > clipMinimum.x && left < clipMaximum.x &&
			bottom > clipMinimum.y && top < clipMaximum.y;
	}
}

#endif // MODLOADER_CLIENT_BUILD
