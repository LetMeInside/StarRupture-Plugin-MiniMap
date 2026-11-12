#if defined(MODLOADER_CLIENT_BUILD)

// Implements Section identity, membership observation tracking, private
// completed-image storage, and Section-owned rendering state and context.
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

	/** Reports whether the most recent observation certifies current membership. */
	bool BuildingMapSection::HasCurrentMembershipObservation() const
	{
		return hasCurrentMembershipObservation_;
	}

	/** Returns the current fingerprint only while a complete observation is certified. */
	bool BuildingMapSection::TryGetCurrentMembershipFingerprint(std::uint64_t& fingerprint) const
	{
		if (!hasCurrentMembershipObservation_)
		{
			return false;
		}
		fingerprint = membershipFingerprint_;
		return true;
	}

	/** Tracks complete observations and reports first certification or fingerprint changes. */
	bool BuildingMapSection::RecordMembershipObservation(
		bool isComplete,
		std::uint64_t membershipFingerprint)
	{
		if (!isComplete)
		{
			hasCurrentMembershipObservation_ = false;
			return false;
		}
		const bool fingerprintChanged = !hasPreviousMembershipFingerprint_ ||
			membershipFingerprint_ != membershipFingerprint;
		membershipFingerprint_ = membershipFingerprint;
		hasPreviousMembershipFingerprint_ = true;
		hasCurrentMembershipObservation_ = true;
		return fingerprintChanged;
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
		if (preparingScene_ == nullptr || preparingSceneSourceContentHash_ != sourceContentHash ||
			preparingScene_->contributionStates_.size() != contributionCount)
		{
			preparingScene_ = std::make_unique<Visuals::PreparedBuildingGeometry>();
			preparingScene_->sourceContentHash_ = sourceContentHash;
			preparingScene_->InitializeContributionStates(contributionCount);
			preparingSceneSourceContentHash_ = sourceContentHash;
			if (completedScene_ == nullptr)
			{
				MarkGpuPresentationChanged();
			}
		}
		return preparingScene_.get();
	}

	/** Replaces the visible scene only after the matching replacement is complete. */
	bool BuildingMapSection::CompletePreparingScene(std::uint64_t sourceContentHash) noexcept
	{
		if (preparingScene_ == nullptr || preparingSceneSourceContentHash_ != sourceContentHash ||
			preparingScene_->sourceContentHash_ != sourceContentHash || !preparingScene_->IsComplete())
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			Visuals::SectionFailureDetails failure{"scene promotion", "PreparingScene absent"};
			if (preparingScene_ != nullptr)
			{
				failure.Condition = "geometry is incomplete";
				if (preparingSceneSourceContentHash_ != sourceContentHash)
				{
					failure.Condition = "PreparingScene source hash mismatch";
					failure.Expected = sourceContentHash;
					failure.Actual = preparingSceneSourceContentHash_;
				}
				else if (preparingScene_->sourceContentHash_ != sourceContentHash)
				{
					failure.Condition = "geometry source identity mismatch";
					failure.Expected = sourceContentHash;
					failure.Actual = preparingScene_->sourceContentHash_;
				}
			}
			ReportFailure(sourceContentHash, -1, failure);
#endif
			return false;
		}
		completedScene_ = std::move(preparingScene_);
		preparingSceneSourceContentHash_ = 0;
		gpuPresentation_.reset();
		gpuPresentationAtlasBindingSignatureValid_ = false;
		gpuPresentationChanged_ = true;
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
#endif

	/** Discards only preparation belonging to an older selected source publication. */
	void BuildingMapSection::InvalidatePreparingScene(std::uint64_t currentSourceContentHash) noexcept
	{
		if (preparingScene_ != nullptr && preparingSceneSourceContentHash_ != currentSourceContentHash)
		{
			preparingScene_.reset();
			preparingSceneSourceContentHash_ = 0;
			MarkGpuPresentationChanged();
		}
	}

	/** Drops incomplete scene values when their scheduler cursor is abandoned. */
	void BuildingMapSection::DiscardPreparingScene() noexcept
	{
		const bool wasVisible = preparingScene_ != nullptr && completedScene_ == nullptr;
		preparingScene_.reset();
		preparingSceneSourceContentHash_ = 0;
		if (wasVisible)
		{
			MarkGpuPresentationChanged();
		}
	}

	/** Drops presentation data only when the selected drawable scene is first-appearance state. */
	void BuildingMapSection::MarkGpuPresentationChanged() noexcept
	{
		if (completedScene_ != nullptr)
		{
			return;
		}
		gpuPresentation_.reset();
		gpuPresentationAtlasBindingSignatureValid_ = false;
		gpuPresentationChanged_ = true;
	}

	/** Hands the change signal to the coordinator without adding a geometry revision. */
	bool BuildingMapSection::ConsumeGpuPresentationChanged() noexcept
	{
		const bool changed = gpuPresentationChanged_;
		gpuPresentationChanged_ = false;
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
	void BuildingMapSection::ResetRenderingState() noexcept
	{
		renderingState_.reset();
		DiscardPreparingScene();
	}
}

#endif // MODLOADER_CLIENT_BUILD
