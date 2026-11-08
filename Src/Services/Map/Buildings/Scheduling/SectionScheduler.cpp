#if defined(MODLOADER_CLIENT_BUILD)

// Owns acquisition resources, session lifecycle, and synchronous one-Section image construction.
#include "SectionScheduler.h"

#include "BuildingScheduler.h"
#include "../BuildingMapSection.h"
#include "../Cache/CpuSpriteSourceManager.h"
#include "../Cache/SpriteDiskCache.h"
#include "../Rendering/Rgba16fImageDestination.h"
#include "../Rendering/SectionRendering.h"
#include "../Visuals/SpriteGeometry.h"
#include "../Visuals/SplineGeometry.h"

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
#include <chrono>
#endif
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace Services::Map::Buildings::Scheduling
{
	/** Clears one owned result while preserving vector capacities for reuse. */
	void SectionScheduler::SplineRenderInputPreparationResult::Reset() noexcept
	{
		PublicationOrdinal = 0;
		SplinePolicyIndex = 0;
		CurvePreparation.SourceStatus = BuildingCandidateSourceStatus::Unknown;
		CurvePreparation.ProfileStatus = SplineProfileAcquisitionStatus::Unknown;
		CurvePreparation.Provenance = BuildingCandidateSplineProvenance::Unknown;
		CurvePreparation.CoordinateSpace = BuildingCandidateSplineCoordinateSpace::Unknown;
		CurvePreparation.ConversionIssue = Visuals::SplineNumericalIssue::None;
		CurvePreparation.PreparationStatus = SplineCurvePreparationResult::CurveStatus::NotPrepared;
		CurvePreparation.RootStatus = SplineCurvePreparationResult::RootTransformStatus::NotApplicable;
		CurvePreparation.RootTransform.reset();
		CurvePreparation.Segments.clear();
		Alternatives.clear();
		SurfaceInputs.clear();
		SamplingPolicy = {};
		PreparationStatus = Status::NotAttempted;
	}

	/** Clears scene-owned values without retaining publication or observation storage. */
	void SectionScheduler::SectionScenePreparationResult::Reset() noexcept
	{
		OccurrenceOutcomes.clear();
		OrdinarySprites.clear();
		SplinePreparations.clear();
		OverlayLastSprites.clear();
		CandidateEvidence = {};
		PublicationIdentity = nullptr;
		PreparationReadiness = Readiness::InvalidPublication;
	}

	/** Clears commit evidence while retaining any caller-owned result capacity. */
	void SectionScheduler::SectionImageCommitResult::Reset() noexcept
	{
		CommitStatus = Status::InvalidOrIncompleteInput;
		SceneReadiness.reset();
		NumericalStatus.reset();
		CompletedImageRemoved = false;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		Timing = {};
#endif
	}

	/** Borrows pass arrays and freshly constructs spline views for immediate synchronous consumption. */
	bool SectionScheduler::SectionScenePreparationResult::BorrowRenderSceneInput(
		const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
		std::vector<Visuals::BuildingSplineRenderInput>& scratch,
		Visuals::BuildingRenderSceneInput& output) const
	{
		output = {};
		scratch.clear();
		if (publicationOwner == nullptr || !publicationOwner->IsCurrent() ||
			PublicationIdentity != publicationOwner.get() ||
			PreparationReadiness != Readiness::CompleteSceneInputs)
		{
			return false;
		}
		scratch.reserve(SplinePreparations.size());
		for (const SplineRenderInputPreparationResult& splinePreparation : SplinePreparations)
		{
			const auto occurrence = std::find_if(
				OccurrenceOutcomes.begin(),
				OccurrenceOutcomes.end(),
				[&splinePreparation](const auto& value)
				{
					return value.PublicationOrdinal == splinePreparation.PublicationOrdinal;
				});
			if (occurrence != OccurrenceOutcomes.end() &&
				occurrence->PreparationStatus == KnownSpritePreparationResult::OccurrenceOutcome::Status::TerminalSourceFailure)
			{
				continue;
			}
			Visuals::BuildingSplineRenderInput splineInput;
			if (!splinePreparation.BorrowRenderInput(splineInput))
			{
				scratch.clear();
				return false;
			}
			scratch.push_back(splineInput);
		}
		if (!publicationOwner->IsCurrent())
		{
			scratch.clear();
			return false;
		}
		output.OrdinarySprites = std::span<const Visuals::BuildingSpriteRenderInput>(OrdinarySprites);
		output.Splines = std::span<const Visuals::BuildingSplineRenderInput>(scratch);
		output.OverlayLastSprites = std::span<const Visuals::BuildingSpriteRenderInput>(OverlayLastSprites);
		return true;
	}

	/** Completes zero-candidate or fully transparent Sections, otherwise adopts the complete rendered image. */
	void SectionScheduler::RenderAndCommitSectionImage(
		::Services::Map::Buildings::BuildingMapSection& section,
		const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
		const BuildingCandidateObservation& observation,
		const KnownSpritePreparationResult& knownSpritePreparation,
		std::span<const SplinePolicy> splinePolicies,
		SectionImageCommitResult& output) const
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		using Clock = std::chrono::steady_clock;
		/** Stores whole-operation elapsed time across every return and exception path. */
		struct TotalTimingScope
		{
			SectionImageCommitResult& Result;
			Clock::time_point Started = Clock::now();

			/** Stores the complete attempt duration, including ordinary early returns. */
			~TotalTimingScope() noexcept
			{
				Result.Timing.TotalMs =
					std::chrono::duration<double, std::milli>(Clock::now() - Started).count();
			}
		};
		/** Records one existing phase call into its numeric result field. */
		struct PhaseTimingScope
		{
			double* ElapsedMs;
			Clock::time_point Started = Clock::now();

			/** Starts timing one result field without allocating additional state. */
			explicit PhaseTimingScope(double& elapsedMs) noexcept : ElapsedMs(&elapsedMs) {}

			/** Stores elapsed time once; the destructor safely skips a completed scope. */
			void Finish() noexcept
			{
				if (ElapsedMs == nullptr)
				{
					return;
				}
				*ElapsedMs = std::chrono::duration<double, std::milli>(Clock::now() - Started).count();
				ElapsedMs = nullptr;
			}

			~PhaseTimingScope() noexcept
			{
				Finish();
			}
		};
		[[maybe_unused]] TotalTimingScope totalTiming{output};
#endif
		output.Reset();
		SectionScenePreparationResult scenePreparation;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		PhaseTimingScope scenePreparationTiming{output.Timing.ScenePreparationMs};
#endif
		const bool sceneReady = TryPrepareSectionSceneForCommit(
			section,
			publicationOwner,
			observation,
			knownSpritePreparation,
			splinePolicies,
			scenePreparation,
			output);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		scenePreparationTiming.Finish();
#endif
		if (!sceneReady)
		{
			return;
		}
		if (observation.candidateOrdinals.empty())
		{
			CommitEmptySection(section, publicationOwner, output);
			return;
		}
		std::vector<Visuals::BuildingSplineRenderInput> splineViewScratch;
		Visuals::BuildingRenderSceneInput sceneInput;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		PhaseTimingScope sceneBorrowTiming{output.Timing.SceneBorrowMs};
#endif
		if (!scenePreparation.BorrowRenderSceneInput(publicationOwner, splineViewScratch, sceneInput))
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			sceneBorrowTiming.Finish();
#endif
			output.CommitStatus = publicationOwner->IsCurrent()
				? SectionImageCommitResult::Status::InvalidOrIncompleteInput
				: SectionImageCommitResult::Status::StalePublication;
			return;
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		sceneBorrowTiming.Finish();
#endif
		const Rendering::Rgba16fImageGrid grid =
			CreateSectionRenderingGrid(section, sectionSizeMapUnits_, sectionResolutionPixels_);
		std::vector<std::uint8_t> imageBytes;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		output.Timing.NumericalRenderingInvoked = true;
		PhaseTimingScope numericalRenderingTiming{output.Timing.NumericalRenderingMs};
#endif
		output.NumericalStatus = RenderIntoSectionContext(section, sceneInput, grid, imageBytes);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		numericalRenderingTiming.Finish();
#endif
		if (!IsCompletedRenderCurrentAndComplete(publicationOwner, output))
		{
			return;
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		output.Timing.TransparencyCheckInvoked = true;
		PhaseTimingScope transparencyCheckTiming{output.Timing.TransparencyCheckMs};
#endif
		const bool imageTransparent = IsFullyTransparentRgba16fImage(
			std::span<const std::uint8_t>(imageBytes));
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		transparencyCheckTiming.Finish();
#endif
		if (imageTransparent)
		{
			CommitEmptySection(section, publicationOwner, output);
			return;
		}
		if (!publicationOwner->IsCurrent())
		{
			output.CommitStatus = SectionImageCommitResult::Status::StalePublication;
			return;
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		output.Timing.ImageAdoptionAttempted = true;
		PhaseTimingScope imageAdoptionTiming{output.Timing.ImageAdoptionMs};
#endif
		const bool imageAdopted = section.TryAdoptCompletedImage(
			sectionResolutionPixels_, sectionResolutionPixels_, std::move(imageBytes));
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		imageAdoptionTiming.Finish();
#endif
		if (!imageAdopted)
		{
			output.CommitStatus = SectionImageCommitResult::Status::ImageAdoptionRejected;
			return;
		}
		output.CommitStatus = SectionImageCommitResult::Status::Committed;
	}

	/** Runs exact-cell validation, existing scene preparation and final readiness checks once. */
	bool SectionScheduler::TryPrepareSectionSceneForCommit(
		const ::Services::Map::Buildings::BuildingMapSection& section,
		const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
		const BuildingCandidateObservation& observation,
		const KnownSpritePreparationResult& knownSpritePreparation,
		std::span<const SplinePolicy> splinePolicies,
		SectionScenePreparationResult& output,
		SectionImageCommitResult& commitResult) const
	{
		if (!IsObservationForSection(section, observation)) return false;
		PrepareSectionSceneInputs(publicationOwner, observation, knownSpritePreparation, splinePolicies, output);
		commitResult.SceneReadiness = output.PreparationReadiness;
		using Readiness = SectionScenePreparationResult::Readiness;
		if (output.PreparationReadiness == Readiness::StalePublication ||
			(publicationOwner != nullptr && !publicationOwner->IsCurrent()))
		{
			commitResult.CommitStatus = SectionImageCommitResult::Status::StalePublication;
			return false;
		}
		return publicationOwner != nullptr && output.PreparationReadiness == Readiness::CompleteSceneInputs;
	}

	/** Preserves currentness and complete numerical evidence before output inspection or adoption. */
	bool SectionScheduler::IsCompletedRenderCurrentAndComplete(
		const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
		SectionImageCommitResult& commitResult) const
	{
		if (!publicationOwner->IsCurrent())
		{
			commitResult.CommitStatus = SectionImageCommitResult::Status::StalePublication;
			return false;
		}
		if (!commitResult.NumericalStatus.has_value() ||
			*commitResult.NumericalStatus != Rendering::SectionRendering::Status::Complete)
		{
			commitResult.CommitStatus = SectionImageCommitResult::Status::NumericalRenderingFailed;
			return false;
		}
		return true;
	}

	/** Completes an empty Section without adopting pixel bytes. */
	void SectionScheduler::CommitEmptySection(
		::Services::Map::Buildings::BuildingMapSection& section,
		const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
		SectionImageCommitResult& output) const
	{
		if (publicationOwner == nullptr)
		{
			output.CommitStatus = SectionImageCommitResult::Status::InvalidOrIncompleteInput;
			return;
		}
		if (!publicationOwner->IsCurrent())
		{
			output.CommitStatus = SectionImageCommitResult::Status::StalePublication;
			return;
		}
		const bool completedImageRemoved = section.HasCompletedImage();
		if (!publicationOwner->IsCurrent())
		{
			output.CommitStatus = SectionImageCommitResult::Status::StalePublication;
			return;
		}
		if (completedImageRemoved)
		{
			section.InvalidateCompletedImage();
		}
		output.CompletedImageRemoved = completedImageRemoved;
		output.CommitStatus = SectionImageCommitResult::Status::CommittedEmpty;
	}

	/** Compares each validated pixel with the exact little-endian transparent RGBA16F clear value. */
	bool SectionScheduler::IsFullyTransparentRgba16fImage(
		std::span<const std::uint8_t> imageBytes) const noexcept
	{
		constexpr std::size_t bytesPerPixel = 8;
		constexpr std::array<std::uint8_t, bytesPerPixel> transparentPixelBytes{
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3C};
		std::size_t expectedImageByteCount = 0;
		if (!::Services::Map::Buildings::BuildingMapSection::TryGetCompletedImageByteCount(
			sectionResolutionPixels_,
			sectionResolutionPixels_,
			expectedImageByteCount) ||
			imageBytes.size() != expectedImageByteCount)
		{
			return false;
		}
		for (std::size_t pixelByteOffset = 0; pixelByteOffset < imageBytes.size(); pixelByteOffset += bytesPerPixel)
		{
			for (std::size_t channelByteOffset = 0; channelByteOffset < bytesPerPixel; ++channelByteOffset)
			{
				if (imageBytes[pixelByteOffset + channelByteOffset] != transparentPixelBytes[channelByteOffset])
				{
					return false;
				}
			}
		}
		return !imageBytes.empty();
	}

	/** Renders into the numerical context exclusively owned by the supplied Section. */
	Rendering::SectionRendering::Status SectionScheduler::RenderIntoSectionContext(
		::Services::Map::Buildings::BuildingMapSection& section,
		const Visuals::BuildingRenderSceneInput& sceneInput,
		const Rendering::Rgba16fImageGrid& grid,
		std::vector<std::uint8_t>& imageBytes)
	{
		Visuals::BuildingVisualsContext& context = section.GetOrCreateVisualsContext();
		return Rendering::SectionRendering::RenderSection(sceneInput, context, grid, imageBytes);
	}


	/** Matches one half-open observation rectangle to the supplied Section's grid coordinate. */
	bool SectionScheduler::IsObservationForSection(
		const ::Services::Map::Buildings::BuildingMapSection& section,
		const BuildingCandidateObservation& observation) const
	{
		const double expectedX = static_cast<double>(section.GetGridX()) * sectionSizeMapUnits_;
		const double expectedY = static_cast<double>(section.GetGridY()) * sectionSizeMapUnits_;
		const double right = observation.requestedRegion.xMapUnits + observation.requestedRegion.widthMapUnits;
		const double bottom = observation.requestedRegion.yMapUnits + observation.requestedRegion.heightMapUnits;
		return std::isfinite(right) && std::isfinite(bottom) &&
			right > observation.requestedRegion.xMapUnits && bottom > observation.requestedRegion.yMapUnits &&
			observation.requestedRegion.widthMapUnits == sectionSizeMapUnits_ &&
			observation.requestedRegion.heightMapUnits == sectionSizeMapUnits_ &&
			IsSectionRegionCoordinateMatch(observation.requestedRegion.xMapUnits, expectedX, sectionSizeMapUnits_) &&
			IsSectionRegionCoordinateMatch(observation.requestedRegion.yMapUnits, expectedY, sectionSizeMapUnits_);
	}

	/** Allows rounding differences only when they remain well below adjacent-cell separation. */
	bool SectionScheduler::IsSectionRegionCoordinateMatch(
		double actual,
		double expected,
		double sectionSize) noexcept
	{
		if (!std::isfinite(actual) || !std::isfinite(expected) || !std::isfinite(sectionSize) || sectionSize <= 0.0)
		{
			return false;
		}
		const double coordinateScale = (std::max)(std::abs(actual), (std::max)(std::abs(expected), sectionSize));
		const double roundingTolerance = std::numeric_limits<double>::epsilon() * coordinateScale * 4.0;
		const double maximumTolerance = sectionSize * 0.25;
		if (roundingTolerance >= maximumTolerance)
		{
			return actual == expected;
		}
		return std::abs(actual - expected) <= roundingTolerance;
	}

	/** Creates the existing zero-origin global grid for one Section destination. */
	Rendering::Rgba16fImageGrid SectionScheduler::CreateSectionRenderingGrid(
		const ::Services::Map::Buildings::BuildingMapSection& section,
		double sectionSizeMapUnits,
		std::uint32_t sectionResolutionPixels)
	{
		Rendering::Rgba16fImageGrid grid;
		grid.WorldOriginX = 0.0;
		grid.WorldOriginY = 0.0;
		grid.CellX = section.GetGridX();
		grid.CellY = section.GetGridY();
		grid.Width = sectionResolutionPixels;
		grid.Height = sectionResolutionPixels;
		grid.WorldUnitsPerPixel = sectionSizeMapUnits / static_cast<double>(sectionResolutionPixels);
		return grid;
	}

	/** Creates temporary render spans only when this result owns complete compatible values. */
	bool SectionScheduler::SplineRenderInputPreparationResult::BorrowRenderInput(
		Visuals::BuildingSplineRenderInput& output) const noexcept
	{
		output = {};
		if (PreparationStatus != Status::Composed ||
			CurvePreparation.SourceStatus != BuildingCandidateSourceStatus::Complete ||
			CurvePreparation.ProfileStatus != SplineProfileAcquisitionStatus::Complete ||
			CurvePreparation.PreparationStatus != SplineCurvePreparationResult::CurveStatus::ConvertedWorldSpace ||
			CurvePreparation.ConversionIssue != Visuals::SplineNumericalIssue::None ||
			CurvePreparation.RootStatus != SplineCurvePreparationResult::RootTransformStatus::Prepared ||
			CurvePreparation.Segments.empty() || SurfaceInputs.empty() ||
			!CurvePreparation.RootTransform.has_value() || !std::isfinite(SamplingPolicy.PixelsPerUnit) ||
			SamplingPolicy.PixelsPerUnit <= 0.0)
		{
			return false;
		}

		output.Segments = std::span<const Visuals::SplineCurveSegment>(CurvePreparation.Segments);
		output.SamplingPolicy = SamplingPolicy;
		output.RootTransform = *CurvePreparation.RootTransform;
		output.Surfaces = std::span<const Visuals::BuildingSplineSurfaceRenderInput>(SurfaceInputs);
		output.SurfaceSelectionMode = Visuals::BuildingSplineSurfaceSelectionMode::FirstNumericallyDrawableAlternative;
		return true;
	}

	/** Stores service configuration, then creates each owner before its borrowing dependents. */
	SectionScheduler::SectionScheduler(
		double sectionSizeMapUnits,
		std::uint32_t sectionResolutionPixels,
		std::filesystem::path buildingsCacheDirectory)
		: sectionSizeMapUnits_(sectionSizeMapUnits)
		, sectionResolutionPixels_(sectionResolutionPixels)
	{
		const double pixelsPerMapUnit =
			static_cast<double>(sectionResolutionPixels_) / sectionSizeMapUnits_;
		spriteDiskCache_.reset(new Cache::SpriteDiskCache(std::move(buildingsCacheDirectory)));
		cpuSpriteSourceManager_.reset(new Cache::CpuSpriteSourceManager(spriteDiskCache_.get()));
		buildingScheduler_.reset(new BuildingScheduler(
			cpuSpriteSourceManager_.get(), spriteDiskCache_.get(), pixelsPerMapUnit));
	}

	/** Destroys subordinates in reverse member order; native retirement remains service-directed. */
	SectionScheduler::~SectionScheduler() = default;

	/** Starts gameplay scheduling against the renderer created with service configuration. */
	bool SectionScheduler::BeginCaptureGameWorldSession()
	{
		return buildingScheduler_ != nullptr &&
			buildingScheduler_->BeginSession();
	}

	/** Ends gameplay scheduling and starts cancellation of active request work. */
	void SectionScheduler::StopCaptureGameWorldSession()
	{
		if (buildingScheduler_ != nullptr)
		{
			buildingScheduler_->StopSession();
		}
	}

	/** Returns the persistent renderer without processing lifecycle work here. */
	Capture::BuildingSpriteRenderer* SectionScheduler::GetCapturerLifecycleTarget() noexcept
	{
		return buildingScheduler_ != nullptr
			? buildingScheduler_->GetCapturerLifecycleTarget()
			: nullptr;
	}

	/** Delegates explicit cache deletion to the owned SpriteDiskCache. */
	bool SectionScheduler::ClearCache()
	{
		return spriteDiskCache_ != nullptr && spriteDiskCache_->ClearCache();
	}

	/** Advances the existing renderer acquisition path without changing its session behavior. */
	void SectionScheduler::AdvanceSpriteAcquisition()
	{
		if (buildingScheduler_ != nullptr)
		{
			buildingScheduler_->AdvanceSpriteAcquisition();
		}
	}

	/** Processes known sprites from one fixed publication without assigning a rendering pass. */
	void SectionScheduler::PrepareKnownSpriteRequirements(
		const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
		KnownSpritePreparationResult& output)
	{
		ResetKnownSpritePreparationResult(output);
		if (publicationOwner == nullptr)
		{
			return;
		}
		if (!publicationOwner->IsCurrent())
		{
			output.PreparationStatus = KnownSpritePreparationResult::Status::StalePublication;
			return;
		}
		output.PublicationIdentity = publicationOwner.get();

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
		InitializeKnownSpriteOccurrences(candidateData, sourceData, output);
		PrepareSpriteRequirementSpan(candidateData.occurrences, *publicationOwner, output);
		if (!RevalidatePreparedPublication(publicationOwner, output))
		{
			return;
		}
		if (output.InvalidRequirementCount != 0)
		{
			output.PreparationStatus = KnownSpritePreparationResult::Status::InvalidSourceRequirement;
			return;
		}
		output.PreparationStatus = KnownSpritePreparationResult::Status::KnownRequirementsProcessed;
	}

	/** Resets per-publication counters, identity and short-lived borrowed pixel views. */
	void SectionScheduler::ResetKnownSpritePreparationResult(KnownSpritePreparationResult& output)
	{
		output.PreparationStatus = KnownSpritePreparationResult::Status::InvalidPublication;
		output.PublicationIdentity = nullptr;
		output.CandidateEvidence = {};
		output.MayHaveUnknownRequirements = true;
		output.PendingCount = 0;
		output.FailedCount = 0;
		output.UnavailableCount = 0;
		output.ReadyWithoutPlacementCount = 0;
		output.UnresolvedSpriteGeometryCount = 0;
		output.InvalidRequirementCount = 0;
		output.OccurrenceOutcomes.clear();
		output.PreparedSprites.clear();
	}

	/** Copies candidate/source correspondence into one owned outcome for every ordinal. */
	void SectionScheduler::InitializeKnownSpriteOccurrences(
		const BuildingCandidateData& candidateData,
		const BuildingCandidateSourceData& sourceData,
		KnownSpritePreparationResult& output)
	{
		output.OccurrenceOutcomes.reserve(candidateData.occurrences.size());
		for (std::size_t index = 0; index < candidateData.occurrences.size(); ++index)
		{
			KnownSpritePreparationResult::OccurrenceOutcome outcome;
			outcome.PublicationOrdinal = candidateData.occurrences[index].publicationOrdinal;
			outcome.SourceKind = sourceData.records[index].kind;
			outcome.SourceStatus = sourceData.records[index].status;
			outcome.SpritePass = sourceData.records[index].spritePass;
			outcome.NativeFactsComplete = candidateData.occurrences[index].nativeFactsComplete;
			outcome.IsPendingOrUnresolved = candidateData.occurrences[index].isPendingOrUnresolved;
			output.OccurrenceOutcomes.push_back(outcome);
		}
	}

	/** Prepares ordered neutral inputs for exactly one explicitly supplied publication observation. */
	void SectionScheduler::PrepareSectionSceneInputs(
		const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
		const BuildingCandidateObservation& observation,
		const KnownSpritePreparationResult& knownSpritePreparation,
		std::span<const SplinePolicy> splinePolicies,
		SectionScenePreparationResult& output) const
	{
		output.Reset();
		if (!ValidateSectionPreparationInputs(
			publicationOwner,
			observation,
			knownSpritePreparation,
			output.PreparationReadiness))
		{
			return;
		}
		output.PublicationIdentity = publicationOwner.get();
		output.CandidateEvidence = GetSectionCandidateEvidence(*publicationOwner, observation);
		output.OccurrenceOutcomes.reserve(observation.candidateOrdinals.size());
		for (const std::uint64_t ordinal : observation.candidateOrdinals)
		{
			ProcessSectionOccurrence(
				publicationOwner,
				ordinal,
				knownSpritePreparation,
				splinePolicies,
				output);
		}
		if (!publicationOwner->IsCurrent())
		{
			output.PreparationReadiness = SectionScenePreparationResult::Readiness::StalePublication;
			return;
		}
		const SectionScenePreparationResult::Readiness readiness = DetermineSceneReadiness(output);
		if (readiness == SectionScenePreparationResult::Readiness::CompleteSceneInputs &&
			!publicationOwner->IsCurrent())
		{
			output.PreparationReadiness = SectionScenePreparationResult::Readiness::StalePublication;
			return;
		}
		output.PreparationReadiness = readiness;
	}

	/** Preserves invalid, stale and mismatched input outcomes before scene preparation starts. */
	bool SectionScheduler::ValidateSectionPreparationInputs(
		const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
		const BuildingCandidateObservation& observation,
		const KnownSpritePreparationResult& spritePreparation,
		SectionScenePreparationResult::Readiness& readiness)
	{
		using Readiness = SectionScenePreparationResult::Readiness;
		if (publicationOwner == nullptr ||
			spritePreparation.PreparationStatus == KnownSpritePreparationResult::Status::InvalidPublication)
		{
			readiness = Readiness::InvalidPublication;
			return false;
		}
		if (!publicationOwner->IsCurrent() ||
			spritePreparation.PreparationStatus == KnownSpritePreparationResult::Status::StalePublication)
		{
			readiness = Readiness::StalePublication;
			return false;
		}
		if (spritePreparation.PublicationIdentity != publicationOwner.get() ||
			(spritePreparation.PreparationStatus != KnownSpritePreparationResult::Status::KnownRequirementsProcessed &&
				spritePreparation.PreparationStatus != KnownSpritePreparationResult::Status::InvalidSourceRequirement) ||
			!IsValidSectionObservation(*publicationOwner, observation) ||
			!IsCompatibleSpritePreparation(*publicationOwner, spritePreparation))
		{
			readiness = Readiness::InvalidObservation;
			return false;
		}
		return true;
	}

	/** Validates that one observation exactly corresponds to source-relative publication ordinals. */
	bool SectionScheduler::IsValidSectionObservation(
		const BuildingCandidatePublication& publication,
		const BuildingCandidateObservation& observation)
	{
		const BuildingCandidateData candidateData = publication.BorrowCandidateData();
		const double right = observation.requestedRegion.xMapUnits + observation.requestedRegion.widthMapUnits;
		const double bottom = observation.requestedRegion.yMapUnits + observation.requestedRegion.heightMapUnits;
		if (!std::isfinite(observation.requestedRegion.xMapUnits) ||
			!std::isfinite(observation.requestedRegion.yMapUnits) ||
			!std::isfinite(observation.requestedRegion.widthMapUnits) ||
			!std::isfinite(observation.requestedRegion.heightMapUnits) ||
			observation.requestedRegion.widthMapUnits <= 0.0 || observation.requestedRegion.heightMapUnits <= 0.0 ||
			!std::isfinite(right) || !std::isfinite(bottom) || right <= observation.requestedRegion.xMapUnits ||
			bottom <= observation.requestedRegion.yMapUnits ||
			observation.candidateOrdinals.size() != candidateData.occurrences.size())
		{
			return false;
		}
		for (std::size_t index = 0; index < candidateData.occurrences.size(); ++index)
		{
			if (observation.candidateOrdinals[index] != candidateData.occurrences[index].publicationOrdinal)
			{
				return false;
			}
		}
		return true;
	}

	/** Requires exact source-ordered sprite outcomes from the same publication owner. */
	bool SectionScheduler::IsCompatibleSpritePreparation(
		const BuildingCandidatePublication& publication,
		const KnownSpritePreparationResult& spritePreparation)
	{
		const BuildingCandidateData candidateData = publication.BorrowCandidateData();
		const BuildingCandidateSourceData sourceData = publication.BorrowSourceData();
		if (candidateData.occurrences.size() != spritePreparation.OccurrenceOutcomes.size() ||
			sourceData.records.size() != candidateData.occurrences.size())
		{
			return false;
		}
		for (std::size_t index = 0; index < candidateData.occurrences.size(); ++index)
		{
			if (spritePreparation.OccurrenceOutcomes[index].PublicationOrdinal !=
				candidateData.occurrences[index].publicationOrdinal)
			{
				return false;
			}
		}
		for (const auto& prepared : spritePreparation.PreparedSprites)
		{
			const std::size_t index = static_cast<std::size_t>(prepared.PublicationOrdinal);
			if (index >= sourceData.records.size() ||
				sourceData.records[index].kind != BuildingCandidateSourceKind::Sprite ||
				sourceData.records[index].spritePass != prepared.SpritePass ||
				(spritePreparation.OccurrenceOutcomes[index].PreparationStatus !=
					KnownSpritePreparationResult::OccurrenceOutcome::Status::ReadySprite &&
					spritePreparation.OccurrenceOutcomes[index].PreparationStatus !=
					KnownSpritePreparationResult::OccurrenceOutcome::Status::UnknownSpritePass))
			{
				return false;
			}
		}
		return true;
	}

	/** Prevents one observation from promoting weaker publication evidence or uncovered regions. */
	BuildingCandidateEvidence SectionScheduler::GetSectionCandidateEvidence(
		const BuildingCandidatePublication& publication,
		const BuildingCandidateObservation& observation)
	{
		const BuildingCandidateData candidateData = publication.BorrowCandidateData();
		BuildingCandidateEvidence evidence = observation.evidence;
		evidence.membershipEnumerationComplete = evidence.membershipEnumerationComplete &&
			candidateData.evidence.membershipEnumerationComplete;
		evidence.nativeAcquisitionComplete = evidence.nativeAcquisitionComplete &&
			candidateData.evidence.nativeAcquisitionComplete;
		evidence.hasPendingOrUnresolvedOccurrences = evidence.hasPendingOrUnresolvedOccurrences ||
			candidateData.evidence.hasPendingOrUnresolvedOccurrences;
		const double right = observation.requestedRegion.xMapUnits + observation.requestedRegion.widthMapUnits;
		const double bottom = observation.requestedRegion.yMapUnits + observation.requestedRegion.heightMapUnits;
		const double coverageRight = candidateData.coverage.xMapUnits + candidateData.coverage.widthMapUnits;
		const double coverageBottom = candidateData.coverage.yMapUnits + candidateData.coverage.heightMapUnits;
		const bool regionCovered = candidateData.hasCoverage &&
			observation.requestedRegion.xMapUnits >= candidateData.coverage.xMapUnits &&
			observation.requestedRegion.yMapUnits >= candidateData.coverage.yMapUnits &&
			right <= coverageRight && bottom <= coverageBottom;
		evidence.candidateCoverageComplete = evidence.candidateCoverageComplete &&
			candidateData.evidence.candidateCoverageComplete && regionCovered;
		return evidence;
	}

	/** Accounts for one candidate occurrence and appends only already prepared eligible inputs. */
	void SectionScheduler::ProcessSectionOccurrence(
		const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
		std::uint64_t ordinal,
		const KnownSpritePreparationResult& spritePreparation,
		std::span<const SplinePolicy> splinePolicies,
		SectionScenePreparationResult& output) const
	{
		const BuildingCandidateData candidateData = publicationOwner->BorrowCandidateData();
		const BuildingCandidateSourceData sourceData = publicationOwner->BorrowSourceData();
		if (ordinal >= candidateData.occurrences.size() || ordinal >= sourceData.records.size())
		{
			KnownSpritePreparationResult::OccurrenceOutcome invalid;
			invalid.PublicationOrdinal = ordinal;
			output.OccurrenceOutcomes.push_back(invalid);
			return;
		}
		const auto& candidate = candidateData.occurrences[static_cast<std::size_t>(ordinal)];
		const auto& source = sourceData.records[static_cast<std::size_t>(ordinal)];
		KnownSpritePreparationResult::OccurrenceOutcome outcome;
		InitializeSectionOccurrenceOutcome(ordinal, candidate, source, spritePreparation, outcome);
		const bool sourceBlocked = ProcessTerminalOrPendingSource(source, candidate, outcome);
		if (source.kind == BuildingCandidateSourceKind::Spline)
		{
			ProcessPreparedSpline(publicationOwner, ordinal, source, splinePolicies, outcome, output);
		}
		else if (sourceBlocked)
		{
			output.OccurrenceOutcomes.push_back(outcome);
			return;
		}
		else if (source.kind == BuildingCandidateSourceKind::Sprite)
		{
			ProcessPreparedSprite(ordinal, spritePreparation, outcome, output);
		}
		else
		{
			outcome.PreparationStatus = KnownSpritePreparationResult::OccurrenceOutcome::Status::UnknownSource;
		}
		output.OccurrenceOutcomes.push_back(outcome);
	}

	/** Copies all independent publication, native and sprite evidence into one scene outcome. */
	void SectionScheduler::InitializeSectionOccurrenceOutcome(
		std::uint64_t ordinal,
		const BuildingCandidateOccurrence& candidate,
		const BuildingCandidateSourceRecord& source,
		const KnownSpritePreparationResult& spritePreparation,
		KnownSpritePreparationResult::OccurrenceOutcome& outcome)
	{
		outcome.PublicationOrdinal = ordinal;
		outcome.SourceKind = source.kind;
		outcome.SourceStatus = source.status;
		outcome.SpritePass = source.spritePass;
		outcome.NativeFactsComplete = candidate.nativeFactsComplete;
		outcome.IsPendingOrUnresolved = candidate.isPendingOrUnresolved;
		if (source.kind != BuildingCandidateSourceKind::Sprite)
		{
			return;
		}
		const auto spriteOutcome = std::find_if(
			spritePreparation.OccurrenceOutcomes.begin(),
			spritePreparation.OccurrenceOutcomes.end(),
			[ordinal](const auto& value) { return value.PublicationOrdinal == ordinal; });
		if (spriteOutcome != spritePreparation.OccurrenceOutcomes.end())
		{
			outcome.SpritePreparationStatus = spriteOutcome->PreparationStatus;
		}
	}

	/** Preserves source acquisition distinctions before resource preparation is considered. */
	bool SectionScheduler::ProcessTerminalOrPendingSource(
		const BuildingCandidateSourceRecord& source,
		const BuildingCandidateOccurrence& candidate,
		KnownSpritePreparationResult::OccurrenceOutcome& outcome)
	{
		using Status = KnownSpritePreparationResult::OccurrenceOutcome::Status;
		if (!candidate.nativeFactsComplete || candidate.isPendingOrUnresolved)
		{
			outcome.PreparationStatus = Status::PendingNativeSourceAcquisition;
			return true;
		}
		if (source.kind == BuildingCandidateSourceKind::Unknown)
		{
			outcome.PreparationStatus = Status::UnknownSource;
			return true;
		}
		if (source.status == BuildingCandidateSourceStatus::Invalid ||
			source.status == BuildingCandidateSourceStatus::Unsupported)
		{
			outcome.PreparationStatus = Status::TerminalSourceFailure;
			return true;
		}
		if (source.status == BuildingCandidateSourceStatus::Unavailable)
		{
			outcome.PreparationStatus = Status::TemporarilyUnavailableSource;
			return true;
		}
		if (source.status == BuildingCandidateSourceStatus::Unknown ||
			source.status == BuildingCandidateSourceStatus::Incomplete)
		{
			outcome.PreparationStatus = Status::PendingNativeSourceAcquisition;
			return true;
		}
		return false;
	}

	/** Reuses the publication-level sprite result without repeating cache or geometry preparation. */
	void SectionScheduler::ProcessPreparedSprite(
		std::uint64_t ordinal,
		const KnownSpritePreparationResult& spritePreparation,
		KnownSpritePreparationResult::OccurrenceOutcome& outcome,
		SectionScenePreparationResult& output)
	{
		using Status = KnownSpritePreparationResult::OccurrenceOutcome::Status;
		const auto result = std::find_if(
			spritePreparation.OccurrenceOutcomes.begin(),
			spritePreparation.OccurrenceOutcomes.end(),
			[ordinal](const auto& value) { return value.PublicationOrdinal == ordinal; });
		if (result == spritePreparation.OccurrenceOutcomes.end())
		{
			outcome.PreparationStatus = Status::InvalidSpriteRequirement;
			return;
		}
		outcome.PreparationStatus = result->PreparationStatus;
		outcome.SpritePreparationStatus = result->PreparationStatus;
		if (outcome.PreparationStatus != Status::ReadySprite)
		{
			return;
		}
		const auto prepared = std::find_if(
			spritePreparation.PreparedSprites.begin(),
			spritePreparation.PreparedSprites.end(),
			[ordinal](const auto& value) { return value.PublicationOrdinal == ordinal; });
		if (prepared == spritePreparation.PreparedSprites.end())
		{
			outcome.PreparationStatus = Status::UnresolvedSpriteGeometry;
			return;
		}
		if (prepared->SpritePass == BuildingCandidateSpritePass::FirstPass)
		{
			output.OrdinarySprites.push_back(prepared->RenderInput);
		}
		else if (prepared->SpritePass == BuildingCandidateSpritePass::SecondPass)
		{
			output.OverlayLastSprites.push_back(prepared->RenderInput);
		}
		else
		{
			outcome.PreparationStatus = Status::UnknownSpritePass;
		}
	}

	/** Reuses R2R composition with the direct immutable service policy index. */
	void SectionScheduler::ProcessPreparedSpline(
		const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
		std::uint64_t ordinal,
		const BuildingCandidateSourceRecord& source,
		std::span<const SplinePolicy> splinePolicies,
		KnownSpritePreparationResult::OccurrenceOutcome& outcome,
		SectionScenePreparationResult& output) const
	{
		const std::uint16_t policyIndex = source.splinePolicyIndex;
		const SplinePolicy* policy = policyIndex != 0 && policyIndex < splinePolicies.size()
			? &splinePolicies[policyIndex]
			: nullptr;
		SplineRenderInputPreparationResult preparation;
		ComposeSplineRenderInput(publicationOwner, ordinal, policyIndex, policy, preparation);
		if (outcome.PreparationStatus != KnownSpritePreparationResult::OccurrenceOutcome::Status::NotProcessed)
		{
			output.SplinePreparations.push_back(std::move(preparation));
			return;
		}
		if (preparation.PreparationStatus == SplineRenderInputPreparationResult::Status::Composed)
		{
			outcome.PreparationStatus = KnownSpritePreparationResult::OccurrenceOutcome::Status::ReadySpline;
		}
		else if (preparation.PreparationStatus == SplineRenderInputPreparationResult::Status::StalePublication)
		{
			outcome.PreparationStatus = KnownSpritePreparationResult::OccurrenceOutcome::Status::PendingNativeSourceAcquisition;
		}
		else if (preparation.PreparationStatus == SplineRenderInputPreparationResult::Status::ProfilesNotComplete &&
			source.splineProfileStatus == SplineProfileAcquisitionStatus::Unavailable)
		{
			outcome.PreparationStatus = KnownSpritePreparationResult::OccurrenceOutcome::Status::TemporarilyUnavailableSource;
		}
		else if (preparation.PreparationStatus == SplineRenderInputPreparationResult::Status::ProfilesNotComplete &&
			(source.splineProfileStatus == SplineProfileAcquisitionStatus::Invalid ||
				source.splineProfileStatus == SplineProfileAcquisitionStatus::Unsupported))
		{
			outcome.PreparationStatus = KnownSpritePreparationResult::OccurrenceOutcome::Status::TerminalSourceFailure;
		}
		else if (preparation.PreparationStatus == SplineRenderInputPreparationResult::Status::SourceNotComplete ||
			preparation.PreparationStatus == SplineRenderInputPreparationResult::Status::ProfilesNotComplete)
		{
			outcome.PreparationStatus = KnownSpritePreparationResult::OccurrenceOutcome::Status::PendingNativeSourceAcquisition;
		}
		else
		{
			outcome.PreparationStatus = KnownSpritePreparationResult::OccurrenceOutcome::Status::UnresolvedInputs;
		}
		output.SplinePreparations.push_back(std::move(preparation));
	}

	/** Requires certified coverage and rejects all unresolved or temporarily unavailable occurrence outcomes. */
	SectionScheduler::SectionScenePreparationResult::Readiness
	SectionScheduler::DetermineSceneReadiness(const SectionScenePreparationResult& output)
	{
		using Readiness = SectionScenePreparationResult::Readiness;
		using Status = KnownSpritePreparationResult::OccurrenceOutcome::Status;
		if (!output.CandidateEvidence.candidateCoverageComplete ||
			!output.CandidateEvidence.membershipEnumerationComplete)
		{
			return Readiness::IncompleteCandidateCoverage;
		}
		if (!output.CandidateEvidence.nativeAcquisitionComplete ||
			output.CandidateEvidence.hasPendingOrUnresolvedOccurrences)
		{
			return Readiness::PendingNativeSourceAcquisition;
		}
		for (const auto& outcome : output.OccurrenceOutcomes)
		{
			switch (outcome.PreparationStatus)
			{
			case Status::PendingNativeSourceAcquisition:
				return Readiness::PendingNativeSourceAcquisition;
			case Status::PendingSpriteAcquisition:
				return Readiness::PendingSpriteAcquisition;
			case Status::TemporarilyUnavailableSource:
			case Status::TemporarilyUnavailableSprite:
				return Readiness::TemporarilyUnavailableResources;
			case Status::InvalidSpriteRequirement:
			case Status::NotProcessed:
			case Status::UnknownSource:
			case Status::MissingSpritePlacement:
			case Status::UnresolvedSpriteGeometry:
			case Status::UnknownSpritePass:
			case Status::UnresolvedInputs:
				return Readiness::UnresolvedInputs;
			case Status::TerminalSourceFailure:
			case Status::TerminalSpriteCaptureFailure:
			case Status::ReadySprite:
			case Status::ReadySpline:
				break;
			}
		}
		return Readiness::CompleteSceneInputs;
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

	/** Converts acquired keyed values without changing their coordinate basis or source status. */
	void SectionScheduler::PrepareSplineCurve(
		const BuildingCandidateSourceRecord& source,
		SplineCurvePreparationResult& output) const
	{
		output.SourceStatus = source.status;
		output.ProfileStatus = source.splineProfileStatus;
		output.Provenance = source.spline.provenance;
		output.CoordinateSpace = source.spline.coordinateSpace;
		output.ConversionIssue = Visuals::SplineNumericalIssue::None;
		output.PreparationStatus = SplineCurvePreparationResult::CurveStatus::NotPrepared;
		output.RootStatus = SplineCurvePreparationResult::RootTransformStatus::NotApplicable;
		output.RootTransform.reset();
		output.Segments.clear();
		if (source.kind != BuildingCandidateSourceKind::Spline)
		{
			output.PreparationStatus = SplineCurvePreparationResult::CurveStatus::NotSpline;
			return;
		}

		PrepareSplineRootTransform(source, output);
		if (!source.spline.hasKeyedCurve)
		{
			output.PreparationStatus = SplineCurvePreparationResult::CurveStatus::MissingKeyedCurve;
			return;
		}

		const Visuals::KeyedSplineCurve curve{
			std::span<const Visuals::KeyedSplinePoint>(source.spline.points),
			source.spline.isLooped};
		output.ConversionIssue = Visuals::SplineGeometry::ConvertKeyedCurve(curve, output.Segments);
		if (output.ConversionIssue != Visuals::SplineNumericalIssue::None)
		{
			return;
		}
		if (source.spline.coordinateSpace != BuildingCandidateSplineCoordinateSpace::WorldSpace)
		{
			output.PreparationStatus = SplineCurvePreparationResult::CurveStatus::UnsupportedCoordinateSpace;
			return;
		}
		output.PreparationStatus = SplineCurvePreparationResult::CurveStatus::ConvertedWorldSpace;
	}

	/** Validates copied placement values without normalizing or replacing missing data. */
	void SectionScheduler::PrepareSplineRootTransform(
		const BuildingCandidateSourceRecord& source,
		SplineCurvePreparationResult& output)
	{
		using RootStatus = SplineCurvePreparationResult::RootTransformStatus;
		output.RootStatus = RootStatus::MissingPlacement;
		if (!source.hasPlacement)
		{
			return;
		}

		Visuals::SplineSurfaceTransform rootTransform;
		for (std::size_t axis = 0; axis < 3; ++axis)
		{
			const double scale = source.placement.SignedScale[axis];
			if (!std::isfinite(source.placement.Position[axis]) || !std::isfinite(scale) || scale == 0.0 ||
				!std::isfinite(1.0 / scale))
			{
				output.RootStatus = RootStatus::InvalidPlacement;
				return;
			}
			rootTransform.Translation[axis] = source.placement.Position[axis];
			rootTransform.Scale[axis] = scale;
		}
		if (!CopySplineSurfaceRotation(source.placement.Rotation, rootTransform))
		{
			output.RootStatus = RootStatus::InvalidPlacement;
			return;
		}
		output.RootTransform = rootTransform;
		output.RootStatus = RootStatus::Prepared;
	}

	/** Checks publication currentness before reading and after composing one ordinal-bound spline input.
	 * The publication and policy are borrowed only during this call and are not retained in output.
	 */
	void SectionScheduler::ComposeSplineRenderInput(
		const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
		std::uint64_t publicationOrdinal,
		std::uint16_t resolvedPolicyIndex,
		const SplinePolicy* resolvedPolicy,
		SplineRenderInputPreparationResult& output) const
	{
		output.Reset();
		const BuildingCandidateSourceRecord* source = nullptr;
		if (publicationOwner == nullptr)
		{
			output.PreparationStatus = SplineRenderInputPreparationResult::Status::InvalidOccurrence;
			return;
		}
		if (!publicationOwner->IsCurrent())
		{
			output.PreparationStatus = SplineRenderInputPreparationResult::Status::StalePublication;
			return;
		}
		if (!publicationOwner->TryGetSourceRecord(publicationOrdinal, source) || source == nullptr)
		{
			output.PreparationStatus = SplineRenderInputPreparationResult::Status::InvalidOccurrence;
			return;
		}
		output.PublicationOrdinal = publicationOrdinal;
		output.SplinePolicyIndex = source->splinePolicyIndex;
		PrepareSplineCurve(*source, output.CurvePreparation);
		SplineProfilePreparationResult profilePreparation;
		PrepareSplineProfileAlternatives(*source, profilePreparation);
		output.Alternatives.reserve(profilePreparation.Alternatives.size());
		for (std::size_t alternativeIndex = 0;
			alternativeIndex < profilePreparation.Alternatives.size();
			++alternativeIndex)
		{
			SplineRenderInputPreparationResult::AlternativeOutcome outcome;
			outcome.SourceAlternativeIndex = alternativeIndex;
			outcome.PreparationStatus = profilePreparation.Alternatives[alternativeIndex].PreparationStatus;
			output.Alternatives.push_back(outcome);
		}
		if (!CheckSplineRenderInputPrerequisites(*source, resolvedPolicyIndex, resolvedPolicy, output))
		{
			return;
		}
		if (!AppendSplineRenderSurfaces(*resolvedPolicy, profilePreparation, output))
		{
			return;
		}
		if (!publicationOwner->IsCurrent())
		{
			output.PreparationStatus = SplineRenderInputPreparationResult::Status::StalePublication;
			return;
		}
		output.PreparationStatus = SplineRenderInputPreparationResult::Status::Composed;
	}

	/** Blocks composition until each independent source, transform, policy and scale requirement is valid. */
	bool SectionScheduler::CheckSplineRenderInputPrerequisites(
		const BuildingCandidateSourceRecord& source,
		std::uint16_t resolvedPolicyIndex,
		const SplinePolicy* resolvedPolicy,
		SplineRenderInputPreparationResult& output) const
	{
		return CheckSplineSourcePrerequisites(source, output) &&
			CheckSplinePolicyPrerequisites(source, resolvedPolicyIndex, resolvedPolicy, output) &&
			TryPrepareSplineSamplingPolicy(output);
	}

	/** Rejects incomplete acquisition or unsupported curve/root values without promoting status. */
	bool SectionScheduler::CheckSplineSourcePrerequisites(
		const BuildingCandidateSourceRecord& source,
		SplineRenderInputPreparationResult& output)
	{
		using Status = SplineRenderInputPreparationResult::Status;
		if (source.kind != BuildingCandidateSourceKind::Spline)
		{
			output.PreparationStatus = Status::NotSpline;
			return false;
		}
		if (source.status != BuildingCandidateSourceStatus::Complete)
		{
			output.PreparationStatus = Status::SourceNotComplete;
			return false;
		}
		if (output.CurvePreparation.ProfileStatus != SplineProfileAcquisitionStatus::Complete)
		{
			output.PreparationStatus = Status::ProfilesNotComplete;
			return false;
		}
		if (output.CurvePreparation.PreparationStatus != SplineCurvePreparationResult::CurveStatus::ConvertedWorldSpace ||
			output.CurvePreparation.ConversionIssue != Visuals::SplineNumericalIssue::None)
		{
			output.PreparationStatus = Status::CurveNotEligible;
			return false;
		}
		if (output.CurvePreparation.RootStatus != SplineCurvePreparationResult::RootTransformStatus::Prepared ||
			!output.CurvePreparation.RootTransform.has_value())
		{
			output.PreparationStatus = Status::RootTransformUnavailable;
			return false;
		}
		return true;
	}

	/** Requires the service-resolved policy to match the occurrence's nonzero source index. */
	bool SectionScheduler::CheckSplinePolicyPrerequisites(
		const BuildingCandidateSourceRecord& source,
		std::uint16_t resolvedPolicyIndex,
		const SplinePolicy* resolvedPolicy,
		SplineRenderInputPreparationResult& output)
	{
		using Status = SplineRenderInputPreparationResult::Status;
		if (source.splinePolicyIndex == 0 || resolvedPolicyIndex == 0 || resolvedPolicy == nullptr)
		{
			output.PreparationStatus = Status::PolicyUnresolved;
			return false;
		}
		if (resolvedPolicyIndex != source.splinePolicyIndex)
		{
			output.PreparationStatus = Status::PolicyIndexMismatch;
			return false;
		}
		if (resolvedPolicy->SolidRgb > 0xFFFFFF || !std::isfinite(resolvedPolicy->Opacity) ||
			resolvedPolicy->Opacity < 0.0 || resolvedPolicy->Opacity > 1.0)
		{
			output.PreparationStatus = Status::InvalidPolicy;
			return false;
		}
		return true;
	}

	/** Uses configured image resolution and leaves XY, height, depth and sample-limit defaults intact. */
	bool SectionScheduler::TryPrepareSplineSamplingPolicy(
		SplineRenderInputPreparationResult& output) const
	{
		using Status = SplineRenderInputPreparationResult::Status;
		if (!std::isfinite(sectionSizeMapUnits_) || sectionSizeMapUnits_ <= 0.0 ||
			sectionResolutionPixels_ == 0)
		{
			output.PreparationStatus = Status::InvalidSamplingDensity;
			return false;
		}
		const double pixelsPerUnit =
			static_cast<double>(sectionResolutionPixels_) / sectionSizeMapUnits_;
		if (!std::isfinite(pixelsPerUnit) || pixelsPerUnit <= 0.0)
		{
			output.PreparationStatus = Status::InvalidSamplingDensity;
			return false;
		}
		output.SamplingPolicy = Visuals::SplineSamplingPolicy{};
		output.SamplingPolicy.PixelsPerUnit = pixelsPerUnit;
		return true;
	}

	/** Moves only prepared surfaces and keeps their source alternative positions addressable. */
	bool SectionScheduler::AppendSplineRenderSurfaces(
		const SplinePolicy& policy,
		SplineProfilePreparationResult& profiles,
		SplineRenderInputPreparationResult& output)
	{
		using AlternativeStatus = SplineProfilePreparationResult::Alternative::Status;
		using Status = SplineRenderInputPreparationResult::Status;
		if (profiles.Alternatives.size() != output.Alternatives.size())
		{
			output.PreparationStatus = Status::MalformedPreparedSurface;
			return false;
		}
		for (const auto& alternative : profiles.Alternatives)
		{
			if (alternative.PreparationStatus == AlternativeStatus::PreparedNumericalInput &&
				!alternative.SurfaceInput.ComponentTransformOverride.has_value())
			{
				output.PreparationStatus = Status::MalformedPreparedSurface;
				return false;
			}
		}
		for (std::size_t alternativeIndex = 0;
			alternativeIndex < profiles.Alternatives.size();
			++alternativeIndex)
		{
			auto& alternative = profiles.Alternatives[alternativeIndex];
			if (alternative.PreparationStatus != AlternativeStatus::PreparedNumericalInput)
			{
				continue;
			}
			auto surface = std::move(alternative.SurfaceInput);
			surface.Appearance.Mode = Visuals::SplineSurfaceAppearanceMode::SolidColor;
			surface.Appearance.SolidRgb = policy.SolidRgb;
			surface.Appearance.Opacity = policy.Opacity;
			output.Alternatives[alternativeIndex].SurfaceIndex = output.SurfaceInputs.size();
			output.SurfaceInputs.push_back(std::move(surface));
		}
		if (output.SurfaceInputs.empty())
		{
			output.PreparationStatus = Status::NoPreparedSurface;
			return false;
		}
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
			PrepareSpriteOccurrence(candidate, publication, output);
		}
	}

	/** Resolves and prepares one Sprite occurrence while retaining its exact per-ordinal outcome. */
	void SectionScheduler::PrepareSpriteOccurrence(
		const BuildingCandidateOccurrence& candidate,
		const BuildingCandidatePublication& publication,
		KnownSpritePreparationResult& output)
	{
		auto outcome = std::find_if(
			output.OccurrenceOutcomes.begin(),
			output.OccurrenceOutcomes.end(),
			[&candidate](const auto& occurrence)
			{
				return occurrence.PublicationOrdinal == candidate.publicationOrdinal;
			});
		if (outcome == output.OccurrenceOutcomes.end())
		{
			++output.InvalidRequirementCount;
			return;
		}
		const BuildingCandidateSourceRecord* requirement = nullptr;
		if (!publication.TryGetSourceRecord(candidate.publicationOrdinal, requirement) || requirement == nullptr)
		{
			++output.InvalidRequirementCount;
			outcome->PreparationStatus = KnownSpritePreparationResult::OccurrenceOutcome::Status::InvalidSpriteRequirement;
			return;
		}
		if (requirement->kind != BuildingCandidateSourceKind::Sprite)
		{
			output.MayHaveUnknownRequirements = true;
			return;
		}
		RecordSpriteSourceUncertainty(*requirement, output);
		Cache::OwnedSpriteKey ownedKey;
		if (!Cache::OwnedSpriteKey::TryCopy(requirement->canonicalClassPath, ownedKey))
		{
			++output.InvalidRequirementCount;
			outcome->PreparationStatus = KnownSpritePreparationResult::OccurrenceOutcome::Status::InvalidSpriteRequirement;
			return;
		}
		PrepareSpriteRequirement(candidate.publicationOrdinal, *requirement, ownedKey, output, *outcome);
	}

	/** Records source uncertainty while preserving the existing cache-preparation path. */
	void SectionScheduler::RecordSpriteSourceUncertainty(
		const BuildingCandidateSourceRecord& requirement,
		KnownSpritePreparationResult& output)
	{
		if (requirement.status != BuildingCandidateSourceStatus::Complete ||
			requirement.spritePass == BuildingCandidateSpritePass::Unknown)
		{
			output.MayHaveUnknownRequirements = true;
		}
	}

	/** Preserves acquisition outcomes and borrows actual payload metadata only for placed ready sprites. */
	void SectionScheduler::PrepareSpriteRequirement(
		std::uint64_t publicationOrdinal,
		const BuildingCandidateSourceRecord& requirement,
		const Cache::OwnedSpriteKey& ownedKey,
		KnownSpritePreparationResult& output,
		KnownSpritePreparationResult::OccurrenceOutcome& occurrenceOutcome)
	{
		if (buildingScheduler_ == nullptr || cpuSpriteSourceManager_ == nullptr)
		{
			++output.UnavailableCount;
			occurrenceOutcome.PreparationStatus =
				KnownSpritePreparationResult::OccurrenceOutcome::Status::TemporarilyUnavailableSprite;
			return;
		}
		const BuildingScheduler::RequirementStatus readiness =
			buildingScheduler_->EnsureSpriteReady(ownedKey);
		if (readiness == BuildingScheduler::RequirementStatus::Pending)
		{
			++output.PendingCount;
			occurrenceOutcome.PreparationStatus =
				KnownSpritePreparationResult::OccurrenceOutcome::Status::PendingSpriteAcquisition;
			return;
		}
		if (readiness == BuildingScheduler::RequirementStatus::Failed)
		{
			++output.FailedCount;
			occurrenceOutcome.PreparationStatus =
				KnownSpritePreparationResult::OccurrenceOutcome::Status::TerminalSpriteCaptureFailure;
			return;
		}
		if (readiness == BuildingScheduler::RequirementStatus::Unavailable)
		{
			++output.UnavailableCount;
			occurrenceOutcome.PreparationStatus =
				KnownSpritePreparationResult::OccurrenceOutcome::Status::TemporarilyUnavailableSprite;
			return;
		}
		PrepareReadySpriteRequirement(
			publicationOrdinal,
			requirement,
			ownedKey,
			output,
			occurrenceOutcome);
	}

	/** Borrows the resident payload and appends an input only after its support has been established. */
	void SectionScheduler::PrepareReadySpriteRequirement(
		std::uint64_t publicationOrdinal,
		const BuildingCandidateSourceRecord& requirement,
		const Cache::OwnedSpriteKey& ownedKey,
		KnownSpritePreparationResult& output,
		KnownSpritePreparationResult::OccurrenceOutcome& occurrenceOutcome)
	{
		if (cpuSpriteSourceManager_ == nullptr)
		{
			++output.UnavailableCount;
			occurrenceOutcome.PreparationStatus =
				KnownSpritePreparationResult::OccurrenceOutcome::Status::TemporarilyUnavailableSprite;
			return;
		}
		const Cache::CpuSpriteSourceManager::SpriteResult spriteResult =
			cpuSpriteSourceManager_->GetSprite(ownedKey);
		if (spriteResult.Result != Cache::CpuSpriteSourceManager::Status::Ready ||
			spriteResult.Payload == nullptr ||
			spriteResult.Payload->Format() != Cache::ProcessedSpritePayload::PixelFormat::StraightRgba8SrgbDisplay)
		{
			++output.UnavailableCount;
		occurrenceOutcome.PreparationStatus =
			KnownSpritePreparationResult::OccurrenceOutcome::Status::TemporarilyUnavailableSprite;
			return;
		}
		if (!requirement.hasPlacement)
		{
			++output.ReadyWithoutPlacementCount;
			occurrenceOutcome.PreparationStatus =
				KnownSpritePreparationResult::OccurrenceOutcome::Status::MissingSpritePlacement;
			return;
		}
		AppendReadySpriteInput(publicationOrdinal, requirement, *spriteResult.Payload, output, occurrenceOutcome);
	}

	/** Converts an already acquired payload through the existing placement path and appends its input. */
	void SectionScheduler::AppendReadySpriteInput(
		std::uint64_t publicationOrdinal,
		const BuildingCandidateSourceRecord& requirement,
		const Cache::ProcessedSpritePayload& payload,
		KnownSpritePreparationResult& output,
		KnownSpritePreparationResult::OccurrenceOutcome& occurrenceOutcome)
	{
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
			occurrenceOutcome.PreparationStatus =
				KnownSpritePreparationResult::OccurrenceOutcome::Status::UnresolvedSpriteGeometry;
			return;
		}
		KnownSpritePreparationResult::PreparedSpriteOccurrence prepared;
		prepared.PublicationOrdinal = publicationOrdinal;
		prepared.SpritePass = requirement.spritePass;
		prepared.RenderInput = renderInput;
		prepared.Support = support;
		output.PreparedSprites.push_back(std::move(prepared));
		occurrenceOutcome.PreparationStatus =
			requirement.spritePass == BuildingCandidateSpritePass::Unknown
			? KnownSpritePreparationResult::OccurrenceOutcome::Status::UnknownSpritePass
			: KnownSpritePreparationResult::OccurrenceOutcome::Status::ReadySprite;
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
