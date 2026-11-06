#if defined(MODLOADER_CLIENT_BUILD)

// Owns acquisition resources, delegates session lifecycle and prepares known sprite render inputs.
#include "SectionScheduler.h"

#include "BuildingScheduler.h"
#include "../Cache/CpuSpriteSourceManager.h"
#include "../Cache/SpriteDiskCache.h"
#include "../Visuals/SpriteGeometry.h"

#include <algorithm>
#include <utility>

namespace Services::Map::Buildings::Scheduling
{
	/** Stores service configuration, then creates each owner before its borrowing dependents. */
	SectionScheduler::SectionScheduler(
		double sectionSizeMapUnits,
		std::uint32_t sectionResolutionPixels,
		std::filesystem::path buildingsCacheDirectory)
		: sectionSizeMapUnits_(sectionSizeMapUnits)
		, sectionResolutionPixels_(sectionResolutionPixels)
	{
		spriteDiskCache_.reset(new Cache::SpriteDiskCache(std::move(buildingsCacheDirectory)));
		cpuSpriteSourceManager_.reset(new Cache::CpuSpriteSourceManager(spriteDiskCache_.get()));
		buildingScheduler_.reset(new BuildingScheduler(
			cpuSpriteSourceManager_.get(), spriteDiskCache_.get()));
	}

	/** Destroys subordinates in reverse member order; native retirement remains service-directed. */
	SectionScheduler::~SectionScheduler() = default;

	/** Delegates session creation without changing BuildingScheduler's generation contract. */
	bool SectionScheduler::BeginCaptureGameWorldSession(std::uint64_t sessionGeneration)
	{
		const double pixelsPerMapUnit =
			static_cast<double>(sectionResolutionPixels_) / sectionSizeMapUnits_;
		return buildingScheduler_ != nullptr &&
			buildingScheduler_->BeginSession(sessionGeneration, pixelsPerMapUnit);
	}

	/** Advances the existing retirement path and releases no native owner before completion. */
	bool SectionScheduler::AdvanceCaptureGameWorldSessionShutdown()
	{
		return buildingScheduler_ == nullptr || buildingScheduler_->AdvanceSessionShutdown();
	}

	/** Processes both known passes from one fixed publication without claiming unknown content is absent. */
	void SectionScheduler::PrepareKnownSpriteRequirements(
		const std::shared_ptr<const IBuildingSourcePublication>& publicationOwner,
		KnownSpritePreparationResult& output)
	{
		output.PreparationStatus = KnownSpritePreparationResult::Status::InvalidPublication;
		output.SourceReadiness = BuildingSourceReadiness::Unavailable;
		output.MayHaveUnknownRequirements = true;
		output.PendingCount = 0;
		output.FailedCount = 0;
		output.UnavailableCount = 0;
		output.ReadyWithoutPlacementCount = 0;
		output.UnresolvedSpriteGeometryCount = 0;
		output.InvalidRequirementCount = 0;
		output.OrdinarySprites.clear();
		output.OverlayLastSprites.clear();
		output.OrdinarySpriteSupports.clear();
		output.OverlayLastSpriteSupports.clear();
		if (publicationOwner == nullptr)
		{
			return;
		}
		if (!publicationOwner->IsCurrent())
		{
			output.PreparationStatus = KnownSpritePreparationResult::Status::StalePublication;
			return;
		}

		const BuildingSourceView sourceView = publicationOwner->BorrowSourceView();
		output.SourceReadiness = sourceView.Readiness;
		output.MayHaveUnknownRequirements = sourceView.Readiness != BuildingSourceReadiness::Complete;
		PrepareSpriteRequirementSpan(
			sourceView.OrdinarySprites,
			output.OrdinarySprites,
			output.OrdinarySpriteSupports,
			output);
		PrepareSpriteRequirementSpan(
			sourceView.OverlayLastSprites,
			output.OverlayLastSprites,
			output.OverlayLastSpriteSupports,
			output);
		if (!RevalidatePreparedPublication(publicationOwner, output))
		{
			return;
		}
		if (output.InvalidRequirementCount != 0)
		{
			output.OrdinarySprites.clear();
			output.OverlayLastSprites.clear();
			output.OrdinarySpriteSupports.clear();
			output.OverlayLastSpriteSupports.clear();
			output.PreparationStatus = KnownSpritePreparationResult::Status::InvalidSourceRequirement;
			return;
		}
		output.PreparationStatus = KnownSpritePreparationResult::Status::KnownRequirementsProcessed;
	}

	/** Validates and resolves each descriptor in source order, retaining repeated contributor occurrences. */
	void SectionScheduler::PrepareSpriteRequirementSpan(
		std::span<const BuildingSpriteSourceRecord> requirements,
		std::vector<Visuals::BuildingSpriteRenderInput>& outputSprites,
		std::vector<KnownSpritePreparationResult::PreparedSpriteSupport>& outputSupports,
		KnownSpritePreparationResult& output)
	{
		for (const BuildingSpriteSourceRecord& requirement : requirements)
		{
			Cache::OwnedSpriteKey ownedKey;
			if (!Cache::OwnedSpriteKey::TryCopy(requirement.SpriteRequirementKey, ownedKey))
			{
				++output.InvalidRequirementCount;
				continue;
			}
			PrepareSpriteRequirement(requirement, ownedKey, outputSprites, outputSupports, output);
		}
	}

	/** Preserves acquisition outcomes and borrows actual payload metadata only for placed ready sprites. */
	void SectionScheduler::PrepareSpriteRequirement(
		const BuildingSpriteSourceRecord& requirement,
		const Cache::OwnedSpriteKey& ownedKey,
		std::vector<Visuals::BuildingSpriteRenderInput>& outputSprites,
		std::vector<KnownSpritePreparationResult::PreparedSpriteSupport>& outputSupports,
		KnownSpritePreparationResult& output)
	{
		if (buildingScheduler_ == nullptr || cpuSpriteSourceManager_ == nullptr)
		{
			++output.UnavailableCount;
			return;
		}
		const BuildingScheduler::RequirementStatus readiness =
			buildingScheduler_->EnsureSpriteReady(ownedKey);
		if (readiness == BuildingScheduler::RequirementStatus::Pending)
		{
			++output.PendingCount;
			return;
		}
		if (readiness == BuildingScheduler::RequirementStatus::Failed)
		{
			++output.FailedCount;
			return;
		}
		if (readiness == BuildingScheduler::RequirementStatus::Unavailable)
		{
			++output.UnavailableCount;
			return;
		}
		PrepareReadySpriteRequirement(
			requirement,
			ownedKey,
			outputSprites,
			outputSupports,
			output);
	}

	/** Borrows the resident payload and appends an input only after its support has been established. */
	void SectionScheduler::PrepareReadySpriteRequirement(
		const BuildingSpriteSourceRecord& requirement,
		const Cache::OwnedSpriteKey& ownedKey,
		std::vector<Visuals::BuildingSpriteRenderInput>& outputSprites,
		std::vector<KnownSpritePreparationResult::PreparedSpriteSupport>& outputSupports,
		KnownSpritePreparationResult& output)
	{
		if (cpuSpriteSourceManager_ == nullptr)
		{
			++output.UnavailableCount;
			return;
		}
		const Cache::CpuSpriteSourceManager::SpriteResult spriteResult =
			cpuSpriteSourceManager_->GetSprite(ownedKey);
		if (spriteResult.Result != Cache::CpuSpriteSourceManager::Status::Ready ||
			spriteResult.Payload == nullptr ||
			spriteResult.Payload->Format() != Cache::ProcessedSpritePayload::PixelFormat::StraightRgba8SrgbDisplay)
		{
			++output.UnavailableCount;
			return;
		}
		if (!requirement.HasPlacement)
		{
			++output.ReadyWithoutPlacementCount;
			return;
		}

		const Cache::ProcessedSpritePayload& payload = *spriteResult.Payload;
		Visuals::BuildingSpriteRenderInput renderInput;
		renderInput.Pixels = payload.Pixels();
		renderInput.PixelWidth = payload.Width();
		renderInput.PixelHeight = payload.Height();
		renderInput.PixelRegion = {{0.0, 0.0, double(payload.Width()), double(payload.Height())}};
		renderInput.Placement = requirement.Placement;
		renderInput.ProjectionSourceSize = payload.CaptureSize();
		renderInput.CaptureBounds = payload.Bounds();
		renderInput.Projection = payload.Projection();
		KnownSpritePreparationResult::PreparedSpriteSupport support;
		if (!TryBuildPreparedSpriteSupport(renderInput, support))
		{
			++output.UnresolvedSpriteGeometryCount;
			return;
		}
		outputSprites.push_back(renderInput);
		outputSupports.push_back(support);
	}

	/** Uses the independent renderer's projected corners to derive one world-space XY envelope. */
	bool SectionScheduler::TryBuildPreparedSpriteSupport(
		const Visuals::BuildingSpriteRenderInput& renderInput,
		KnownSpritePreparationResult::PreparedSpriteSupport& output) const noexcept
	{
		Visuals::SpriteWorldQuad worldQuad;
		if (!Visuals::SpriteGeometry::BuildWorldQuad(
			renderInput.Placement,
			renderInput.ProjectionSourceSize,
			renderInput.CaptureBounds,
			renderInput.Projection,
			worldQuad))
		{
			return false;
		}

		KnownSpritePreparationResult::PreparedSpriteSupport support;
		support.MinimumX = worldQuad.Corners[0][0];
		support.MinimumY = worldQuad.Corners[0][1];
		support.MaximumX = worldQuad.Corners[0][0];
		support.MaximumY = worldQuad.Corners[0][1];
		for (const auto& corner : worldQuad.Corners)
		{
			support.MinimumX = (std::min)(support.MinimumX, corner[0]);
			support.MinimumY = (std::min)(support.MinimumY, corner[1]);
			support.MaximumX = (std::max)(support.MaximumX, corner[0]);
			support.MaximumY = (std::max)(support.MaximumY, corner[1]);
		}
		output = support;
		return true;
	}

	/** Withdraws both pass arrays if the fixed publication lost eligibility during processing. */
	bool SectionScheduler::RevalidatePreparedPublication(
		const std::shared_ptr<const IBuildingSourcePublication>& publicationOwner,
		KnownSpritePreparationResult& output) const noexcept
	{
		if (publicationOwner != nullptr && publicationOwner->IsCurrent())
		{
			return true;
		}
		output.OrdinarySprites.clear();
		output.OverlayLastSprites.clear();
		output.OrdinarySpriteSupports.clear();
		output.OverlayLastSpriteSupports.clear();
		output.PreparationStatus = KnownSpritePreparationResult::Status::StalePublication;
		return false;
	}
}

#endif // MODLOADER_CLIENT_BUILD
