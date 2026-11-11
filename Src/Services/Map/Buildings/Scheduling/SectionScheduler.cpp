#if defined(MODLOADER_CLIENT_BUILD)

// Owns acquisition resources and advances resumable Section-owned image construction.
#include "SectionScheduler.h"

#include "BuildingScheduler.h"
#include "../BuildingMapSection.h"
#include "../Cache/CpuSpriteSourceManager.h"
#include "../Cache/SpritePngCodec.h"
#include "../Cache/SpriteDiskCache.h"
#include "../../../Log.h"
#include "../Rendering/SectionRendering.h"
#include "../Visuals/SpriteGeometry.h"
#include "../Visuals/SplineGeometry.h"

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
#include <chrono>
#endif
#include <algorithm>
#include <cmath>
#include <exception>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
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

	/** Builds borrowed pass arrays for synchronous use or one-time retained-scene construction. */
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
		const ImageGrid grid =
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
		ExportCompletedSectionImage(section, std::span<const std::uint8_t>(imageBytes));
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		output.Timing.TransparencyCheckInvoked = true;
		PhaseTimingScope transparencyCheckTiming{output.Timing.TransparencyCheckMs};
#endif
		const bool imageTransparent = IsFullyTransparentRgba8Image(
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
		section.AdoptCompletedImage(
			sectionResolutionPixels_, sectionResolutionPixels_, std::move(imageBytes));
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		imageAdoptionTiming.Finish();
#endif
		output.CommitStatus = SectionImageCommitResult::Status::Committed;
	}

	/** Exports one finalized image without changing its owner or pixel allocation. */
	void SectionScheduler::ExportCompletedSectionImage(
		const ::Services::Map::Buildings::BuildingMapSection& section,
		std::span<const std::uint8_t> rgba8Bytes) const noexcept
	{
		const std::int64_t gridX = section.GetGridX();
		const std::int64_t gridY = section.GetGridY();
		try
		{
			std::filesystem::create_directories(diagnosticsDirectory_);
			std::vector<std::uint8_t> pngBytes;
			const auto encoded = Cache::SpritePngCodec::Encode(
				rgba8Bytes,
				sectionResolutionPixels_,
				sectionResolutionPixels_,
				pngBytes);
			if (encoded.Status != Cache::SpritePngCodec::ResultStatus::Complete)
			{
				throw std::runtime_error("PNG encoding failed");
			}
			const std::string fileName = "Section_" + std::to_string(gridX) + "_" +
				std::to_string(gridY) + ".png";
			std::ofstream output(
				diagnosticsDirectory_ / fileName,
				std::ios::binary | std::ios::trunc);
			output.write(
				reinterpret_cast<const char*>(pngBytes.data()),
				static_cast<std::streamsize>(pngBytes.size()));
			output.close();
			if (!output)
			{
				throw std::runtime_error("PNG file open, write, or close failed");
			}
		}
		catch (const std::exception& exception)
		{
			if (MiniMap::Services::ILog* log = MiniMap::Services::TryLog())
			{
				MiniMap::Services::Logf(
					*log,
					MiniMap::Services::Level::Error,
					"MiniMap: Section PNG export failed for (%lld, %lld): %s",
					static_cast<long long>(gridX),
					static_cast<long long>(gridY),
					exception.what());
			}
		}
		catch (...)
		{
			if (MiniMap::Services::ILog* log = MiniMap::Services::TryLog())
			{
				MiniMap::Services::Logf(
					*log,
					MiniMap::Services::Level::Error,
					"MiniMap: Section PNG export failed for (%lld, %lld): unknown exception",
					static_cast<long long>(gridX),
					static_cast<long long>(gridY));
			}
		}
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

	/** Retains one complete source-composed scene and builds stable render spans after ownership settles. */
	SectionScheduler::SectionScenePreparationResult::Readiness
		SectionScheduler::PrepareRetainedSectionScene(
			const ::Services::Map::Buildings::BuildingMapSection& section,
			SectionJob& job,
			std::span<const SplinePolicy> splinePolicies,
			std::chrono::steady_clock::time_point deadline,
			bool& deadlineDeferred)
	{
		using Readiness = SectionScenePreparationResult::Readiness;
		deadlineDeferred = false;
		if (job.Preparation.TerminalReadiness.has_value())
		{
			return *job.Preparation.TerminalReadiness;
		}
		if (job.publicationOwner == nullptr)
		{
			job.Preparation.TerminalReadiness = Readiness::InvalidPublication;
			return Readiness::InvalidPublication;
		}
		if (job.retainedScene != nullptr)
		{
			const Readiness readiness = job.retainedScene->Preparation.PublicationIdentity == job.publicationOwner.get() &&
				job.retainedScene->Preparation.PreparationReadiness == Readiness::CompleteSceneInputs
				? Readiness::CompleteSceneInputs
				: Readiness::InvalidPreparationInputs;
			if (readiness != Readiness::CompleteSceneInputs)
			{
				job.Preparation.TerminalReadiness = readiness;
			}
			return readiness;
		}
		if (!job.Preparation.Initialized && !IsPublicationForSection(section, *job.publicationOwner))
		{
			job.Preparation.TerminalReadiness = Readiness::InvalidSection;
			return Readiness::InvalidSection;
		}
		if (!InitializeSectionPreparationProgress(job, job.publicationOwner))
		{
			job.Preparation.TerminalReadiness = Readiness::InvalidPublication;
			return Readiness::InvalidPublication;
		}
		deadlineDeferred = AdvanceSectionOccurrencePreparation(
			job,
			job.publicationOwner,
			splinePolicies,
			deadline);
		const Readiness readiness = GetSectionPreparationReadiness(job);
		if (readiness != Readiness::CompleteSceneInputs)
		{
			return readiness;
		}
		if (std::chrono::steady_clock::now() >= deadline)
		{
			return readiness;
		}
		if (!FinalizeRetainedSectionScene(job, job.publicationOwner))
		{
			job.Preparation.TerminalReadiness = Readiness::InvalidPreparationInputs;
			return Readiness::InvalidPreparationInputs;
		}
		return Readiness::CompleteSceneInputs;
	}

	/** Initializes per-ordinal evidence once for the exact selected publication owner. */
	bool SectionScheduler::InitializeSectionPreparationProgress(
		SectionJob& job,
		const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner)
	{
		if (job.Preparation.Initialized)
		{
			return job.Preparation.SpritePreparation.PublicationIdentity == publicationOwner.get();
		}
		if (publicationOwner == nullptr)
		{
			return false;
		}
		const BuildingCandidateData candidateData = publicationOwner->BorrowCandidateData();
		const BuildingCandidateSourceData sourceData = publicationOwner->BorrowSourceData();
		auto& preparation = job.Preparation;
		if (!InitializeKnownSpriteOccurrences(candidateData, sourceData, preparation.SpritePreparation))
		{
			preparation.SpritePreparation.PreparationStatus = KnownSpritePreparationResult::Status::InvalidPublication;
			return false;
		}
		preparation.SpritePreparation.PublicationIdentity = publicationOwner.get();
		preparation.SpritePreparation.PreparationStatus =
			KnownSpritePreparationResult::Status::KnownRequirementsProcessed;
		preparation.Occurrences.resize(candidateData.occurrences.size());
		preparation.Initialized = true;
		return true;
	}

	/** Advances untouched ordinals once and visits only sprite resources still unavailable. */
	bool SectionScheduler::AdvanceSectionOccurrencePreparation(
		SectionJob& job,
		const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
		std::span<const SplinePolicy> splinePolicies,
		std::chrono::steady_clock::time_point deadline)
	{
		auto& preparation = job.Preparation;
		const std::size_t retryCountAtStart = preparation.SpriteRetryCount;
		const BuildingCandidateData candidateData = publicationOwner->BorrowCandidateData();
		const BuildingCandidateSourceData sourceData = publicationOwner->BorrowSourceData();
		while (preparation.NextOccurrenceOrdinal < preparation.Occurrences.size() &&
			std::chrono::steady_clock::now() < deadline)
		{
			const std::size_t ordinal = preparation.NextOccurrenceOrdinal;
			++preparation.NextOccurrenceOrdinal;
			PrepareSectionOccurrence(
				job,
				*publicationOwner,
				candidateData.occurrences[ordinal],
				sourceData.records[ordinal],
				ordinal,
				splinePolicies);
		}
		bool deadlineDeferred = preparation.NextOccurrenceOrdinal < preparation.Occurrences.size();
		for (std::size_t retryIndex = 0; retryIndex < retryCountAtStart; ++retryIndex)
		{
			if (std::chrono::steady_clock::now() >= deadline)
			{
				deadlineDeferred = true;
				break;
			}
			std::size_t ordinal = 0;
			PopSectionSpriteRetry(job, ordinal);
			RetrySectionSpriteResource(job, sourceData.records[ordinal], ordinal);
		}
		return deadlineDeferred;
	}

	/** Classifies and prepares one occurrence without revisiting its completed actions. */
	void SectionScheduler::PrepareSectionOccurrence(
		SectionJob& job,
		const BuildingCandidatePublication& publication,
		const BuildingCandidateOccurrence& candidate,
		const BuildingCandidateSourceRecord& source,
		std::size_t ordinal,
		std::span<const SplinePolicy> splinePolicies)
	{
		auto& progress = job.Preparation;
		auto& outcome = progress.SpritePreparation.OccurrenceOutcomes[ordinal];
		const bool sourceBlocked = ProcessTerminalOrPendingSource(source, candidate, outcome);
		if (source.kind == BuildingCandidateSourceKind::Spline)
		{
			progress.SplinePreparations.emplace_back();
			PrepareSplineOccurrence(
				job.publicationOwner,
				ordinal,
				source,
				splinePolicies,
				outcome,
				progress.SplinePreparations.back());
		}
		else if (source.kind == BuildingCandidateSourceKind::Sprite)
		{
			PrepareSectionSpriteOccurrence(job, publication, source, ordinal, sourceBlocked);
		}
		SetSectionOccurrenceStatus(job, ordinal, outcome.PreparationStatus);
	}

	/** Validates one canonical key once and retains it only for this occurrence's generation. */
	void SectionScheduler::PrepareSectionSpriteOccurrence(
		SectionJob& job,
		const BuildingCandidatePublication& publication,
		const BuildingCandidateSourceRecord& source,
		std::size_t ordinal,
		bool sourcePreparationBlocked)
	{
		auto& preparation = job.Preparation;
		auto& outcome = preparation.SpritePreparation.OccurrenceOutcomes[ordinal];
		const auto sceneStatus = outcome.PreparationStatus;
		const BuildingCandidateSourceRecord* validatedSource = nullptr;
		Cache::OwnedSpriteKey spriteKey;
		if (!publication.TryGetSourceRecord(ordinal, validatedSource) || validatedSource == nullptr ||
			source.kind != BuildingCandidateSourceKind::Sprite ||
			validatedSource->kind != source.kind ||
			!Cache::OwnedSpriteKey::TryCopy(validatedSource->canonicalClassPath, spriteKey))
		{
			const auto spriteStatus = KnownSpritePreparationResult::OccurrenceOutcome::Status::InvalidSpriteRequirement;
			outcome.PreparationStatus = sourcePreparationBlocked ? sceneStatus : spriteStatus;
			outcome.SpritePreparationStatus = spriteStatus;
			return;
		}
		preparation.Occurrences[ordinal].SpriteKey = std::move(spriteKey);
		PrepareSpriteRequirement(
			ordinal,
			source,
			*preparation.Occurrences[ordinal].SpriteKey,
			preparation.SpritePreparation,
			outcome,
			false);
		const auto spriteStatus = outcome.PreparationStatus;
		outcome.SpritePreparationStatus = spriteStatus;
		if (sourcePreparationBlocked)
		{
			outcome.PreparationStatus = sceneStatus;
		}
		if (IsSpriteResourceRetryStatus(spriteStatus) && IsSectionOccurrenceBlockingStatus(sceneStatus))
		{
			QueueSectionSpriteRetry(job, ordinal);
		}
		else if (spriteStatus != KnownSpritePreparationResult::OccurrenceOutcome::Status::ReadySprite &&
			spriteStatus != KnownSpritePreparationResult::OccurrenceOutcome::Status::UnknownSpritePass)
		{
			preparation.Occurrences[ordinal].SpriteKey.reset();
		}
	}

	/** Retries only the validated resource lookup for one queued sprite occurrence. */
	void SectionScheduler::RetrySectionSpriteResource(
		SectionJob& job,
		const BuildingCandidateSourceRecord& source,
		std::size_t ordinal)
	{
		auto& progress = job.Preparation;
		auto& occurrence = progress.Occurrences[ordinal];
		auto& outcome = progress.SpritePreparation.OccurrenceOutcomes[ordinal];
		const bool sourceBlocked = outcome.PreparationStatus != outcome.SpritePreparationStatus;
		const auto sceneStatus = outcome.PreparationStatus;
		PrepareSpriteRequirement(
			ordinal,
			source,
			*occurrence.SpriteKey,
			progress.SpritePreparation,
			outcome,
			false);
		const auto spriteStatus = outcome.PreparationStatus;
		outcome.SpritePreparationStatus = spriteStatus;
		if (sourceBlocked)
		{
			outcome.PreparationStatus = sceneStatus;
		}
		SetSectionOccurrenceStatus(job, ordinal, outcome.PreparationStatus);
		if (IsSpriteResourceRetryStatus(spriteStatus))
		{
			QueueSectionSpriteRetry(job, ordinal);
		}
		else if (spriteStatus != KnownSpritePreparationResult::OccurrenceOutcome::Status::ReadySprite &&
			spriteStatus != KnownSpritePreparationResult::OccurrenceOutcome::Status::UnknownSpritePass)
		{
			occurrence.SpriteKey.reset();
		}
	}

	/** Stores one outcome and skips each newly resolved ordinal at most once. */
	void SectionScheduler::SetSectionOccurrenceStatus(
		SectionJob& job,
		std::size_t ordinal,
		KnownSpritePreparationResult::OccurrenceOutcome::Status status)
	{
		auto& preparation = job.Preparation;
		preparation.SpritePreparation.OccurrenceOutcomes[ordinal].PreparationStatus = status;
		if (ordinal != preparation.FirstUnresolvedOrdinal)
		{
			return;
		}
		while (preparation.FirstUnresolvedOrdinal < preparation.NextOccurrenceOrdinal)
		{
			const auto currentStatus = preparation.SpritePreparation.OccurrenceOutcomes[
				preparation.FirstUnresolvedOrdinal].PreparationStatus;
			if (IsSectionOccurrenceBlockingStatus(currentStatus))
			{
				break;
			}
			++preparation.FirstUnresolvedOrdinal;
		}
	}

	/** Appends one pending sprite ordinal to the intrusive retry queue. */
	void SectionScheduler::QueueSectionSpriteRetry(SectionJob& job, std::size_t ordinal)
	{
		auto& preparation = job.Preparation;
		auto& occurrence = preparation.Occurrences[ordinal];
		occurrence.NextSpriteRetryOrdinal.reset();
		if (preparation.SpriteRetryTail.has_value())
		{
			preparation.Occurrences[*preparation.SpriteRetryTail].NextSpriteRetryOrdinal = ordinal;
		}
		else
		{
			preparation.SpriteRetryHead = ordinal;
		}
		preparation.SpriteRetryTail = ordinal;
		++preparation.SpriteRetryCount;
	}

	/** Pops the next sprite-resource retry ordinal without scanning other occurrence state. */
	void SectionScheduler::PopSectionSpriteRetry(SectionJob& job, std::size_t& ordinal) noexcept
	{
		auto& preparation = job.Preparation;
		ordinal = *preparation.SpriteRetryHead;
		auto& occurrence = preparation.Occurrences[ordinal];
		preparation.SpriteRetryHead = occurrence.NextSpriteRetryOrdinal;
		if (!preparation.SpriteRetryHead.has_value())
		{
			preparation.SpriteRetryTail.reset();
		}
		occurrence.NextSpriteRetryOrdinal.reset();
		--preparation.SpriteRetryCount;
	}

	/** Returns the existing readiness value of the lowest-ordinal unresolved occurrence. */
	SectionScheduler::SectionScenePreparationResult::Readiness
		SectionScheduler::GetSectionPreparationReadiness(const SectionJob& job) const noexcept
	{
		using Readiness = SectionScenePreparationResult::Readiness;
		using Status = KnownSpritePreparationResult::OccurrenceOutcome::Status;
		if (!job.Preparation.Initialized)
		{
			return Readiness::InvalidPublication;
		}
		if (job.Preparation.NextOccurrenceOrdinal < job.Preparation.Occurrences.size())
		{
			return Readiness::UnresolvedInputs;
		}
		if (job.Preparation.FirstUnresolvedOrdinal >= job.Preparation.NextOccurrenceOrdinal)
		{
			return Readiness::CompleteSceneInputs;
		}
		const Status status = job.Preparation.SpritePreparation.OccurrenceOutcomes[
			job.Preparation.FirstUnresolvedOrdinal].PreparationStatus;
		if (status == Status::PendingNativeSourceAcquisition)
		{
			return Readiness::PendingNativeSourceAcquisition;
		}
		if (status == Status::PendingSpriteAcquisition)
		{
			return Readiness::PendingSpriteAcquisition;
		}
		if (status == Status::TemporarilyUnavailableSource || status == Status::TemporarilyUnavailableSprite)
		{
			return Readiness::TemporarilyUnavailableResources;
		}
		return Readiness::UnresolvedInputs;
	}

	/** Identifies only sprite-resource states that can change under the same publication. */
	bool SectionScheduler::IsSpriteResourceRetryStatus(
		KnownSpritePreparationResult::OccurrenceOutcome::Status status) noexcept
	{
		using Status = KnownSpritePreparationResult::OccurrenceOutcome::Status;
		return status == Status::PendingSpriteAcquisition || status == Status::TemporarilyUnavailableSprite;
	}

	/** Applies the established scene-readiness blocking policy to one occurrence status. */
	bool SectionScheduler::IsSectionOccurrenceBlockingStatus(
		KnownSpritePreparationResult::OccurrenceOutcome::Status status) noexcept
	{
		using Status = KnownSpritePreparationResult::OccurrenceOutcome::Status;
		return status != Status::TerminalSourceFailure && status != Status::TerminalSpriteCaptureFailure &&
			status != Status::ReadySprite && status != Status::ReadySpline;
	}

	/** Assembles complete ordinal results into the retained scene and transfers payload ownership once. */
	bool SectionScheduler::FinalizeRetainedSectionScene(
		SectionJob& job,
		const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner)
	{
		auto& progress = job.Preparation;
		auto& spritePreparation = progress.SpritePreparation;
		SectionScenePreparationResult preparation;
		preparation.PublicationIdentity = publicationOwner.get();
		preparation.PreparationReadiness = SectionScenePreparationResult::Readiness::CompleteSceneInputs;
		if (publicationOwner == nullptr ||
			spritePreparation.PublicationIdentity != publicationOwner.get() ||
			!progress.Initialized ||
			progress.NextOccurrenceOrdinal != progress.Occurrences.size() ||
			spritePreparation.OccurrenceOutcomes.size() != progress.Occurrences.size())
		{
			return false;
		}
		std::vector<std::size_t> preparedOrder;
		if (!CollectReadySpriteOrder(job, preparedOrder))
		{
			return false;
		}
		preparation.OccurrenceOutcomes = std::move(spritePreparation.OccurrenceOutcomes);
		preparation.SplinePreparations = std::move(progress.SplinePreparations);
		SortPreparedSpriteOrder(spritePreparation, preparedOrder);
		auto retained = std::make_unique<RetainedSectionScene>();
		if (!MoveOrderedSpriteInputs(job, preparedOrder, *retained, preparation))
		{
			return false;
		}
		preparation.PreparationReadiness = SectionScenePreparationResult::Readiness::CompleteSceneInputs;
		if (!HasOnlyRetainableSplineAppearances(preparation))
		{
			return false;
		}
		retained->Preparation = std::move(preparation);
		if (!InitializeRetainedSceneInputs(*retained, publicationOwner))
		{
			return false;
		}
		job.retainedScene = std::move(retained);
		job.Preparation = {};
		return true;
	}

	/** Collects every ready sprite exactly once while checking its direct ordinal slot. */
	bool SectionScheduler::CollectReadySpriteOrder(
		const SectionJob& job,
		std::vector<std::size_t>& output) const
	{
		const auto& progress = job.Preparation;
		const auto& outcomes = progress.SpritePreparation.OccurrenceOutcomes;
		const auto& preparedSprites = progress.SpritePreparation.PreparedSprites;
		const auto& preparedIndices = progress.SpritePreparation.PreparedSpriteIndicesByOrdinal;
		output.clear();
		output.reserve(preparedSprites.size());
		for (std::size_t ordinal = 0; ordinal < outcomes.size(); ++ordinal)
		{
			if (outcomes[ordinal].PreparationStatus !=
				KnownSpritePreparationResult::OccurrenceOutcome::Status::ReadySprite)
			{
				continue;
			}
			if (ordinal >= preparedIndices.size() || !preparedIndices[ordinal].has_value())
			{
				return false;
			}
			const std::size_t preparedIndex = *preparedIndices[ordinal];
			if (preparedIndex >= preparedSprites.size() ||
				preparedSprites[preparedIndex].PublicationOrdinal != ordinal)
			{
				return false;
			}
			output.push_back(preparedIndex);
		}
		return true;
	}

	/** Sorts prepared occurrences with the established stable Z/Y/X painter comparator. */
	void SectionScheduler::SortPreparedSpriteOrder(
		const KnownSpritePreparationResult& preparation,
		std::vector<std::size_t>& order) const
	{
		std::stable_sort(order.begin(), order.end(), [&preparation](std::size_t leftIndex, std::size_t rightIndex)
		{
			const auto& left = preparation.PreparedSprites[leftIndex].RenderInput.Placement.Position;
			const auto& right = preparation.PreparedSprites[rightIndex].RenderInput.Placement.Position;
			if (left[2] != right[2])
			{
				return left[2] < right[2];
			}
			if (left[1] != right[1])
			{
				return left[1] < right[1];
			}
			return left[0] < right[0];
		});
	}

	/** Moves sorted sprite views and their validated keys/payload owners into matching retained passes. */
	bool SectionScheduler::MoveOrderedSpriteInputs(
		SectionJob& job,
		const std::vector<std::size_t>& preparedOrder,
		RetainedSectionScene& retained,
		SectionScenePreparationResult& preparation)
	{
		auto& progress = job.Preparation;
		auto& preparedSprites = progress.SpritePreparation.PreparedSprites;
		const BuildingCandidateSourceData sourceData = job.publicationOwner->BorrowSourceData();
		for (const std::size_t preparedIndex : preparedOrder)
		{
			auto& prepared = preparedSprites[preparedIndex];
			const std::size_t ordinal = static_cast<std::size_t>(prepared.PublicationOrdinal);
			auto& occurrence = progress.Occurrences[ordinal];
			const auto& source = sourceData.records[ordinal];
			if (ordinal >= preparation.OccurrenceOutcomes.size() ||
				preparation.OccurrenceOutcomes[ordinal].PublicationOrdinal != ordinal ||
				preparation.OccurrenceOutcomes[ordinal].PreparationStatus !=
					KnownSpritePreparationResult::OccurrenceOutcome::Status::ReadySprite ||
				preparation.OccurrenceOutcomes[ordinal].SpritePass != prepared.SpritePass ||
				!occurrence.SpriteKey.has_value() || prepared.Payload == nullptr ||
				source.kind != BuildingCandidateSourceKind::Sprite ||
				source.spritePass != prepared.SpritePass ||
				prepared.RenderInput.Pixels.empty() ||
				prepared.RenderInput.Pixels.data() != prepared.Payload->Pixels().data())
			{
				return false;
			}
			RetainedSpriteRequirement retainedSprite;
			retainedSprite.PublicationOrdinal = prepared.PublicationOrdinal;
			retainedSprite.SpritePass = prepared.SpritePass;
			retainedSprite.SpriteKey = std::move(*occurrence.SpriteKey);
			retainedSprite.Payload = std::move(prepared.Payload);
			if (retainedSprite.SpritePass == BuildingCandidateSpritePass::FirstPass)
			{
				preparation.OrdinarySprites.push_back(prepared.RenderInput);
				retained.OrdinarySprites.push_back(std::move(retainedSprite));
			}
			else if (retainedSprite.SpritePass == BuildingCandidateSpritePass::SecondPass)
			{
				preparation.OverlayLastSprites.push_back(prepared.RenderInput);
				retained.OverlayLastSprites.push_back(std::move(retainedSprite));
			}
			else
			{
				return false;
			}
		}
		return true;
	}

	/** Creates the one sorted spline descriptor array and stable spans into retained scene storage. */
	bool SectionScheduler::InitializeRetainedSceneInputs(
		RetainedSectionScene& retained,
		const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner)
	{
		std::vector<Visuals::BuildingSplineRenderInput> splineDescriptors;
		Visuals::BuildingRenderSceneInput validatedScene;
		if (!retained.Preparation.BorrowRenderSceneInput(
			publicationOwner, splineDescriptors, validatedScene))
		{
			return false;
		}
		retained.Splines = std::move(splineDescriptors);
		retained.RenderSceneInput.OrdinarySprites = std::span<const Visuals::BuildingSpriteRenderInput>(
			retained.Preparation.OrdinarySprites);
		retained.RenderSceneInput.Splines = std::span<const Visuals::BuildingSplineRenderInput>(retained.Splines);
		retained.RenderSceneInput.OverlayLastSprites = std::span<const Visuals::BuildingSpriteRenderInput>(
			retained.Preparation.OverlayLastSprites);
		return true;
	}

	/** Restricts retained spline appearances to current owned solid values and empty texture views. */
	bool SectionScheduler::HasOnlyRetainableSplineAppearances(
		const SectionScenePreparationResult& preparation) noexcept
	{
		for (const SplineRenderInputPreparationResult& spline : preparation.SplinePreparations)
		{
			for (const Visuals::BuildingSplineSurfaceRenderInput& surface : spline.SurfaceInputs)
			{
				if (surface.Appearance.Mode != Visuals::SplineSurfaceAppearanceMode::SolidColor ||
					!surface.Appearance.Texture.Pixels.empty())
				{
					return false;
				}
			}
		}
		return true;
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

	/** Classifies finalized RGBA8 transparency by inspecting only each trusted pixel alpha byte. */
	bool SectionScheduler::IsFullyTransparentRgba8Image(
		std::span<const std::uint8_t> imageBytes) const noexcept
	{
		constexpr std::size_t bytesPerPixel = 4;
		constexpr std::size_t alphaOffset = 3;
		const std::size_t pixelCount = std::size_t(sectionResolutionPixels_) * sectionResolutionPixels_;
		for (std::size_t pixelIndex = 0; pixelIndex < pixelCount; ++pixelIndex)
		{
			const std::size_t alphaByteOffset = pixelIndex * bytesPerPixel + alphaOffset;
			if (imageBytes[alphaByteOffset] != 0)
			{
				return false;
			}
		}
		return true;
	}

	/** Renders into the numerical context exclusively owned by the supplied Section. */
	Rendering::SectionRendering::Status SectionScheduler::RenderIntoSectionContext(
		::Services::Map::Buildings::BuildingMapSection& section,
		const Visuals::BuildingRenderSceneInput& sceneInput,
		const ImageGrid& grid,
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
	ImageGrid SectionScheduler::CreateSectionRenderingGrid(
		const ::Services::Map::Buildings::BuildingMapSection& section,
		double sectionSizeMapUnits,
		std::uint32_t sectionResolutionPixels)
	{
		ImageGrid grid;
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
		, diagnosticsDirectory_(buildingsCacheDirectory / "Diagnostics")
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

	/** Keeps the existing owner for equivalent Section content and resets genuine changes. */
	std::shared_ptr<const BuildingCandidatePublication> SectionScheduler::EnsureSectionJob(
		::Services::Map::Buildings::BuildingMapSection& section,
		const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
		bool& generationReplaced)
	{
		generationReplaced = false;
		if (publicationOwner == nullptr)
		{
			throw std::invalid_argument("A Section job requires a selected publication owner.");
		}
		if (!section.renderingState_)
		{
			section.renderingState_ = std::make_shared<SectionJob>(SectionJob{publicationOwner});
			return publicationOwner;
		}
		SectionJob& job = *static_cast<SectionJob*>(section.renderingState_.get());
		const auto& retainedOwner = job.publicationOwner;
		const bool sameOwnership = retainedOwner.get() == publicationOwner.get() &&
			!retainedOwner.owner_before(publicationOwner) &&
			!publicationOwner.owner_before(retainedOwner);
		if (!sameOwnership)
		{
			if (retainedOwner->ContentHash() == publicationOwner->ContentHash())
			{
				return retainedOwner;
			}
			ResetSectionJobProgress(job);
			job.publicationOwner = publicationOwner;
			generationReplaced = true;
			return publicationOwner;
		}
		return retainedOwner;
	}

	/** Selects a continuation mode only when exact owner and retained scene state are internally consistent. */
	SectionJobContinuationMode SectionScheduler::SelectSectionJobContinuationMode(
		const ::Services::Map::Buildings::BuildingMapSection& section,
		const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner) const noexcept
	{
		using Mode = SectionJobContinuationMode;
		if (publicationOwner == nullptr)
		{
			return Mode::FullPreparation;
		}
		if (!section.renderingState_)
		{
			return Mode::FullPreparation;
		}
		const SectionJob& job = *static_cast<const SectionJob*>(section.renderingState_.get());
		const bool sameOwner = job.publicationOwner != nullptr &&
			job.publicationOwner.get() == publicationOwner.get() &&
			!job.publicationOwner.owner_before(publicationOwner) &&
			!publicationOwner.owner_before(job.publicationOwner);
		if (!sameOwner || job.retainedScene == nullptr)
		{
			return Mode::FullPreparation;
		}
		if (!HasValidRetainedSceneForContinuation(job, publicationOwner))
		{
			return Mode::FullPreparation;
		}
		if (job.NumericalStatus.has_value())
		{
			if (job.NumericalOperation == nullptr || job.NumericalOperation->RequiresSceneInput())
			{
				return Mode::FullPreparation;
			}
			const auto status = job.NumericalStatus.value();
			const bool completedSuccessfully =
				status == Rendering::SectionRendering::Status::Complete ||
				status == Rendering::SectionRendering::Status::CompleteWithOmissions;
			return completedSuccessfully && job.AttemptEvidence->NumericalStatus == job.NumericalStatus
				? Mode::NoSceneInput
				: Mode::FullPreparation;
		}
		if (job.AttemptEvidence->NumericalStatus.has_value() || job.CompletedImageReady ||
			job.CompletedImageTransparent)
		{
			return Mode::FullPreparation;
		}
		if (job.NumericalOperation == nullptr || job.NumericalOperation->RequiresSceneInput())
		{
			return Mode::PreparedSceneInput;
		}
		return Mode::NoSceneInput;
	}

	/** Checks exact owner identity, complete scene evidence and retained drawable correspondence. */
	bool SectionScheduler::HasValidRetainedSceneForContinuation(
		const SectionJob& job,
		const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner) const noexcept
	{
		const bool sameOwner = job.publicationOwner != nullptr &&
			job.publicationOwner.get() == publicationOwner.get() &&
			!job.publicationOwner.owner_before(publicationOwner) &&
			!publicationOwner.owner_before(job.publicationOwner);
		return sameOwner && job.retainedScene != nullptr &&
			job.retainedScene->Preparation.PublicationIdentity == publicationOwner.get() &&
			job.retainedScene->Preparation.PreparationReadiness ==
				SectionScenePreparationResult::Readiness::CompleteSceneInputs &&
			job.AttemptEvidence.has_value() &&
			job.AttemptEvidence->SceneReadiness == SectionScenePreparationResult::Readiness::CompleteSceneInputs;
	}

	/** Releases operation state before the prepared scene while retaining its publication owner. */
	void SectionScheduler::ResetSectionJobProgress(SectionJob& job) noexcept
	{
		job.NumericalOperation.reset();
		job.Preparation = {};
		job.retainedScene.reset();
		job.CompletedImage.clear();
		job.TerminalSplineOmissions = {};
		job.NumericalStatus.reset();
		job.CompletedImageReady = false;
		job.CompletedImageTransparent = false;
		job.AttemptEvidence.reset();
	}

	/** Transfers one finished attempt to the caller and clears its retained progress. */
	void SectionScheduler::FinishSectionJobAttempt(
		SectionJob& job,
		SectionJobAdvanceResult& result)
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		auto& timing = job.AttemptEvidence->Timing;
		timing.TotalMs = timing.ScenePreparationMs + timing.SceneBorrowMs +
			timing.NumericalRenderingMs + timing.TransparencyCheckMs + timing.ImageAdoptionMs;
#endif
		result.FinishedAttempt = std::move(job.AttemptEvidence);
		result.CurrentState = SectionJobAdvanceResult::State::AttemptFinished;
		ResetSectionJobProgress(job);
	}

	/** Prepares a complete retained scene or finishes the established empty-publication path. */
	bool SectionScheduler::PrepareSectionJobAttempt(
		::Services::Map::Buildings::BuildingMapSection& section,
		SectionJob& job,
		SectionJobContinuationMode continuationMode,
		std::span<const SplinePolicy> splinePolicies,
		std::chrono::steady_clock::time_point deadline,
		SectionJobAdvanceResult& result)
	{
		if (!PrepareSectionJobSceneForAttempt(
			section, job, continuationMode, splinePolicies, deadline, result))
		{
			return false;
		}
		if (!job.publicationOwner->BorrowCandidateData().occurrences.empty())
		{
			return std::chrono::steady_clock::now() < deadline;
		}
		if (std::chrono::steady_clock::now() >= deadline)
		{
			return false;
		}
		CommitEmptySection(section, *job.AttemptEvidence);
		++result.WorkUnitsConsumed;
		FinishSectionJobAttempt(job, result);
		return false;
	}

	/** Resumes Section-owned occurrence preparation or accepts an already complete retained scene. */
	bool SectionScheduler::PrepareSectionJobSceneForAttempt(
		::Services::Map::Buildings::BuildingMapSection& section,
		SectionJob& job,
		SectionJobContinuationMode continuationMode,
		std::span<const SplinePolicy> splinePolicies,
		std::chrono::steady_clock::time_point deadline,
		SectionJobAdvanceResult& result)
	{
		if (continuationMode == SectionJobContinuationMode::FullPreparation &&
			(job.retainedScene != nullptr || job.NumericalOperation != nullptr || job.NumericalStatus.has_value() ||
				job.CompletedImageReady || job.CompletedImageTransparent))
		{
			ResetSectionJobProgress(job);
		}
		if (job.AttemptEvidence == std::nullopt)
		{
			job.AttemptEvidence.emplace();
		}
		if (job.retainedScene == nullptr)
		{
			if (continuationMode != SectionJobContinuationMode::FullPreparation)
			{
				ResetSectionJobProgress(job);
				result.CurrentState = SectionJobAdvanceResult::State::RestartRequired;
				return false;
			}
			if (std::chrono::steady_clock::now() >= deadline)
			{
				return false;
			}
			if (!PrepareRetainedSceneForAttempt(section, job, splinePolicies, deadline, result))
			{
				return false;
			}
		}
		else
		{
			result.SceneReadiness = SectionScenePreparationResult::Readiness::CompleteSceneInputs;
			job.AttemptEvidence->SceneReadiness = result.SceneReadiness;
		}
		return true;
	}

	/** Runs one source-composition attempt and records its readiness and Debug duration. */
	bool SectionScheduler::PrepareRetainedSceneForAttempt(
		::Services::Map::Buildings::BuildingMapSection& section,
		SectionJob& job,
		std::span<const SplinePolicy> splinePolicies,
		std::chrono::steady_clock::time_point deadline,
		SectionJobAdvanceResult& result)
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const auto started = std::chrono::steady_clock::now();
#endif
		bool deadlineDeferred = false;
		const auto readiness = PrepareRetainedSectionScene(
			section, job, splinePolicies, deadline, deadlineDeferred);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		job.AttemptEvidence->Timing.ScenePreparationMs +=
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
#endif
		++result.WorkUnitsConsumed;
		if (deadlineDeferred)
		{
			result.CurrentState = SectionJobAdvanceResult::State::Pending;
			return false;
		}
		if (readiness == SectionScenePreparationResult::Readiness::CompleteSceneInputs &&
			job.retainedScene == nullptr)
		{
			result.CurrentState = SectionJobAdvanceResult::State::Pending;
			return false;
		}
		result.SceneReadiness = readiness;
		job.AttemptEvidence->SceneReadiness = readiness;
		if (readiness != SectionScenePreparationResult::Readiness::CompleteSceneInputs)
		{
			const bool retryable = readiness == SectionScenePreparationResult::Readiness::PendingNativeSourceAcquisition ||
				readiness == SectionScenePreparationResult::Readiness::PendingSpriteAcquisition ||
				readiness == SectionScenePreparationResult::Readiness::TemporarilyUnavailableResources ||
				readiness == SectionScenePreparationResult::Readiness::UnresolvedInputs;
			result.CurrentState = retryable
				? SectionJobAdvanceResult::State::WaitingForResources
				: SectionJobAdvanceResult::State::RestartRequired;
			if (!retryable && !job.Preparation.TerminalReadiness.has_value())
			{
				ResetSectionJobProgress(job);
			}
			return false;
		}
		return true;
	}

	/** Advances the persistent operation with its Section-owned scene spans when required. */
	bool SectionScheduler::AdvanceSectionJobNumerical(
		::Services::Map::Buildings::BuildingMapSection& section,
		SectionJob& job,
		std::chrono::steady_clock::time_point deadline,
		SectionJobAdvanceResult& result)
	{
		if (job.NumericalStatus.has_value())
		{
			return true;
		}
		if (job.NumericalOperation == nullptr)
		{
			if (std::chrono::steady_clock::now() >= deadline)
			{
				return false;
			}
			const auto grid = CreateSectionRenderingGrid(section, sectionSizeMapUnits_, sectionResolutionPixels_);
			job.NumericalOperation = std::make_unique<Rendering::SectionRendering::IncrementalOperation>(grid);
		}
		Visuals::BuildingRenderSceneInput emptyScene;
		const Visuals::BuildingRenderSceneInput* sceneInput = &emptyScene;
		if (job.NumericalOperation->RequiresSceneInput())
		{
			if (job.retainedScene == nullptr)
			{
				ResetSectionJobProgress(job);
				result.CurrentState = SectionJobAdvanceResult::State::RestartRequired;
				return false;
			}
			sceneInput = &job.retainedScene->RenderSceneInput;
		}
		return RunSectionNumericalSlice(section, job, *sceneInput, deadline, result);
	}

	/** Runs one numerical call and translates its terminal status without repeating prior work. */
	bool SectionScheduler::RunSectionNumericalSlice(
		::Services::Map::Buildings::BuildingMapSection& section,
		SectionJob& job,
		const Visuals::BuildingRenderSceneInput& sceneInput,
		std::chrono::steady_clock::time_point deadline,
		SectionJobAdvanceResult& result)
	{
		if (std::chrono::steady_clock::now() >= deadline)
		{
			return false;
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const auto numericalStarted = std::chrono::steady_clock::now();
		job.AttemptEvidence->Timing.NumericalRenderingInvoked = true;
#endif
		const auto step = job.NumericalOperation->Advance(
			sceneInput, section.GetOrCreateVisualsContext(), deadline);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		job.AttemptEvidence->Timing.NumericalRenderingMs +=
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - numericalStarted).count();
#endif
		result.WorkUnitsConsumed += step.WorkUnitsConsumed;
		if (!step.TerminalStatus.has_value())
		{
			return false;
		}
		job.AttemptEvidence->NumericalStatus = step.TerminalStatus;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		job.AttemptEvidence->Timing.NumericalPhases = job.NumericalOperation->PhaseTimings();
#endif
		if (!IsCompletedRenderComplete(*job.AttemptEvidence))
		{
			job.AttemptEvidence->TerminalSplineOmissions = {};
			FinishSectionJobAttempt(job, result);
			return false;
		}
		job.NumericalStatus = step.TerminalStatus;
		return true;
	}

	/** Routes finalized transparency or adopts complete image bytes before the deadline. */
	void SectionScheduler::CommitSectionJobImage(
		::Services::Map::Buildings::BuildingMapSection& section,
		SectionJob& job,
		std::chrono::steady_clock::time_point deadline,
		SectionJobAdvanceResult& result)
	{
		if (!job.CompletedImageReady)
		{
			if (std::chrono::steady_clock::now() >= deadline ||
				!ClassifyCompletedSectionImage(section, job, result))
			{
				return;
			}
		}
		if (job.CompletedImageTransparent || std::chrono::steady_clock::now() >= deadline)
		{
			return;
		}
		auto& evidence = *job.AttemptEvidence;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const auto adoptionStarted = std::chrono::steady_clock::now();
		evidence.Timing.ImageAdoptionAttempted = true;
#endif
		section.AdoptCompletedImage(
			sectionResolutionPixels_, sectionResolutionPixels_, std::move(job.CompletedImage));
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		evidence.Timing.ImageAdoptionMs +=
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - adoptionStarted).count();
#endif
		++result.WorkUnitsConsumed;
		evidence.TerminalSplineOmissions = job.TerminalSplineOmissions;
		evidence.CommitStatus = SectionImageCommitResult::Status::Committed;
		FinishSectionJobAttempt(job, result);
	}

	/** Takes finalized output once and commits an exact-clear image as empty. */
	bool SectionScheduler::ClassifyCompletedSectionImage(
		::Services::Map::Buildings::BuildingMapSection& section,
		SectionJob& job,
		SectionJobAdvanceResult& result)
	{
		auto& evidence = *job.AttemptEvidence;
		if (!job.CompletedImageReady)
		{
			if (!job.NumericalOperation->TakeCompletedImage(
				job.CompletedImage,
				job.TerminalSplineOmissions,
				job.CompletedImageTransparent))
			{
				evidence.CommitStatus = SectionImageCommitResult::Status::NumericalRenderingFailed;
				evidence.TerminalSplineOmissions = {};
				FinishSectionJobAttempt(job, result);
				return false;
			}
			job.CompletedImageReady = true;
			ExportCompletedSectionImage(
				section,
				std::span<const std::uint8_t>(job.CompletedImage));
		}
		if (job.CompletedImageTransparent)
		{
			evidence.TerminalSplineOmissions = job.TerminalSplineOmissions;
			CommitEmptySection(section, evidence);
			FinishSectionJobAttempt(job, result);
			return false;
		}
		return true;
	}

	/** Advances one existing job for its exact publication owner until the shared deadline. */
	SectionScheduler::SectionJobAdvanceResult SectionScheduler::AdvanceSectionJob(
		::Services::Map::Buildings::BuildingMapSection& section,
		const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
		SectionJobContinuationMode continuationMode,
		std::span<const SplinePolicy> splinePolicies,
		std::chrono::steady_clock::time_point deadline)
	{
		SectionJobAdvanceResult result;
		if (!section.renderingState_)
		{
			result.CurrentState = SectionJobAdvanceResult::State::RestartRequired;
			return result;
		}
		SectionJob& job = *static_cast<SectionJob*>(section.renderingState_.get());
		if (std::chrono::steady_clock::now() >= deadline)
		{
			return result;
		}
		const bool sameOwner = publicationOwner != nullptr && job.publicationOwner != nullptr &&
			job.publicationOwner.get() == publicationOwner.get() &&
			!job.publicationOwner.owner_before(publicationOwner) &&
			!publicationOwner.owner_before(job.publicationOwner);
		if (!sameOwner)
		{
			ResetSectionJobProgress(job);
			result.CurrentState = SectionJobAdvanceResult::State::RestartRequired;
			return result;
		}
		if (continuationMode != SelectSectionJobContinuationMode(section, publicationOwner))
		{
			ResetSectionJobProgress(job);
			result.CurrentState = SectionJobAdvanceResult::State::RestartRequired;
			return result;
		}
		if (!PrepareSectionJobAttempt(
			section, job, continuationMode, splinePolicies, deadline, result))
		{
			return result;
		}
		if (AdvanceSectionJobNumerical(section, job, deadline, result))
		{
			CommitSectionJobImage(section, job, deadline, result);
		}
		return result;
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

	/** Resets per-publication counters, identity, prepared views and shared payload owners. */
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
			SortSectionSpriteInputs(output);
		}
	}

	/** Preserves the established stable painter ordering for ordinary and overlay sprite inputs. */
	void SectionScheduler::SortSectionSpriteInputs(SectionScenePreparationResult& output)
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
		SplineRenderInputPreparationResult preparation;
		PrepareSplineOccurrence(publicationOwner, ordinal, source, splinePolicies, outcome, preparation);
		output.SplinePreparations.push_back(std::move(preparation));
	}

	/** Composes one immutable spline occurrence and preserves its established readiness class. */
	void SectionScheduler::PrepareSplineOccurrence(
		const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
		std::uint64_t ordinal,
		const BuildingCandidateSourceRecord& source,
		std::span<const SplinePolicy> splinePolicies,
		KnownSpritePreparationResult::OccurrenceOutcome& outcome,
		SplineRenderInputPreparationResult& preparation) const
	{
		const std::uint16_t policyIndex = source.splinePolicyIndex;
		const SplinePolicy* policy = policyIndex != 0 && policyIndex < splinePolicies.size()
			? &splinePolicies[policyIndex]
			: nullptr;
		const bool classifyComposition = outcome.PreparationStatus ==
			KnownSpritePreparationResult::OccurrenceOutcome::Status::NotProcessed;
		ComposeSplineRenderInput(publicationOwner, ordinal, policyIndex, policy, preparation);
		if (!classifyComposition)
		{
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
		KnownSpritePreparationResult::OccurrenceOutcome& occurrenceOutcome,
		bool updatePreparationCounts)
	{
		if (buildingScheduler_ == nullptr || cpuSpriteSourceManager_ == nullptr)
		{
			if (updatePreparationCounts)
			{
				++output.UnavailableCount;
			}
			occurrenceOutcome.PreparationStatus =
				KnownSpritePreparationResult::OccurrenceOutcome::Status::TemporarilyUnavailableSprite;
			return;
		}
		const BuildingScheduler::RequirementStatus readiness =
			buildingScheduler_->EnsureSpriteReady(ownedKey);
		if (readiness == BuildingScheduler::RequirementStatus::Pending)
		{
			if (updatePreparationCounts)
			{
				++output.PendingCount;
			}
			occurrenceOutcome.PreparationStatus =
				KnownSpritePreparationResult::OccurrenceOutcome::Status::PendingSpriteAcquisition;
			return;
		}
		if (readiness == BuildingScheduler::RequirementStatus::Failed)
		{
			if (updatePreparationCounts)
			{
				++output.FailedCount;
			}
			occurrenceOutcome.PreparationStatus =
				KnownSpritePreparationResult::OccurrenceOutcome::Status::TerminalSpriteCaptureFailure;
			return;
		}
		if (readiness == BuildingScheduler::RequirementStatus::Unavailable)
		{
			if (updatePreparationCounts)
			{
				++output.UnavailableCount;
			}
			occurrenceOutcome.PreparationStatus =
				KnownSpritePreparationResult::OccurrenceOutcome::Status::TemporarilyUnavailableSprite;
			return;
		}
		PrepareReadySpriteRequirement(
			publicationOrdinal,
			requirement,
			ownedKey,
			output,
			occurrenceOutcome,
			updatePreparationCounts);
	}

	/** Obtains the shared resident payload and appends only after support has been established. */
	void SectionScheduler::PrepareReadySpriteRequirement(
		std::uint64_t publicationOrdinal,
		const BuildingCandidateSourceRecord& requirement,
		const Cache::OwnedSpriteKey& ownedKey,
		KnownSpritePreparationResult& output,
		KnownSpritePreparationResult::OccurrenceOutcome& occurrenceOutcome,
		bool updatePreparationCounts)
	{
		if (cpuSpriteSourceManager_ == nullptr)
		{
			if (updatePreparationCounts)
			{
				++output.UnavailableCount;
			}
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
			if (updatePreparationCounts)
			{
				++output.UnavailableCount;
			}
			occurrenceOutcome.PreparationStatus =
				KnownSpritePreparationResult::OccurrenceOutcome::Status::TemporarilyUnavailableSprite;
			return;
		}
		if (!requirement.hasPlacement)
		{
			if (updatePreparationCounts)
			{
				++output.ReadyWithoutPlacementCount;
			}
			occurrenceOutcome.PreparationStatus =
				KnownSpritePreparationResult::OccurrenceOutcome::Status::MissingSpritePlacement;
			return;
		}
		AppendReadySpriteInput(
			publicationOrdinal,
			requirement,
			spriteResult.Payload,
			output,
			occurrenceOutcome,
			updatePreparationCounts);
	}

	/** Appends the placement input, shared payload owner and sparse ordinal index.
	 * The input span borrows pixels from the payload retained in the paired prepared occurrence.
	 */
	void SectionScheduler::AppendReadySpriteInput(
		std::uint64_t publicationOrdinal,
		const BuildingCandidateSourceRecord& requirement,
		std::shared_ptr<const Cache::ProcessedSpritePayload> payloadOwner,
		KnownSpritePreparationResult& output,
		KnownSpritePreparationResult::OccurrenceOutcome& occurrenceOutcome,
		bool updatePreparationCounts)
	{
		const Cache::ProcessedSpritePayload& payload = *payloadOwner;
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
			if (updatePreparationCounts)
			{
				++output.UnresolvedSpriteGeometryCount;
			}
			occurrenceOutcome.PreparationStatus =
				KnownSpritePreparationResult::OccurrenceOutcome::Status::UnresolvedSpriteGeometry;
			return;
		}
		KnownSpritePreparationResult::PreparedSpriteOccurrence prepared;
		prepared.PublicationOrdinal = publicationOrdinal;
		prepared.SpritePass = requirement.spritePass;
		prepared.RenderInput = renderInput;
		prepared.Payload = std::move(payloadOwner);
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
