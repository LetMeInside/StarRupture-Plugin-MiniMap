#if defined(MODLOADER_CLIENT_BUILD)

// Implements Section identity, private completed-image storage, and
// Section-owned rendering state and context.
#include "BuildingMapSection.h"

#include "Visuals/BuildingVisualsContext.h"
#include "Visuals/PreparedBuildingGeometry.h"
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
#include "../../Log.h"
#include "Visuals/BuildingVisualsResult.h"
#endif

#include <memory>
#include <utility>

namespace Services::Map::Buildings
{
	/** Stores the fixed integer coordinate assigned by the owning service. */
	BuildingMapSection::BuildingMapSection(std::int64_t gridX, std::int64_t gridY)
		: gridX_(gridX)
		, gridY_(gridY)
	{
	}

	/** Destroys the Section-owned numerical context with its service-owned Section. */
	BuildingMapSection::~BuildingMapSection() = default;

	/** Returns this section's world-grid X coordinate. */
	std::int64_t BuildingMapSection::GetGridX() const
	{
		return gridX_;
	}

	/** Returns this section's world-grid Y coordinate. */
	std::int64_t BuildingMapSection::GetGridY() const
	{
		return gridY_;
	}

	/** Reports whether this Section currently owns a completed image. */
	bool BuildingMapSection::HasCompletedImage() const
	{
		return completedImage_.has_value();
	}

	/** Borrows immutable metadata and bytes from the stored image when available. */
	bool BuildingMapSection::TryBorrowCompletedImage(CompletedImageView& image) const
	{
		image = {};
		if (!completedImage_)
		{
			return false;
		}
		image.PixelWidth = completedImage_->PixelWidth;
		image.PixelHeight = completedImage_->PixelHeight;
		image.Rgba8Bytes = completedImage_->Rgba8Bytes;
		return true;
	}

	/** Selects a stable completed presentation or first-appearance prepared contributions. */
	bool BuildingMapSection::TryBorrowPresentationScene(ScenePresentationView& view) const noexcept
	{
		view = {};
		if (completedScene_ != nullptr)
		{
			view.Scene = completedScene_.get();
			view.IsComplete = true;
			return true;
		}
		if (preparingScene_ == nullptr)
		{
			return false;
		}
		view.Scene = preparingScene_.get();
		view.IsComplete = preparingScene_->IsComplete();
		return true;
	}

	/** Reports exact content identity only for a fully completed Section scene. */
	bool BuildingMapSection::HasCompletedSceneForContentHash(std::uint64_t sourceContentHash) const noexcept
	{
		return completedScene_ != nullptr && completedScene_->IsComplete() &&
			completedScene_->SourceContentHash() == sourceContentHash;
	}

	/** Releases the image without changing Section identity or numerical context. */
	void BuildingMapSection::InvalidateCompletedImage()
	{
		completedImage_.reset();
	}

	/** Replaces completed storage by moving a trusted finalized RGBA8 byte vector. */
	void BuildingMapSection::AdoptCompletedImage(
		std::uint32_t pixelWidth,
		std::uint32_t pixelHeight,
		std::vector<std::uint8_t>&& rgba8Bytes)
	{
		CompletedImage replacement;
		replacement.PixelWidth = pixelWidth;
		replacement.PixelHeight = pixelHeight;
		replacement.Rgba8Bytes = std::move(rgba8Bytes);
		completedImage_ = std::move(replacement);
	}

	/** Creates a source-bound replacement only when the selected content changes. */
	Visuals::PreparedBuildingGeometry* BuildingMapSection::BeginPreparingScene(
		std::uint64_t sourceContentHash,
		std::size_t contributionCount)
	{
		if (preparingScene_ == nullptr || preparingScene_->sourceContentHash_ != sourceContentHash ||
			preparingScene_->contributionStates_.size() != contributionCount)
		{
			if (completedScene_ == nullptr && preparingScene_ != nullptr)
			{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
				ReportGpuPresentationWithdrawal(
					"scene-generation-replacement", preparingScene_->SourceContentHash(), sourceContentHash, true);
#endif
			}
			if (gpuGeometryAssembly_ != nullptr && gpuGeometryAssembly_->SourceScene == preparingScene_.get())
			{
				gpuGeometryAssembly_.reset();
			}
			preparingScene_ = std::make_unique<Visuals::PreparedBuildingGeometry>();
			preparingScene_->sourceContentHash_ = sourceContentHash;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			preparingScene_->diagnosticGeneration_ = BuildingMapPerformanceDiagnostics::NextGeneration();
#endif
			preparingScene_->InitializeContributionStates(contributionCount);
			if (completedScene_ == nullptr)
			{
				gpuPresentation_.reset();
				gpuPresentationAtlasRevisionValid_ = false;
				gpuGeometrySnapshotChanged_ = true;
			}
		}
		return preparingScene_.get();
	}

	/** Replaces the visible scene only after the matching replacement is complete. */
	bool BuildingMapSection::CompletePreparingScene(std::uint64_t sourceContentHash) noexcept
	{
		if (preparingScene_ == nullptr || preparingScene_->sourceContentHash_ != sourceContentHash ||
			!preparingScene_->IsComplete())
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			Visuals::SectionFailureDetails failure{"scene promotion", "PreparingScene absent"};
			if (preparingScene_ != nullptr)
			{
				failure.Condition = "geometry is incomplete";
				if (preparingScene_->sourceContentHash_ != sourceContentHash)
				{
					failure.Condition = "PreparingScene source hash mismatch";
					failure.Expected = sourceContentHash;
					failure.Actual = preparingScene_->sourceContentHash_;
				}
			}
			ReportFailure(sourceContentHash, -1, failure);
#endif
			return false;
		}
		const bool hadCompletedScene = completedScene_ != nullptr;
		const bool hadPreparingAssembly = gpuGeometryAssembly_ != nullptr &&
			gpuGeometryAssembly_->SourceScene == preparingScene_.get();
		if (gpuGeometryAssembly_ != nullptr && !hadPreparingAssembly)
		{
			gpuGeometryAssembly_.reset();
		}
		completedScene_ = std::move(preparingScene_);
		if (completedScene_->drawableGpuTriangleCount_ == 0)
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			ReportGpuPresentationWithdrawal("completed-empty-replacement", std::nullopt, sourceContentHash, true);
#endif
			gpuPresentation_.reset();
			gpuPresentationAtlasRevisionValid_ = false;
			gpuGeometryAssembly_.reset();
			gpuGeometrySnapshotChanged_ = true;
		}
		else if (hadCompletedScene && !hadPreparingAssembly)
		{
			gpuPresentationChanged_ = true;
		}
		return true;
	}

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
	/** Reports scalar failure evidence using this Section's immutable grid coordinate. */
	void BuildingMapSection::ReportFailure(
		std::optional<std::uint64_t> sourceContentHash,
		int status,
		const Visuals::SectionFailureDetails& failure) const noexcept
	{
		if (auto* log = MiniMap::Services::TryLog())
		{
			MiniMap::Services::Logf(*log, MiniMap::Services::Level::Warn,
				"MiniMap: Section failure GridX=%lld GridY=%lld sourceHash=%s0x%016llx "
				"stage=%s condition=%s status=%d statusName=%s ordinal=%lld descriptorIndex=%lld surfaceIndex=%lld "
				"expected=%llu actual=%llu operationStatus=%d publicationCount=%lld",
				static_cast<long long>(gridX_), static_cast<long long>(gridY_),
				sourceContentHash.has_value() ? "" : "unavailable/",
				static_cast<unsigned long long>(sourceContentHash.value_or(0)),
				failure.Stage, failure.Condition, status, failure.StatusName != nullptr ? failure.StatusName : "-",
				failure.PublicationOrdinal, failure.DescriptorIndex, failure.SurfaceIndex,
				failure.Expected, failure.Actual, failure.OperationStatus, failure.PublicationCount);
		}
	}

	/** Reports the old drawable owner and scene generations before a Section presentation is withdrawn. */
	void BuildingMapSection::ReportGpuPresentationWithdrawal(
		const char* transition,
		std::optional<std::uint64_t> previousSourceHash,
		std::optional<std::uint64_t> nextSourceHash,
		std::optional<bool> demanded) const noexcept
	{
		const auto geometry = gpuPresentation_ != nullptr ? gpuPresentation_->PreparedGeometry : nullptr;
		if (geometry == nullptr || (geometry->OrdinaryTriangles.empty() &&
			geometry->SolidSurfaces.empty() && geometry->OverlayTriangles.empty()))
		{
			return;
		}
		if (auto* log = MiniMap::Services::TryLog())
		{
			MiniMap::Services::Logf(*log, MiniMap::Services::Level::Warn,
				"MiniMap: BuildingMapPresentationTrace event=withdrawal grid=(%lld,%lld) "
				"transition=%s previousHash=%s0x%016llx nextHash=%s0x%016llx "
				"completed=%d completedHash=0x%016llx preparing=%d preparingHash=0x%016llx "
				"gpuPresentation=1 triangles={ordinary:%llu solid:%llu overlay:%llu} demanded=%s",
				static_cast<long long>(gridX_), static_cast<long long>(gridY_), transition,
				previousSourceHash.has_value() ? "" : "unknown/",
				static_cast<unsigned long long>(previousSourceHash.value_or(0)),
				nextSourceHash.has_value() ? "" : "absent/",
				static_cast<unsigned long long>(nextSourceHash.value_or(0)),
				completedScene_ != nullptr ? 1 : 0,
				static_cast<unsigned long long>(completedScene_ != nullptr ? completedScene_->SourceContentHash() : 0),
				preparingScene_ != nullptr ? 1 : 0,
				static_cast<unsigned long long>(preparingScene_ != nullptr ? preparingScene_->SourceContentHash() : 0),
				static_cast<unsigned long long>(geometry->OrdinaryTriangles.size()),
				static_cast<unsigned long long>(geometry->SolidSurfaces.size()),
				static_cast<unsigned long long>(geometry->OverlayTriangles.size()),
				demanded.has_value() ? (*demanded ? "1" : "0") : "unknown");
		}
	}
#endif

	/** Discards only preparation belonging to an older selected source publication. */
	void BuildingMapSection::InvalidatePreparingScene(std::uint64_t currentSourceContentHash) noexcept
	{
		if (preparingScene_ != nullptr && preparingScene_->sourceContentHash_ != currentSourceContentHash)
		{
			if (completedScene_ == nullptr)
			{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
				ReportGpuPresentationWithdrawal(
					"preparing-scene-invalidation", preparingScene_->SourceContentHash(), currentSourceContentHash, true);
#endif
			}
			if (gpuGeometryAssembly_ != nullptr && gpuGeometryAssembly_->SourceScene == preparingScene_.get())
			{
				gpuGeometryAssembly_.reset();
			}
			preparingScene_.reset();
			if (completedScene_ == nullptr)
			{
				gpuPresentation_.reset();
				gpuPresentationAtlasRevisionValid_ = false;
				gpuGeometrySnapshotChanged_ = true;
			}
		}
	}

	/** Drops incomplete scene values when their scheduler cursor is abandoned. */
	void BuildingMapSection::DiscardPreparingScene(
		[[maybe_unused]] const char* transition,
		[[maybe_unused]] std::optional<bool> demanded) noexcept
	{
		const bool wasVisible = preparingScene_ != nullptr && completedScene_ == nullptr;
		if (wasVisible)
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			ReportGpuPresentationWithdrawal(
				transition, std::optional{preparingScene_->SourceContentHash()}, std::nullopt,
				demanded.has_value() ? demanded : std::optional<bool>{true});
#endif
		}
		if (gpuGeometryAssembly_ != nullptr && gpuGeometryAssembly_->SourceScene == preparingScene_.get())
		{
			gpuGeometryAssembly_.reset();
		}
		preparingScene_.reset();
		if (wasVisible)
		{
			gpuPresentation_.reset();
			gpuPresentationAtlasRevisionValid_ = false;
			gpuGeometrySnapshotChanged_ = true;
		}
	}

	/** Drops presentation data only when the selected drawable scene is first-appearance state. */
	void BuildingMapSection::MarkGpuPresentationChanged() noexcept
	{
		if (completedScene_ != nullptr)
		{
			return;
		}
		if (gpuGeometryAssembly_ != nullptr)
		{
			gpuGeometryChangedDuringAssembly_ = true;
		}
		gpuPresentationChanged_ = true;
	}

	/** Hands the change signal to the coordinator without adding a geometry revision. */
	bool BuildingMapSection::ConsumeGpuPresentationChanged() noexcept
	{
		const bool changed = gpuPresentationChanged_;
		gpuPresentationChanged_ = false;
		return changed;
	}

	/** Hands off eligibility changes without changing Section-owned prepared geometry. */
	bool BuildingMapSection::ConsumeGpuGeometrySnapshotChanged() noexcept
	{
		const bool changed = gpuGeometrySnapshotChanged_;
		gpuGeometrySnapshotChanged_ = false;
		return changed;
	}

	/** Lazily creates and returns this Section's exclusively owned numerical context. */
	Visuals::BuildingVisualsContext& BuildingMapSection::GetOrCreateVisualsContext()
	{
		if (!visualsContext_)
		{
			visualsContext_ = std::make_unique<Visuals::BuildingVisualsContext>();
		}
		return *visualsContext_;
	}

	/** Lazily creates scratch independent of the raster renderer's reusable context. */
	Visuals::BuildingVisualsContext& BuildingMapSection::GetOrCreateGeometryContext()
	{
		if (!geometryContext_)
		{
			geometryContext_ = std::make_unique<Visuals::BuildingVisualsContext>();
		}
		return *geometryContext_;
	}

	/** Releases unfinished render state without changing the context or published image. */
	void BuildingMapSection::ResetRenderingState(
		[[maybe_unused]] const char* transition,
		std::optional<bool> demanded) noexcept
	{
		renderingState_.reset();
		DiscardPreparingScene(transition, demanded);
	}
}

#endif // MODLOADER_CLIENT_BUILD
