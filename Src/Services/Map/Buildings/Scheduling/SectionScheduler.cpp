#if defined(MODLOADER_CLIENT_BUILD)

// Owns acquisition resources, delegates session lifecycle and prepares known sprite render inputs.
#include "SectionScheduler.h"

#include "BuildingScheduler.h"
#include "../Cache/CpuSpriteSourceManager.h"
#include "../Cache/SpriteDiskCache.h"
#include "../Visuals/SpriteGeometry.h"

#include <algorithm>
#include <cmath>
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

	/** Processes known sprites from one fixed publication without assigning a rendering pass. */
	void SectionScheduler::PrepareKnownSpriteRequirements(
		const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
		KnownSpritePreparationResult& output)
	{
		output.PreparationStatus = KnownSpritePreparationResult::Status::InvalidPublication;
		output.CandidateEvidence = {};
		output.MayHaveUnknownRequirements = true;
		output.PendingCount = 0;
		output.FailedCount = 0;
		output.UnavailableCount = 0;
		output.ReadyWithoutPlacementCount = 0;
		output.UnresolvedSpriteGeometryCount = 0;
		output.InvalidRequirementCount = 0;
		output.PreparedSprites.clear();
		if (publicationOwner == nullptr)
		{
			return;
		}
		if (!publicationOwner->IsCurrent())
		{
			output.PreparationStatus = KnownSpritePreparationResult::Status::StalePublication;
			return;
		}

		const BuildingCandidateData candidateData = publicationOwner->BorrowCandidateData();
		const BuildingCandidateSourceData sourceData = publicationOwner->BorrowSourceData();
		output.CandidateEvidence = candidateData.evidence;
		output.MayHaveUnknownRequirements = !candidateData.hasCoverage ||
			!candidateData.evidence.candidateCoverageComplete ||
			!candidateData.evidence.membershipEnumerationComplete ||
			!candidateData.evidence.nativeAcquisitionComplete ||
			candidateData.evidence.hasPendingOrUnresolvedOccurrences;
		if (sourceData.records.size() != candidateData.occurrences.size())
		{
			output.PreparationStatus = KnownSpritePreparationResult::Status::InvalidPublication;
			return;
		}
		PrepareSpriteRequirementSpan(candidateData.occurrences, *publicationOwner, output);
		if (!RevalidatePreparedPublication(publicationOwner, output))
		{
			return;
		}
		if (output.InvalidRequirementCount != 0)
		{
			output.PreparedSprites.clear();
			output.PreparationStatus = KnownSpritePreparationResult::Status::InvalidSourceRequirement;
			return;
		}
		output.PreparationStatus = KnownSpritePreparationResult::Status::KnownRequirementsProcessed;
	}

	/** Copies acquired profile values without selecting alternatives or assigning appearance policy. */
	void SectionScheduler::PrepareSplineProfileAlternatives(
		const BuildingCandidateSourceRecord& source,
		SplineProfilePreparationResult& output) const
	{
		output.SourceStatus = source.splineProfileStatus;
		output.Alternatives.clear();
		if (source.splineProfiles == nullptr)
		{
			return;
		}
		output.Alternatives.reserve(source.splineProfiles->Alternatives.size());
		for (const SplineProfileAlternative& sourceAlternative : source.splineProfiles->Alternatives)
		{
			SplineProfilePreparationResult::Alternative prepared;
			PrepareSplineProfileAlternative(sourceAlternative, prepared);
			output.Alternatives.push_back(std::move(prepared));
		}
	}

	/** Retains unsupported evidence and copies only parameters represented by fixed surface inputs. */
	void SectionScheduler::PrepareSplineProfileAlternative(
		const SplineProfileAlternative& source,
		SplineProfilePreparationResult::Alternative& output)
	{
		using AlternativeStatus = SplineProfilePreparationResult::Alternative::Status;
		if (source.Status != SplineProfileAlternativeStatus::Complete)
		{
			output.PreparationStatus = source.Status == SplineProfileAlternativeStatus::MissingMesh
				? AlternativeStatus::MissingMesh
				: source.Status == SplineProfileAlternativeStatus::InvalidBounds
				? AlternativeStatus::InvalidBounds
				: source.Status == SplineProfileAlternativeStatus::InvalidValues
				? AlternativeStatus::InvalidValues
				: AlternativeStatus::Unsupported;
			return;
		}
		if (!source.HasBounds)
		{
			output.PreparationStatus = AlternativeStatus::InvalidBounds;
			return;
		}
		if (!CopySplineSurfaceValues(source, output.SurfaceInput))
		{
			output.SurfaceInput = {};
			output.PreparationStatus = AlternativeStatus::InvalidValues;
			return;
		}
		if (source.ForwardAxis != 0 || source.CrossSection.StartRoll != 0.0 ||
			source.CrossSection.EndRoll != 0.0 ||
			source.CrossSection.StartScale != source.CrossSection.EndScale ||
			source.CrossSection.StartOffset != source.CrossSection.EndOffset)
		{
			output.SurfaceInput = {};
			output.PreparationStatus = AlternativeStatus::Unsupported;
			return;
		}
		output.PreparationStatus = AlternativeStatus::PreparedNumericalInput;
	}

	/** Copies bounds, fixed section values and preconverted Unreal component transform. */
	bool SectionScheduler::CopySplineSurfaceValues(
		const SplineProfileAlternative& source,
		Visuals::BuildingSplineSurfaceRenderInput& output)
	{
		output.ComponentTransformOverride.emplace();
		double upLength = 0.0;
		if (!std::isfinite(source.CrossSection.StartRoll) || !std::isfinite(source.CrossSection.EndRoll))
		{
			return false;
		}
		for (std::size_t axis = 0; axis < 3; ++axis)
		{
			if (!std::isfinite(source.Bounds.Origin[axis]) || !std::isfinite(source.Bounds.Extent[axis]) ||
				source.Bounds.Extent[axis] < 0.0 || !std::isfinite(source.Transform.Translation[axis]) ||
				!std::isfinite(source.Transform.Scale[axis]) || source.Transform.Scale[axis] == 0.0 ||
				!std::isfinite(1.0 / source.Transform.Scale[axis]) || !std::isfinite(source.CrossSection.Up[axis]))
			{
				return false;
			}
			upLength += source.CrossSection.Up[axis] * source.CrossSection.Up[axis];
			output.Bounds.Origin[axis] = source.Bounds.Origin[axis];
			output.Bounds.Extent[axis] = source.Bounds.Extent[axis];
			output.ComponentTransformOverride->Translation[axis] = source.Transform.Translation[axis];
			output.ComponentTransformOverride->Scale[axis] = source.Transform.Scale[axis];
			output.CrossSection.Up[axis] = source.CrossSection.Up[axis];
		}
		for (std::size_t axis = 0; axis < 2; ++axis)
		{
			if (!std::isfinite(source.CrossSection.StartScale[axis]) ||
				!std::isfinite(source.CrossSection.EndScale[axis]) ||
				!std::isfinite(source.CrossSection.StartOffset[axis]) ||
				!std::isfinite(source.CrossSection.EndOffset[axis]))
			{
				return false;
			}
			output.CrossSection.Scale[axis] = source.CrossSection.StartScale[axis];
			output.CrossSection.Offset[axis] = source.CrossSection.StartOffset[axis];
		}
		if (!std::isfinite(upLength) || upLength == 0.0)
		{
			return false;
		}
		return CopySplineSurfaceRotation(source.Transform.Rotation, *output.ComponentTransformOverride);
	}

	/** Rejects malformed quaternion data instead of normalizing or approximating it. */
	bool SectionScheduler::CopySplineSurfaceRotation(
		const std::array<double, 4>& rotation,
		Visuals::SplineSurfaceTransform& output)
	{
		double norm = 0.0;
		for (std::size_t axis = 0; axis < rotation.size(); ++axis)
		{
			if (!std::isfinite(rotation[axis]))
			{
				return false;
			}
			norm += rotation[axis] * rotation[axis];
		}
		if (!std::isfinite(norm) || std::abs(norm - 1.0) > 1.0e-6)
		{
			return false;
		}
		output.Rotation = rotation;
		return true;
	}

	/** Resolves source records by publication ordinal and retains repeated sprite occurrences. */
	void SectionScheduler::PrepareSpriteRequirementSpan(
		std::span<const BuildingCandidateOccurrence> candidates,
		const BuildingCandidatePublication& publication,
		KnownSpritePreparationResult& output)
	{
		for (const BuildingCandidateOccurrence& candidate : candidates)
		{
			const BuildingCandidateSourceRecord* requirement = nullptr;
			if (!publication.TryGetSourceRecord(candidate.publicationOrdinal, requirement) || requirement == nullptr)
			{
				++output.InvalidRequirementCount;
				continue;
			}
			if (requirement->kind != BuildingCandidateSourceKind::Sprite)
			{
				output.MayHaveUnknownRequirements = true;
				continue;
			}
			if (requirement->status != BuildingCandidateSourceStatus::Complete)
			{
				output.MayHaveUnknownRequirements = true;
			}
			if (requirement->spritePass == BuildingCandidateSpritePass::Unknown)
			{
				output.MayHaveUnknownRequirements = true;
			}
			Cache::OwnedSpriteKey ownedKey;
			if (!Cache::OwnedSpriteKey::TryCopy(requirement->canonicalClassPath, ownedKey))
			{
				++output.InvalidRequirementCount;
				continue;
			}
			PrepareSpriteRequirement(candidate.publicationOrdinal, *requirement, ownedKey, output);
		}
	}

	/** Preserves acquisition outcomes and borrows actual payload metadata only for placed ready sprites. */
	void SectionScheduler::PrepareSpriteRequirement(
		std::uint64_t publicationOrdinal,
		const BuildingCandidateSourceRecord& requirement,
		const Cache::OwnedSpriteKey& ownedKey,
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
			publicationOrdinal,
			requirement,
			ownedKey,
			output);
	}

	/** Borrows the resident payload and appends an input only after its support has been established. */
	void SectionScheduler::PrepareReadySpriteRequirement(
		std::uint64_t publicationOrdinal,
		const BuildingCandidateSourceRecord& requirement,
		const Cache::OwnedSpriteKey& ownedKey,
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
		if (!requirement.hasPlacement)
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
		renderInput.Placement = requirement.placement;
		renderInput.ProjectionSourceSize = payload.CaptureSize();
		renderInput.CaptureBounds = payload.Bounds();
		renderInput.Projection = payload.Projection();
		KnownSpritePreparationResult::PreparedSpriteSupport support;
		if (!TryBuildPreparedSpriteSupport(renderInput, support))
		{
			++output.UnresolvedSpriteGeometryCount;
			return;
		}
		KnownSpritePreparationResult::PreparedSpriteOccurrence prepared;
		prepared.PublicationOrdinal = publicationOrdinal;
		prepared.SpritePass = requirement.spritePass;
		prepared.RenderInput = renderInput;
		prepared.Support = support;
		output.PreparedSprites.push_back(std::move(prepared));
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

	/** Withdraws borrowed prepared inputs if the fixed publication lost eligibility during processing. */
	bool SectionScheduler::RevalidatePreparedPublication(
		const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
		KnownSpritePreparationResult& output) const noexcept
	{
		if (publicationOwner != nullptr && publicationOwner->IsCurrent())
		{
			return true;
		}
		output.PreparedSprites.clear();
		output.PreparationStatus = KnownSpritePreparationResult::Status::StalePublication;
		return false;
	}
}

#endif // MODLOADER_CLIENT_BUILD
