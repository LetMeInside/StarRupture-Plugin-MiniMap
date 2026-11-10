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
#include <stdexcept>
#include <utility>

namespace Services::Map::Buildings::Scheduling
{
	/** Clears one owned result while preserving vector capacities for reuse. */
	void SectionScheduler::SplineRenderInputPreparationResult::Reset() noexcept
	{
		PublicationOrdinal = 0;
		SplinePolicyIndex = 0;
		GeometryMode = BuildingCandidateSplineGeometryMode::AuthoredProfiles;
		SyntheticThinRibbonHalfWidthMapUnits = 0.0;
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

	/** Clears scene-owned values without retaining publication spans or source data. */
	void SectionScheduler::SectionScenePreparationResult::Reset() noexcept
	{
		OccurrenceOutcomes.clear();
		OrdinarySprites.clear();
		SplinePreparations.clear();
		OverlayLastSprites.clear();
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
		TerminalSplineOmissions = {};
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
		if (publicationOwner == nullptr || PublicationIdentity != publicationOwner.get() ||
			PreparationReadiness != Readiness::CompleteSceneInputs)
		{
			return false;
		}
		scratch.reserve(SplinePreparations.size());
		for (const SplineRenderInputPreparationResult& splinePreparation : SplinePreparations)
		{
			const std::uint64_t ordinal = splinePreparation.PublicationOrdinal;
			if (ordinal < OccurrenceOutcomes.size() &&
				OccurrenceOutcomes[static_cast<std::size_t>(ordinal)].PublicationOrdinal == ordinal &&
				OccurrenceOutcomes[static_cast<std::size_t>(ordinal)].PreparationStatus ==
					KnownSpritePreparationResult::OccurrenceOutcome::Status::TerminalSourceFailure)
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
		std::stable_sort(
			scratch.begin(),
			scratch.end(),
			[](const Visuals::BuildingSplineRenderInput& left, const Visuals::BuildingSplineRenderInput& right)
			{
				const auto compareComponent = [](double leftValue, double rightValue)
				{
					const bool leftFinite = std::isfinite(leftValue);
					const bool rightFinite = std::isfinite(rightValue);
					if (leftFinite != rightFinite)
					{
						return leftFinite ? -1 : 1;
					}
					if (!leftFinite || leftValue == rightValue)
					{
						return 0;
					}
					return leftValue < rightValue ? -1 : 1;
				};
				const auto getElevation = [](const Visuals::BuildingSplineRenderInput& input)
					-> std::optional<double>
				{
					if (input.Segments.empty())
					{
						return std::nullopt;
					}
					const double startZ = input.Segments.front().Controls[0][2];
					const double endZ = input.Segments.back().Controls[3][2];
					if (!std::isfinite(startZ) || !std::isfinite(endZ))
					{
						return std::nullopt;
					}
					return (std::max)(startZ, endZ);
				};
				const std::optional<double> leftElevation = getElevation(left);
				const std::optional<double> rightElevation = getElevation(right);
				if (leftElevation.has_value() != rightElevation.has_value())
				{
					return leftElevation.has_value();
				}
				if (leftElevation.has_value())
				{
					const int elevationOrder = compareComponent(*leftElevation, *rightElevation);
					if (elevationOrder != 0)
					{
						return elevationOrder < 0;
					}
				}
				const int yOrder = compareComponent(
					left.RootTransform.Translation[1],
					right.RootTransform.Translation[1]);
				if (yOrder != 0)
				{
					return yOrder < 0;
				}
				return compareComponent(
					left.RootTransform.Translation[0],
					right.RootTransform.Translation[0]) < 0;
			});
		output.OrdinarySprites = std::span<const Visuals::BuildingSpriteRenderInput>(OrdinarySprites);
		output.Splines = std::span<const Visuals::BuildingSplineRenderInput>(scratch);
		output.OverlayLastSprites = std::span<const Visuals::BuildingSpriteRenderInput>(OverlayLastSprites);
		return true;
	}

	/** Completes zero-candidate or fully transparent Sections, otherwise adopts the complete rendered image. */
	void SectionScheduler::RenderAndCommitSectionImage(
		::Services::Map::Buildings::BuildingMapSection& section,
		const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
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
		if (publicationOwner->BorrowCandidateData().occurrences.empty())
		{
			CommitEmptySection(section, output);
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
			output.CommitStatus = SectionImageCommitResult::Status::InvalidOrIncompleteInput;
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
		output.Timing.OrdinarySpriteInputCount = sceneInput.OrdinarySprites.size();
		output.Timing.SplineContributorInputCount = sceneInput.Splines.size();
		output.Timing.OverlayLastSpriteInputCount = sceneInput.OverlayLastSprites.size();
		PhaseTimingScope numericalRenderingTiming{output.Timing.NumericalRenderingMs};
#endif
		output.NumericalStatus = RenderIntoSectionContext(
			section,
			sceneInput,
			grid,
			imageBytes,
			output.TerminalSplineOmissions
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			, output.Timing.NumericalPhases
#endif
			);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		numericalRenderingTiming.Finish();
#endif
		if (!IsCompletedRenderComplete(output))
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
			CommitEmptySection(section, output);
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
		const KnownSpritePreparationResult& knownSpritePreparation,
		std::span<const SplinePolicy> splinePolicies,
		SectionScenePreparationResult& output,
		SectionImageCommitResult& commitResult) const
	{
		if (publicationOwner == nullptr || !IsPublicationForSection(section, *publicationOwner))
		{
			output.Reset();
			output.PreparationReadiness = SectionScenePreparationResult::Readiness::InvalidSection;
			commitResult.SceneReadiness = output.PreparationReadiness;
			return false;
		}
		PrepareSectionSceneInputs(publicationOwner, knownSpritePreparation, splinePolicies, output);
		commitResult.SceneReadiness = output.PreparationReadiness;
		return publicationOwner != nullptr && output.PreparationReadiness ==
			SectionScenePreparationResult::Readiness::CompleteSceneInputs;
	}

	/** Preserves complete numerical evidence before output inspection or adoption. */
	bool SectionScheduler::IsCompletedRenderComplete(
		SectionImageCommitResult& commitResult) const
	{
		if (!commitResult.NumericalStatus.has_value() ||
			(*commitResult.NumericalStatus != Rendering::SectionRendering::Status::Complete &&
				*commitResult.NumericalStatus != Rendering::SectionRendering::Status::CompleteWithOmissions))
		{
			commitResult.CommitStatus = SectionImageCommitResult::Status::NumericalRenderingFailed;
			return false;
		}
		return true;
	}

	/** Completes an empty Section without adopting pixel bytes. */
	void SectionScheduler::CommitEmptySection(
		::Services::Map::Buildings::BuildingMapSection& section,
		SectionImageCommitResult& output) const
	{
		const bool completedImageRemoved = section.HasCompletedImage();
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
		std::vector<std::uint8_t>& imageBytes,
		Visuals::BuildingVisualsTerminalOmissionSummary& omissionSummary
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		, Visuals::BuildingVisualsPhaseTimings& phaseTimings
#endif
		)
	{
		Visuals::BuildingVisualsContext& context = section.GetOrCreateVisualsContext();
		return Rendering::SectionRendering::RenderSection(
			sceneInput,
			context,
			grid,
			imageBytes,
			omissionSummary
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			, phaseTimings
#endif
			);
	}


	/** Matches one publication rectangle to the supplied Section's grid coordinate. */
	bool SectionScheduler::IsPublicationForSection(
		const ::Services::Map::Buildings::BuildingMapSection& section,
		const BuildingCandidatePublication& publication) const
	{
		const double expectedX = static_cast<double>(section.GetGridX()) * sectionSizeMapUnits_;
		const double expectedY = static_cast<double>(section.GetGridY()) * sectionSizeMapUnits_;
		const BuildingCollectionRegion& region = publication.SectionRegion();
		const double right = region.xMapUnits + region.widthMapUnits;
		const double bottom = region.yMapUnits + region.heightMapUnits;
		return std::isfinite(region.xMapUnits) && std::isfinite(region.yMapUnits) &&
			std::isfinite(region.widthMapUnits) && std::isfinite(region.heightMapUnits) &&
			region.widthMapUnits > 0.0 && region.heightMapUnits > 0.0 &&
			std::isfinite(right) && std::isfinite(bottom) &&
			right > region.xMapUnits && bottom > region.yMapUnits &&
			region.widthMapUnits == sectionSizeMapUnits_ &&
			region.heightMapUnits == sectionSizeMapUnits_ &&
			IsSectionRegionCoordinateMatch(region.xMapUnits, expectedX, sectionSizeMapUnits_) &&
			IsSectionRegionCoordinateMatch(region.yMapUnits, expectedY, sectionSizeMapUnits_);
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
		const bool syntheticThinRibbon =
			GeometryMode == BuildingCandidateSplineGeometryMode::SyntheticThinRibbon;
		const bool validGeometryMode = syntheticThinRibbon ||
			GeometryMode == BuildingCandidateSplineGeometryMode::AuthoredProfiles;
		const bool validProfileStatus = syntheticThinRibbon
			? CurvePreparation.ProfileStatus == SplineProfileAcquisitionStatus::NotApplicable &&
				std::isfinite(SyntheticThinRibbonHalfWidthMapUnits) &&
				SyntheticThinRibbonHalfWidthMapUnits > 0.0 && SurfaceInputs.size() == 1
			: CurvePreparation.ProfileStatus == SplineProfileAcquisitionStatus::Complete;
		if (PreparationStatus != Status::Composed ||
			CurvePreparation.SourceStatus != BuildingCandidateSourceStatus::Complete ||
			!validGeometryMode || !validProfileStatus ||
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
		output.SectionLocalOrdinal = PublicationOrdinal;
		output.SamplingPolicy = SamplingPolicy;
		output.RootTransform = syntheticThinRibbon
			? Visuals::SplineSurfaceTransform{}
			: *CurvePreparation.RootTransform;
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

	/** Keeps a job when shared publication ownership matches and resets it otherwise. */
	void SectionScheduler::EnsureSectionJob(
		std::int64_t gridX,
		std::int64_t gridY,
		const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner)
	{
		if (publicationOwner == nullptr)
		{
			throw std::invalid_argument("A Section job requires a selected publication owner.");
		}
		const auto coordinate = std::make_pair(gridX, gridY);
		const auto existingJob = sectionJobs_.find(coordinate);
		if (existingJob == sectionJobs_.end())
		{
			sectionJobs_.emplace(coordinate, SectionJob{publicationOwner});
			return;
		}
		const auto& retainedOwner = existingJob->second.publicationOwner;
		const bool sameOwnership = !retainedOwner.owner_before(publicationOwner) &&
			!publicationOwner.owner_before(retainedOwner);
		if (!sameOwnership)
		{
			existingJob->second = SectionJob{publicationOwner};
		}
	}

	/** Erases the exact job key while leaving demand, resource owners and images intact. */
	void SectionScheduler::CancelSectionJob(std::int64_t gridX, std::int64_t gridY) noexcept
	{
		sectionJobs_.erase(std::make_pair(gridX, gridY));
	}

	/** Releases all retained publication owners without changing other scheduler state. */
	void SectionScheduler::ClearSectionJobs() noexcept
	{
		sectionJobs_.clear();
	}

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

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
	/** Returns the owned scheduler's completed acquisition interval without advancing work. */
	bool SectionScheduler::TakeSpriteAcquisitionDiagnosticsSnapshot(
		BuildingScheduler::DiagnosticsSnapshot& output) noexcept
	{
		return buildingScheduler_ != nullptr &&
			buildingScheduler_->TakeDiagnosticsSnapshot(output);
	}
#endif

	/** Processes exactly one Section publication without assigning a rendering pass. */
	void SectionScheduler::PrepareKnownSpriteRequirements(
		const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
		KnownSpritePreparationResult& output)
	{
		ResetKnownSpritePreparationResult(output);
		if (publicationOwner == nullptr)
		{
			return;
		}
		output.PublicationIdentity = publicationOwner.get();

		const BuildingCandidateData candidateData = publicationOwner->BorrowCandidateData();
		const BuildingCandidateSourceData sourceData = publicationOwner->BorrowSourceData();
		if (!InitializeKnownSpriteOccurrences(candidateData, sourceData, output))
		{
			output.PreparationStatus = KnownSpritePreparationResult::Status::InvalidPublication;
			return;
		}
		PrepareSpriteRequirementSpan(candidateData.occurrences, *publicationOwner, output);
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
		output.PendingCount = 0;
		output.FailedCount = 0;
		output.UnavailableCount = 0;
		output.ReadyWithoutPlacementCount = 0;
		output.UnresolvedSpriteGeometryCount = 0;
		output.InvalidRequirementCount = 0;
		output.OccurrenceOutcomes.clear();
		output.PreparedSprites.clear();
		output.PreparedSpriteIndicesByOrdinal.clear();
	}

	/** Copies aligned candidate/source facts and allocates empty sparse-sprite indices per ordinal. */
	bool SectionScheduler::InitializeKnownSpriteOccurrences(
		const BuildingCandidateData& candidateData,
		const BuildingCandidateSourceData& sourceData,
		KnownSpritePreparationResult& output)
	{
		if (sourceData.records.size() != candidateData.occurrences.size())
		{
			return false;
		}
		output.OccurrenceOutcomes.reserve(candidateData.occurrences.size());
		output.PreparedSpriteIndicesByOrdinal.resize(candidateData.occurrences.size());
		for (std::size_t index = 0; index < candidateData.occurrences.size(); ++index)
		{
			if (candidateData.occurrences[index].publicationOrdinal != index)
			{
				output.OccurrenceOutcomes.clear();
				output.PreparedSpriteIndicesByOrdinal.clear();
				return false;
			}
			KnownSpritePreparationResult::OccurrenceOutcome outcome;
			outcome.PublicationOrdinal = candidateData.occurrences[index].publicationOrdinal;
			outcome.SourceKind = sourceData.records[index].kind;
			outcome.SourceStatus = sourceData.records[index].status;
			outcome.SpritePass = sourceData.records[index].spritePass;
			outcome.NativeFactsComplete = candidateData.occurrences[index].nativeFactsComplete;
			outcome.IsPendingOrUnresolved = candidateData.occurrences[index].isPendingOrUnresolved;
			output.OccurrenceOutcomes.push_back(outcome);
		}
		return true;
	}

	/** Prepares ordered neutral inputs for every occurrence in one retained Section publication. */
	void SectionScheduler::PrepareSectionSceneInputs(
		const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
		const KnownSpritePreparationResult& knownSpritePreparation,
		std::span<const SplinePolicy> splinePolicies,
		SectionScenePreparationResult& output) const
	{
		output.Reset();
		if (!ValidateSectionPreparationInputs(
			publicationOwner,
			knownSpritePreparation,
			output.PreparationReadiness))
		{
			return;
		}
		output.PublicationIdentity = publicationOwner.get();
		const BuildingCandidateData candidateData = publicationOwner->BorrowCandidateData();
		output.OccurrenceOutcomes.reserve(candidateData.occurrences.size());
		for (const BuildingCandidateOccurrence& occurrence : candidateData.occurrences)
		{
			ProcessSectionOccurrence(
				publicationOwner,
				occurrence.publicationOrdinal,
				knownSpritePreparation,
				splinePolicies,
				output);
		}
		const SectionScenePreparationResult::Readiness readiness = DetermineSceneReadiness(output);
		output.PreparationReadiness = readiness;
		if (readiness == SectionScenePreparationResult::Readiness::CompleteSceneInputs)
		{
			const auto placementOrder = [](const Visuals::BuildingSpriteRenderInput& left,
				const Visuals::BuildingSpriteRenderInput& right)
			{
				if (left.Placement.Position[2] != right.Placement.Position[2])
				{
					return left.Placement.Position[2] < right.Placement.Position[2];
				}
				if (left.Placement.Position[1] != right.Placement.Position[1])
				{
					return left.Placement.Position[1] < right.Placement.Position[1];
				}
				return left.Placement.Position[0] < right.Placement.Position[0];
			};
			std::stable_sort(output.OrdinarySprites.begin(), output.OrdinarySprites.end(), placementOrder);
			std::stable_sort(output.OverlayLastSprites.begin(), output.OverlayLastSprites.end(), placementOrder);
		}
	}

	/** Preserves invalid publication and mismatched sprite-result outcomes before preparation. */
	bool SectionScheduler::ValidateSectionPreparationInputs(
		const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
		const KnownSpritePreparationResult& spritePreparation,
		SectionScenePreparationResult::Readiness& readiness) const
	{
		using Readiness = SectionScenePreparationResult::Readiness;
		if (publicationOwner == nullptr ||
			spritePreparation.PreparationStatus == KnownSpritePreparationResult::Status::InvalidPublication)
		{
			readiness = Readiness::InvalidPublication;
			return false;
		}
		if (spritePreparation.PublicationIdentity != publicationOwner.get() ||
			(spritePreparation.PreparationStatus != KnownSpritePreparationResult::Status::KnownRequirementsProcessed &&
				spritePreparation.PreparationStatus != KnownSpritePreparationResult::Status::InvalidSourceRequirement) ||
			!IsCompatibleSpritePreparation(*publicationOwner, spritePreparation))
		{
			readiness = Readiness::InvalidPreparationInputs;
			return false;
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
			sourceData.records.size() != candidateData.occurrences.size() ||
			spritePreparation.PreparedSpriteIndicesByOrdinal.size() != candidateData.occurrences.size())
		{
			return false;
		}
		for (std::size_t index = 0; index < candidateData.occurrences.size(); ++index)
		{
			if (candidateData.occurrences[index].publicationOrdinal != index ||
				spritePreparation.OccurrenceOutcomes[index].PublicationOrdinal !=
				candidateData.occurrences[index].publicationOrdinal)
			{
				return false;
			}
		}
		for (const auto& prepared : spritePreparation.PreparedSprites)
		{
			if (prepared.PublicationOrdinal >= sourceData.records.size())
			{
				return false;
			}
			const std::size_t index = static_cast<std::size_t>(prepared.PublicationOrdinal);
			if (sourceData.records[index].kind != BuildingCandidateSourceKind::Sprite ||
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

	/** Copies independent evidence; sprite status requires an in-range, ordinal-matching outcome. */
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
		if (ordinal < spritePreparation.OccurrenceOutcomes.size() &&
			spritePreparation.OccurrenceOutcomes[static_cast<std::size_t>(ordinal)].PublicationOrdinal == ordinal)
		{
			outcome.SpritePreparationStatus =
				spritePreparation.OccurrenceOutcomes[static_cast<std::size_t>(ordinal)].PreparationStatus;
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

	/** Reuses sprite results through checked ordinal and sparse-index correspondence.
	 * Missing outcomes remain invalid; missing/mismatched prepared sprites remain unresolved geometry.
	 */
	void SectionScheduler::ProcessPreparedSprite(
		std::uint64_t ordinal,
		const KnownSpritePreparationResult& spritePreparation,
		KnownSpritePreparationResult::OccurrenceOutcome& outcome,
		SectionScenePreparationResult& output)
	{
		using Status = KnownSpritePreparationResult::OccurrenceOutcome::Status;
		if (ordinal >= spritePreparation.OccurrenceOutcomes.size() ||
			spritePreparation.OccurrenceOutcomes[static_cast<std::size_t>(ordinal)].PublicationOrdinal != ordinal)
		{
			outcome.PreparationStatus = Status::InvalidSpriteRequirement;
			return;
		}
		const auto& result = spritePreparation.OccurrenceOutcomes[static_cast<std::size_t>(ordinal)];
		outcome.PreparationStatus = result.PreparationStatus;
		outcome.SpritePreparationStatus = result.PreparationStatus;
		if (outcome.PreparationStatus != Status::ReadySprite)
		{
			return;
		}
		if (ordinal >= spritePreparation.PreparedSpriteIndicesByOrdinal.size())
		{
			outcome.PreparationStatus = Status::UnresolvedSpriteGeometry;
			return;
		}
		const auto preparedIndex = spritePreparation.PreparedSpriteIndicesByOrdinal[static_cast<std::size_t>(ordinal)];
		if (!preparedIndex.has_value() || *preparedIndex >= spritePreparation.PreparedSprites.size() ||
			spritePreparation.PreparedSprites[*preparedIndex].PublicationOrdinal != ordinal)
		{
			outcome.PreparationStatus = Status::UnresolvedSpriteGeometry;
			return;
		}
		const auto* prepared = &spritePreparation.PreparedSprites[*preparedIndex];
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

	/** Completes only when every Section occurrence is ready or terminally omittable. */
	SectionScheduler::SectionScenePreparationResult::Readiness
	SectionScheduler::DetermineSceneReadiness(const SectionScenePreparationResult& output)
	{
		using Readiness = SectionScenePreparationResult::Readiness;
		using Status = KnownSpritePreparationResult::OccurrenceOutcome::Status;
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

	/** Composes one ordinal-bound spline input from a retained immutable Section publication.
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
		if (!publicationOwner->TryGetSourceRecord(publicationOrdinal, source) || source == nullptr)
		{
			output.PreparationStatus = SplineRenderInputPreparationResult::Status::InvalidOccurrence;
			return;
		}
		output.PublicationOrdinal = publicationOrdinal;
		output.SplinePolicyIndex = source->splinePolicyIndex;
		output.GeometryMode = source->splineGeometryMode;
		output.SyntheticThinRibbonHalfWidthMapUnits = source->syntheticThinRibbonHalfWidthMapUnits;
		PrepareSplineCurve(*source, output.CurvePreparation);
		SplineProfilePreparationResult profilePreparation;
		if (output.GeometryMode == BuildingCandidateSplineGeometryMode::AuthoredProfiles)
		{
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
		}
		if (!CheckSplineRenderInputPrerequisites(*source, resolvedPolicyIndex, resolvedPolicy, output))
		{
			return;
		}
		const bool surfacesPrepared = output.GeometryMode ==
			BuildingCandidateSplineGeometryMode::SyntheticThinRibbon
			? AppendSyntheticThinRibbon(*resolvedPolicy,
				output.SyntheticThinRibbonHalfWidthMapUnits, output)
			: AppendSplineRenderSurfaces(*resolvedPolicy, profilePreparation, output);
		if (!surfacesPrepared)
		{
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
		if (!CheckSplineGeometryIntentPrerequisites(source, output))
		{
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

	/** Requires mode-specific profile evidence without inferring geometry from NotApplicable. */
	bool SectionScheduler::CheckSplineGeometryIntentPrerequisites(
		const BuildingCandidateSourceRecord& source,
		SplineRenderInputPreparationResult& output)
	{
		using Status = SplineRenderInputPreparationResult::Status;
		if (source.splineGeometryMode == BuildingCandidateSplineGeometryMode::AuthoredProfiles)
		{
			if (output.CurvePreparation.ProfileStatus == SplineProfileAcquisitionStatus::NotApplicable)
			{
				output.PreparationStatus = Status::CurveNotEligible;
				return false;
			}
			if (output.CurvePreparation.ProfileStatus != SplineProfileAcquisitionStatus::Complete)
			{
				output.PreparationStatus = Status::ProfilesNotComplete;
				return false;
			}
			return true;
		}
		if (source.splineGeometryMode == BuildingCandidateSplineGeometryMode::SyntheticThinRibbon &&
			output.CurvePreparation.ProfileStatus == SplineProfileAcquisitionStatus::NotApplicable &&
			std::isfinite(output.SyntheticThinRibbonHalfWidthMapUnits) &&
			output.SyntheticThinRibbonHalfWidthMapUnits > 0.0)
		{
			return true;
		}
		output.PreparationStatus = Status::CurveNotEligible;
		return false;
	}

	/** Appends one finite synthetic ribbon with world-up orientation and policy appearance. */
	bool SectionScheduler::AppendSyntheticThinRibbon(
		const SplinePolicy& policy,
		double halfWidthMapUnits,
		SplineRenderInputPreparationResult& output)
	{
		if (!std::isfinite(halfWidthMapUnits) || halfWidthMapUnits <= 0.0)
		{
			output.PreparationStatus = SplineRenderInputPreparationResult::Status::CurveNotEligible;
			return false;
		}
		Visuals::BuildingSplineSurfaceRenderInput surface;
		surface.Bounds.Extent = {0.0, halfWidthMapUnits, 0.0};
		surface.CrossSection.Scale = {1.0, 1.0};
		surface.CrossSection.Offset = {0.0, 0.0};
		surface.CrossSection.Up = {0.0, 0.0, 1.0};
		surface.Appearance.Mode = Visuals::SplineSurfaceAppearanceMode::SolidColor;
		surface.Appearance.SolidRgb = policy.SolidRgb;
		surface.Appearance.Opacity = policy.Opacity;
		surface.ComponentTransformOverride.emplace();
		output.SurfaceInputs.push_back(std::move(surface));
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

	/** Prepares one Sprite through checked publication-ordinal indexing, counting mismatches as invalid. */
	void SectionScheduler::PrepareSpriteOccurrence(
		const BuildingCandidateOccurrence& candidate,
		const BuildingCandidatePublication& publication,
		KnownSpritePreparationResult& output)
	{
		if (candidate.publicationOrdinal >= output.OccurrenceOutcomes.size() ||
			output.OccurrenceOutcomes[static_cast<std::size_t>(candidate.publicationOrdinal)].PublicationOrdinal !=
				candidate.publicationOrdinal)
		{
			++output.InvalidRequirementCount;
			return;
		}
		auto* outcome = &output.OccurrenceOutcomes[static_cast<std::size_t>(candidate.publicationOrdinal)];
		const BuildingCandidateSourceRecord* requirement = nullptr;
		if (!publication.TryGetSourceRecord(candidate.publicationOrdinal, requirement) || requirement == nullptr)
		{
			++output.InvalidRequirementCount;
			outcome->PreparationStatus = KnownSpritePreparationResult::OccurrenceOutcome::Status::InvalidSpriteRequirement;
			return;
		}
		if (requirement->kind != BuildingCandidateSourceKind::Sprite)
		{
			return;
		}
		Cache::OwnedSpriteKey ownedKey;
		if (!Cache::OwnedSpriteKey::TryCopy(requirement->canonicalClassPath, ownedKey))
		{
			++output.InvalidRequirementCount;
			outcome->PreparationStatus = KnownSpritePreparationResult::OccurrenceOutcome::Status::InvalidSpriteRequirement;
			return;
		}
		PrepareSpriteRequirement(candidate.publicationOrdinal, *requirement, ownedKey, output, *outcome);
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

	/** Appends the existing placement input and records its sparse index, never a vector pointer.
	 * PrepareSpriteOccurrence validated the ordinal; initialization sized the mapping for this publication.
	 * Pixels remain borrowed only for the existing immediate synchronous consumption.
	 */
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
		output.PreparedSpriteIndicesByOrdinal[static_cast<std::size_t>(publicationOrdinal)] =
			output.PreparedSprites.size() - 1;
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

}

#endif // MODLOADER_CLIENT_BUILD
