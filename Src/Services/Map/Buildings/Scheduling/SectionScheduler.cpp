#if defined(MODLOADER_CLIENT_BUILD)

// Owns acquisition resources and advances sprite-first Section geometry and optional image work.
#include "SectionScheduler.h"

#include "BuildingScheduler.h"
#include "../BuildingMapSection.h"
#include "../Cache/SpriteDiskCache.h"
#include "../Cache/SpriteSourceManager.h"
#include "../Cache/SpritePngCodec.h"
#include "../../../Log.h"
#include "../Rendering/SectionRendering.h"
#include "../Visuals/SpriteGeometry.h"
#include "../Visuals/SplineGeometry.h"
#include "../Visuals/PreparedBuildingGeometry.h"

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

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
#define REPORT_SECTION_FAILURE(section, hash, status, ...) \
	(section).ReportFailure((hash), static_cast<int>(status), Visuals::SectionFailureDetails{__VA_ARGS__})
#else
#define REPORT_SECTION_FAILURE(...) ((void)0)
#endif

namespace Services::Map::Buildings::Scheduling
{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
	using Performance = BuildingMapPerformanceDiagnostics;
#endif
	/** Clears one owned result while preserving vector capacities for reuse. */
	void SectionScheduler::SplineRenderInputPreparationResult::Reset() noexcept
	{
		PublicationOrdinal = 0;
		GeometryMode = BuildingCandidateSplineGeometryMode::AuthoredProfiles;
		CurvePreparation.SourceStatus = BuildingCandidateSourceStatus::Unknown;
		CurvePreparation.ProfileStatus = SplineProfileAcquisitionStatus::Unknown;
		CurvePreparation.ConversionIssue = Visuals::SplineNumericalIssue::None;
		CurvePreparation.PreparationStatus = SplineCurvePreparationResult::CurveStatus::NotPrepared;
		CurvePreparation.RootStatus = SplineCurvePreparationResult::RootTransformStatus::NotApplicable;
		CurvePreparation.RootTransform.reset();
		CurvePreparation.Segments.clear();
		SurfaceInputs.clear();
		SamplingPolicy = {};
		PreparationStatus = Status::NotAttempted;
	}

	/** Clears scene-owned values without retaining publication spans or source data. */
	void SectionScheduler::SectionScenePreparationResult::Reset() noexcept
	{
		OccurrenceOutcomes.clear();
		SplinePreparations.clear();
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

	/** Builds borrowed ordered spline inputs after direct Section sprite ownership has settled. */
	bool SectionScheduler::SectionScenePreparationResult::BorrowRenderSceneInput(
		const ::Services::Map::Buildings::BuildingMapSection& section,
		const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
		std::span<const SplineRenderInputPreparationResult> splinePreparations,
		std::vector<Visuals::BuildingSplineRenderInput>& scratch,
		Visuals::BuildingRenderSceneInput& output) const
	{
		output = {};
		scratch.clear();
		if (publicationOwner == nullptr || PublicationIdentity != publicationOwner.get() ||
			PreparationReadiness != Readiness::CompleteSceneInputs)
		{
			REPORT_SECTION_FAILURE(section, publicationOwner ? std::optional{publicationOwner->ContentHash()} : std::nullopt,
				Readiness::InvalidPreparationInputs, "retained scene assembly", publicationOwner == nullptr
				? "publication owner absent" : PublicationIdentity != publicationOwner.get()
				? "scene publication identity mismatch" : "retained scene inputs are incomplete",
				-1, -1, static_cast<unsigned>(Readiness::CompleteSceneInputs), static_cast<unsigned>(PreparationReadiness));
			return false;
		}
		scratch.reserve(splinePreparations.size());
		for (const SplineRenderInputPreparationResult& splinePreparation : splinePreparations)
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
			if (!splinePreparation.BorrowRenderInput(section, *publicationOwner, splineInput))
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
		output.Splines = std::span<const Visuals::BuildingSplineRenderInput>(scratch);
		return true;
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

	/** Retains one complete source-composed scene and builds stable render spans after ownership settles. */
	SectionScheduler::SectionScenePreparationResult::Readiness
		SectionScheduler::PrepareRetainedSectionScene(
			::Services::Map::Buildings::BuildingMapSection& section,
			SectionJob& job,
			std::span<const SplinePolicy> splinePolicies,
			const SectionPreparationVisit& visit,
			bool& deadlineDeferred)
	{
		using Readiness = SectionScenePreparationResult::Readiness;
		deadlineDeferred = false;
		if (job.Preparation.TerminalReadiness.has_value())
		{
			return *job.Preparation.TerminalReadiness;
		}
		if (TryReuseCompletedSceneWithoutResources(section, job))
		{
			return Readiness::CompleteSceneInputs;
		}
		if (!InitializeSectionPreparationProgress(section, job, job.publicationOwner))
		{
			return *job.Preparation.TerminalReadiness;
		}
		const BuildingCandidateData candidateData = job.publicationOwner->BorrowCandidateData();
		if (!job.GeometryComplete)
		{
			section.BeginPreparingScene(job.publicationOwner->ContentHash(), candidateData.occurrences.size());
		}
		deadlineDeferred = AdvanceSectionOccurrencePreparation(
			section,
			job,
			job.publicationOwner,
			splinePolicies,
			visit);
		if (deadlineDeferred)
		{
			return Readiness::UnresolvedInputs;
		}
		const Readiness readiness = GetSectionPreparationReadiness(job);
		if (readiness != Readiness::CompleteSceneInputs)
		{
			return readiness;
		}
		if (std::chrono::steady_clock::now() >= visit.Deadline)
		{
			return readiness;
		}
		if (!FinalizeRetainedSectionScene(section, job, job.publicationOwner))
		{
			job.Preparation.TerminalReadiness = Readiness::InvalidPreparationInputs;
			return Readiness::InvalidPreparationInputs;
		}
		return Readiness::CompleteSceneInputs;
	}

	/** Reuses a hash-matched completed scene when raster drawing needs no borrowed resources. */
	bool SectionScheduler::TryReuseCompletedSceneWithoutResources(
		::Services::Map::Buildings::BuildingMapSection& section,
		SectionJob& job) const
	{
		if (!section.HasCompletedSceneForContentHash(job.publicationOwner->ContentHash()) ||
			!IsPublicationForSection(section, *job.publicationOwner))
		{
			return false;
		}
		const auto& surfaces = section.completedScene_->SplineSurfaces();
		if (std::any_of(surfaces.begin(), surfaces.end(), [](const auto& surface) { return surface.IsTextured; }))
		{
			return false;
		}
		job.GeometryComplete = true;
		return true;
	}

	/** Initializes per-ordinal evidence once for the exact selected publication owner. */
	bool SectionScheduler::InitializeSectionPreparationProgress(
		::Services::Map::Buildings::BuildingMapSection& section,
		SectionJob& job,
		const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner)
	{
		if (job.Preparation.Initialized)
		{
			const bool samePublication = job.Preparation.SpritePreparation.PublicationIdentity == publicationOwner.get();
			if (!samePublication)
			{
				REPORT_SECTION_FAILURE(section, publicationOwner ? std::optional{publicationOwner->ContentHash()} : std::nullopt,
					SectionScenePreparationResult::Readiness::InvalidPublication,
					"source publication", "initialized occurrence publication identity mismatch");
				job.Preparation.TerminalReadiness = SectionScenePreparationResult::Readiness::InvalidPublication;
			}
			return samePublication;
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		Performance::Timer initialization(&Performance::Game().SectionInitialization, &Performance::Game().Indivisible);
#endif
		using Readiness = SectionScenePreparationResult::Readiness;
		if (publicationOwner == nullptr)
		{
			REPORT_SECTION_FAILURE(section, std::nullopt, Readiness::InvalidPublication,
				"source publication", "publication owner absent during occurrence initialization");
			job.Preparation.TerminalReadiness = Readiness::InvalidPublication;
			return false;
		}
		if (!IsPublicationForSection(section, *publicationOwner))
		{
			REPORT_SECTION_FAILURE(section, publicationOwner->ContentHash(), Readiness::InvalidSection,
				"source publication", "publication region does not match Section grid");
			job.Preparation.TerminalReadiness = Readiness::InvalidSection;
			return false;
		}
		return InitializeSectionOccurrenceOrder(section, job, *publicationOwner);
	}

	/** Validates aligned outcomes and builds the single stable sprite-first traversal index. */
	bool SectionScheduler::InitializeSectionOccurrenceOrder(
		::Services::Map::Buildings::BuildingMapSection& section,
		SectionJob& job,
		const BuildingCandidatePublication& publication)
	{
		const BuildingCandidateData candidateData = publication.BorrowCandidateData();
		const BuildingCandidateSourceData sourceData = publication.BorrowSourceData();
		auto& preparation = job.Preparation;
		if (!InitializeKnownSpriteOccurrences(
			section, publication.ContentHash(), candidateData, sourceData, preparation.SpritePreparation))
		{
			preparation.SpritePreparation.PreparationStatus = KnownSpritePreparationResult::Status::InvalidPublication;
			preparation.TerminalReadiness = SectionScenePreparationResult::Readiness::InvalidPublication;
			return false;
		}
		preparation.SpritePreparation.PublicationIdentity = &publication;
		preparation.SpritePreparation.PreparationStatus =
			KnownSpritePreparationResult::Status::KnownRequirementsProcessed;
		preparation.Occurrences.resize(candidateData.occurrences.size());
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		Performance::Timer ordering(&Performance::Game().OccurrenceOrder);
#endif
		if (section.HasCompletedSceneForContentHash(publication.ContentHash()))
		{
			const auto& completed = *section.completedScene_;
			if (completed.spritePainterOrder_.size() != sourceData.records.size() ||
				completed.spritePainterCount_ > completed.spritePainterOrder_.size() ||
				completed.validSpritePainterCount_ > completed.spritePainterCount_)
			{
				preparation.TerminalReadiness =
					SectionScenePreparationResult::Readiness::InvalidPreparationInputs;
				return false;
			}
			preparation.OccurrenceOrder = completed.spritePainterOrder_;
			preparation.InitialSpriteCount = completed.spritePainterCount_;
			preparation.ValidSpriteCount = completed.validSpritePainterCount_;
			job.GeometryComplete = true;
		}
		else
		{
			preparation.OccurrenceOrder.resize(sourceData.records.size());
			std::size_t remainingBegin = preparation.OccurrenceOrder.size();
			for (std::size_t ordinal = 0; ordinal < sourceData.records.size(); ++ordinal)
			{
				if (sourceData.records[ordinal].kind == BuildingCandidateSourceKind::Sprite)
				{
					preparation.OccurrenceOrder[preparation.InitialSpriteCount++] = ordinal;
				}
				else
				{
					preparation.OccurrenceOrder[--remainingBegin] = ordinal;
				}
			}
			std::reverse(preparation.OccurrenceOrder.begin() + remainingBegin, preparation.OccurrenceOrder.end());
			preparation.ValidSpriteCount = SortSpriteOccurrenceOrder(
				sourceData,
				preparation.OccurrenceOrder,
				preparation.InitialSpriteCount);
			auto* preparedGeometry = section.BeginPreparingScene(
				publication.ContentHash(), candidateData.occurrences.size());
			preparedGeometry->spritePainterOrder_ = preparation.OccurrenceOrder;
			preparedGeometry->validSpritePainterCount_ = preparation.ValidSpriteCount;
			preparedGeometry->spritePainterCount_ = preparation.InitialSpriteCount;
		}
		preparation.Initialized = true;
		return true;
	}

	/** Sorts valid sprite placements once while keeping unresolved ordinals in source order. */
	std::size_t SectionScheduler::SortSpriteOccurrenceOrder(
		const BuildingCandidateSourceData& sourceData,
		std::vector<std::size_t>& occurrenceOrder,
		std::size_t spriteCount)
	{
		/** Caches the painter comparison fields for one sprite during this sort only. */
		struct SpritePainterSortKey
		{
			bool HasValidPlacement = false;
			double Z = 0.0;
			double Y = 0.0;
			double X = 0.0;
			std::size_t OriginalOrdinal = 0;

			/** Orders valid keys by Z/Y/X and resolves every tie by source ordinal. */
			bool operator<(const SpritePainterSortKey& other) const
			{
				if (HasValidPlacement != other.HasValidPlacement)
				{
					return HasValidPlacement;
				}
				if (!HasValidPlacement)
				{
					return OriginalOrdinal < other.OriginalOrdinal;
				}
				if (Z != other.Z)
				{
					return Z < other.Z;
				}
				if (Y != other.Y)
				{
					return Y < other.Y;
				}
				if (X != other.X)
				{
					return X < other.X;
				}
				return OriginalOrdinal < other.OriginalOrdinal;
			}
		};
		std::vector<SpritePainterSortKey> sortKeys;
		sortKeys.reserve(spriteCount);
		std::size_t validSpriteCount = 0;
		for (std::size_t spriteIndex = 0; spriteIndex < spriteCount; ++spriteIndex)
		{
			const std::size_t ordinal = occurrenceOrder[spriteIndex];
			const BuildingCandidateSourceRecord& source = sourceData.records[ordinal];
			const auto& position = source.placement.Position;
			const bool hasValidPlacement = source.hasPlacement &&
				std::isfinite(position[0]) &&
				std::isfinite(position[1]) &&
				std::isfinite(position[2]);
			if (hasValidPlacement)
			{
				++validSpriteCount;
			}
			sortKeys.push_back({hasValidPlacement, position[2], position[1], position[0], ordinal});
		}
		std::sort(sortKeys.begin(), sortKeys.end());
		for (std::size_t spriteIndex = 0; spriteIndex < spriteCount; ++spriteIndex)
		{
			occurrenceOrder[spriteIndex] = sortKeys[spriteIndex].OriginalOrdinal;
		}
		return validSpriteCount;
	}

	/** Advances untouched ordinals once and visits only sprite resources still unavailable. */
	bool SectionScheduler::AdvanceSectionOccurrencePreparation(
		::Services::Map::Buildings::BuildingMapSection& section,
		SectionJob& job,
		const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner,
		std::span<const SplinePolicy> splinePolicies,
		const SectionPreparationVisit& visit)
	{
		auto& preparation = job.Preparation;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		Performance::Timer batch(&Performance::Game().OccurrenceBatches);
#endif
		if (preparation.RetryTickSequence != visit.TickSequence)
		{
			preparation.RetryTickSequence = visit.TickSequence;
			preparation.FirstRetryQueuedThisTick.reset();
		}
		const BuildingCandidateData candidateData = publicationOwner->BorrowCandidateData();
		const BuildingCandidateSourceData sourceData = publicationOwner->BorrowSourceData();
		const std::size_t end = visit.InitialSpritesOnly
			? preparation.InitialSpriteCount : preparation.OccurrenceOrder.size();
		while (preparation.NextOccurrenceIndex < end &&
			std::chrono::steady_clock::now() < visit.Deadline)
		{
			const std::size_t ordinal = preparation.OccurrenceOrder[preparation.NextOccurrenceIndex++];
			PrepareSectionOccurrence(section, job, *publicationOwner, candidateData.occurrences[ordinal],
				sourceData.records[ordinal], ordinal, splinePolicies);
			RetainTerminalSpriteOmission(section, job, ordinal);
		}
		if (visit.InitialSpritesOnly || preparation.NextOccurrenceIndex < end)
		{
			return true;
		}
		AdvanceSectionSpriteRetries(section, job, sourceData, visit.Deadline);
		return std::chrono::steady_clock::now() >= visit.Deadline;
	}

	/** Marks an initial terminal source or capture outcome on its Section occurrence. */
	void SectionScheduler::RetainTerminalSpriteOmission(
		::Services::Map::Buildings::BuildingMapSection& section,
		SectionJob& job,
		std::size_t ordinal)
	{
		using Status = KnownSpritePreparationResult::OccurrenceOutcome::Status;
		const auto status = job.Preparation.SpritePreparation.OccurrenceOutcomes[ordinal].PreparationStatus;
		if (!job.GeometryComplete &&
			(status == Status::TerminalSourceFailure || status == Status::TerminalSpriteCaptureFailure ||
				status == Status::TerminalSpriteGeometryFailure))
		{
			auto* preparingScene = section.BeginPreparingScene(
				job.publicationOwner->ContentHash(), job.Preparation.Occurrences.size());
			preparingScene->TrySetContributionStatus(ordinal,
				Visuals::PreparedBuildingGeometry::ContributionStatus::TerminalOmission);
		}
	}

	/** Leaves new and requeued resources behind the current tick's retry boundary. */
	void SectionScheduler::AdvanceSectionSpriteRetries(
		::Services::Map::Buildings::BuildingMapSection& section,
		SectionJob& job,
		const BuildingCandidateSourceData& sourceData,
		std::chrono::steady_clock::time_point deadline)
	{
		auto& preparation = job.Preparation;
		while (preparation.SpriteRetryHead.has_value() &&
			preparation.SpriteRetryHead != preparation.FirstRetryQueuedThisTick &&
			std::chrono::steady_clock::now() < deadline)
		{
			std::size_t ordinal = 0;
			PopSectionSpriteRetry(job, ordinal);
			RetrySectionSpriteResource(section, job, sourceData.records[ordinal], ordinal);
			RetainTerminalSpriteOmission(section, job, ordinal);
			const auto retryStatus = preparation.SpritePreparation.OccurrenceOutcomes[ordinal].SpritePreparationStatus;
			if (IsSpriteResourceRetryStatus(retryStatus))
			{
				return;
			}
		}
	}

	/** Classifies and prepares one occurrence without revisiting its completed actions. */
	void SectionScheduler::PrepareSectionOccurrence(
		::Services::Map::Buildings::BuildingMapSection& section,
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
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			Performance::Timer indivisible(&Performance::Game().Indivisible);
#endif
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
			PrepareSectionSpriteOccurrence(section, job, publication, source, ordinal, sourceBlocked);
		}
		AdvanceFirstUnresolvedOrdinal(job, ordinal);
	}

	/** Validates one canonical key once and retains it only for this occurrence's generation. */
	void SectionScheduler::PrepareSectionSpriteOccurrence(
		::Services::Map::Buildings::BuildingMapSection& section,
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
		if (job.GeometryComplete)
		{
			if (sourcePreparationBlocked)
			{
				outcome.SpritePreparationStatus = outcome.PreparationStatus;
				return;
			}
			bool isOverlayLast = false;
			const auto* prepared = section.completedScene_->SpriteOccurrenceAtOrdinal(ordinal, isOverlayLast);
			const bool expectedOverlayLast = source.spritePass == BuildingCandidateSpritePass::SecondPass;
			if (prepared != nullptr && prepared->Payload != nullptr &&
				prepared->SourceKey.Bytes() == source.canonicalClassPath &&
				isOverlayLast == expectedOverlayLast)
			{
				outcome.PreparationStatus = KnownSpritePreparationResult::OccurrenceOutcome::Status::ReadySprite;
			}
			else if (section.completedScene_->contributionStates_[ordinal] ==
				Visuals::PreparedBuildingGeometry::ContributionStatus::TerminalOmission)
			{
				outcome.PreparationStatus =
					KnownSpritePreparationResult::OccurrenceOutcome::Status::TerminalSpriteCaptureFailure;
			}
			else
			{
				outcome.PreparationStatus =
					KnownSpritePreparationResult::OccurrenceOutcome::Status::UnresolvedInputs;
			}
			outcome.SpritePreparationStatus = outcome.PreparationStatus;
			AdvanceFirstUnresolvedOrdinal(job, ordinal);
			return;
		}
		preparation.Occurrences[ordinal].SpriteKey = std::move(spriteKey);
		PrepareSpriteRequirement(section, job, source, ordinal, sourcePreparationBlocked, false);
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
		::Services::Map::Buildings::BuildingMapSection& section,
		SectionJob& job,
		const BuildingCandidateSourceRecord& source,
		std::size_t ordinal)
	{
		auto& progress = job.Preparation;
		auto& occurrence = progress.Occurrences[ordinal];
		auto& outcome = progress.SpritePreparation.OccurrenceOutcomes[ordinal];
		const bool sourceBlocked = outcome.PreparationStatus != outcome.SpritePreparationStatus;
		const auto sceneStatus = outcome.PreparationStatus;
		PrepareSpriteRequirement(section, job, source, ordinal, sourceBlocked, false);
		const auto spriteStatus = outcome.PreparationStatus;
		outcome.SpritePreparationStatus = spriteStatus;
		if (sourceBlocked)
		{
			outcome.PreparationStatus = sceneStatus;
		}
		AdvanceFirstUnresolvedOrdinal(job, ordinal);
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

	/** Advances the lowest unresolved source ordinal without passing untouched outcomes. */
	void SectionScheduler::AdvanceFirstUnresolvedOrdinal(SectionJob& job, std::size_t ordinal) noexcept
	{
		auto& preparation = job.Preparation;
		const auto& outcomes = preparation.SpritePreparation.OccurrenceOutcomes;
		if (ordinal != preparation.FirstUnresolvedOrdinal)
		{
			return;
		}
		while (preparation.FirstUnresolvedOrdinal < outcomes.size() &&
			!IsSectionOccurrenceBlockingStatus(outcomes[preparation.FirstUnresolvedOrdinal].PreparationStatus))
		{
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
		if (!preparation.FirstRetryQueuedThisTick.has_value())
		{
			preparation.FirstRetryQueuedThisTick = ordinal;
		}
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
		if (job.Preparation.NextOccurrenceIndex < job.Preparation.OccurrenceOrder.size())
		{
			return Readiness::UnresolvedInputs;
		}
		if (job.Preparation.FirstUnresolvedOrdinal == job.Preparation.Occurrences.size())
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
			status != Status::TerminalSpriteGeometryFailure &&
			status != Status::ReadySprite && status != Status::ReadySpline;
	}

	/** Retains completed spline inputs; sprite geometry and payloads already belong to the Section. */
	bool SectionScheduler::FinalizeRetainedSectionScene(
		::Services::Map::Buildings::BuildingMapSection& section,
		SectionJob& job,
		const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner)
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		Performance::Timer finalization(&Performance::Game().Continuation, &Performance::Game().Indivisible);
#endif
		auto& progress = job.Preparation;
		auto& spritePreparation = progress.SpritePreparation;
		SectionScenePreparationResult preparation;
		preparation.PublicationIdentity = publicationOwner.get();
		preparation.PreparationReadiness = SectionScenePreparationResult::Readiness::CompleteSceneInputs;
		if (publicationOwner == nullptr ||
			spritePreparation.PublicationIdentity != publicationOwner.get() ||
			!progress.Initialized ||
			progress.NextOccurrenceIndex != progress.Occurrences.size() ||
			spritePreparation.OccurrenceOutcomes.size() != progress.Occurrences.size())
		{
			REPORT_SECTION_FAILURE(section, publicationOwner ? std::optional{publicationOwner->ContentHash()} : std::nullopt,
				SectionScenePreparationResult::Readiness::InvalidPreparationInputs, "retained scene assembly",
				publicationOwner == nullptr ? "publication owner absent" :
				spritePreparation.PublicationIdentity != publicationOwner.get() ? "sprite preparation publication identity mismatch" :
				!progress.Initialized ? "occurrence progress not initialized" :
				progress.NextOccurrenceIndex != progress.Occurrences.size() ? "occurrence traversal incomplete" :
				"occurrence outcome count mismatch", -1, -1, progress.Occurrences.size(),
				progress.NextOccurrenceIndex != progress.Occurrences.size() ?
				progress.NextOccurrenceIndex : spritePreparation.OccurrenceOutcomes.size());
			return false;
		}
		preparation.OccurrenceOutcomes = std::move(spritePreparation.OccurrenceOutcomes);
		preparation.SplinePreparations = std::move(progress.SplinePreparations);
		auto retained = std::make_unique<RetainedSectionScene>();
		preparation.PreparationReadiness = SectionScenePreparationResult::Readiness::CompleteSceneInputs;
		if (!HasOnlyRetainableSplineAppearances(section, publicationOwner->ContentHash(), preparation))
		{
			return false;
		}
		if (!InitializeRetainedSceneInputs(section, preparation, *retained, publicationOwner))
		{
			return false;
		}
		job.retainedScene = std::move(retained);
		job.Preparation = {};
		return true;
	}

	/** Retains stable spline descriptors for numerical Section rendering. */
	bool SectionScheduler::InitializeRetainedSceneInputs(
		::Services::Map::Buildings::BuildingMapSection& section,
		SectionScenePreparationResult& preparation,
		RetainedSectionScene& retained,
		const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner)
	{
		std::vector<Visuals::BuildingSplineRenderInput> splineDescriptors;
		Visuals::BuildingRenderSceneInput validatedScene;
		retained.SplinePreparations = std::move(preparation.SplinePreparations);
		if (!preparation.BorrowRenderSceneInput(
			section, publicationOwner, retained.SplinePreparations, splineDescriptors, validatedScene))
		{
			return false;
		}
		retained.Splines = std::move(splineDescriptors);
		retained.RenderSceneInput.Splines =
			std::span<const Visuals::BuildingSplineRenderInput>(retained.Splines);
		return true;
	}
	/** Restricts retained spline appearances to current owned solid values and empty texture views. */
	bool SectionScheduler::HasOnlyRetainableSplineAppearances(
		const ::Services::Map::Buildings::BuildingMapSection& section,
		std::uint64_t sourceContentHash,
		const SectionScenePreparationResult& preparation) noexcept
	{
		for (const SplineRenderInputPreparationResult& spline : preparation.SplinePreparations)
		{
			for (const Visuals::BuildingSplineSurfaceRenderInput& surface : spline.SurfaceInputs)
			{
				if (surface.Appearance.Mode != Visuals::SplineSurfaceAppearanceMode::SolidColor ||
					!surface.Appearance.Texture.Pixels.empty())
				{
					REPORT_SECTION_FAILURE(section, sourceContentHash,
						SectionScenePreparationResult::Readiness::InvalidPreparationInputs, "retained spline assembly",
						surface.Appearance.Mode != Visuals::SplineSurfaceAppearanceMode::SolidColor
							? "spline appearance is not SolidColor" : "spline retains a borrowed texture span",
						static_cast<long long>(spline.PublicationOrdinal), -1, 0, 0,
						static_cast<long long>(&surface - spline.SurfaceInputs.data()), static_cast<int>(surface.Appearance.Mode));
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
		const ::Services::Map::Buildings::BuildingMapSection& section,
		const BuildingCandidatePublication& publication,
		Visuals::BuildingSplineRenderInput& output) const noexcept
	{
		output = {};
		const bool syntheticThinRibbon =
			GeometryMode == BuildingCandidateSplineGeometryMode::SyntheticThinRibbon;
		const bool validGeometryMode = syntheticThinRibbon ||
			GeometryMode == BuildingCandidateSplineGeometryMode::AuthoredProfiles;
		const bool validProfileStatus = syntheticThinRibbon
			? CurvePreparation.ProfileStatus == SplineProfileAcquisitionStatus::NotApplicable &&
				SurfaceInputs.size() == 1
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
			REPORT_SECTION_FAILURE(section, publication.ContentHash(),
				SectionScenePreparationResult::Readiness::InvalidPreparationInputs, "spline descriptor correspondence",
				PreparationStatus != Status::Composed ? "spline preparation is not Composed" :
				CurvePreparation.SourceStatus != BuildingCandidateSourceStatus::Complete ? "spline source is incomplete" :
				!validGeometryMode ? "unsupported spline geometry mode" : !validProfileStatus ? "invalid spline profile or ribbon inputs" :
				CurvePreparation.PreparationStatus != SplineCurvePreparationResult::CurveStatus::ConvertedWorldSpace
					? "spline curve is not ConvertedWorldSpace" :
				CurvePreparation.ConversionIssue != Visuals::SplineNumericalIssue::None ? "spline curve conversion issue" :
				CurvePreparation.RootStatus != SplineCurvePreparationResult::RootTransformStatus::Prepared
					? "spline root transform is not Prepared" :
				CurvePreparation.Segments.empty() ? "spline segments empty" : SurfaceInputs.empty() ? "spline surfaces empty" :
				!CurvePreparation.RootTransform.has_value() ? "spline root transform absent" : "invalid spline sampling pixel scale",
				static_cast<long long>(PublicationOrdinal), -1, 0, 0, -1, static_cast<int>(PreparationStatus));
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

	/** Stores Section configuration and constructs BuildingScheduler with the service-owned source manager. */
	SectionScheduler::SectionScheduler(
		double sectionSizeMapUnits,
		std::uint32_t sectionResolutionPixels,
		double spriteCaptureDensityPixelsPerMapUnit,
		std::filesystem::path buildingsCacheDirectory,
		Cache::SpriteSourceManager* spriteSourceManager)
		: sectionSizeMapUnits_(sectionSizeMapUnits)
		, sectionResolutionPixels_(sectionResolutionPixels)
		, diagnosticsDirectory_(buildingsCacheDirectory / "Diagnostics")
		, spriteSourceManager_(spriteSourceManager)
	{
		buildingScheduler_.reset(new BuildingScheduler(
			spriteSourceManager_, spriteSourceManager_ != nullptr ? spriteSourceManager_->DiskCache() : nullptr,
			spriteCaptureDensityPixelsPerMapUnit));
	}

	/** Destroys subordinates in reverse member order; native retirement remains service-directed. */
	SectionScheduler::~SectionScheduler() = default;

	/** Keeps the existing owner for equivalent Section content and resets genuine changes. */
	std::shared_ptr<const BuildingCandidatePublication> SectionScheduler::EnsureSectionJob(
		::Services::Map::Buildings::BuildingMapSection& section,
		const std::shared_ptr<const BuildingCandidatePublication>& publicationOwner)
	{
		if (publicationOwner == nullptr)
		{
			throw std::invalid_argument("A Section job requires a selected publication owner.");
		}
		if (!section.renderingState_)
		{
			section.InvalidatePreparingScene(publicationOwner->ContentHash());
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
			ResetSectionJobProgress(job, section);
			section.InvalidatePreparingScene(publicationOwner->ContentHash());
			job.publicationOwner = publicationOwner;
			return publicationOwner;
		}
		return retainedOwner;
	}

	/** Keeps only valid unfinished progress; failed or empty job shells are retired. */
	bool SectionScheduler::CanRetainIncompleteJobOnDemandWithdrawal(
		const ::Services::Map::Buildings::BuildingMapSection* section) const noexcept
	{
		if (section == nullptr || !section->renderingState_)
		{
			return false;
		}
		const auto& job = *static_cast<const SectionJob*>(section->renderingState_.get());
		return job.publicationOwner != nullptr && !job.GeometryFailed &&
			!job.Preparation.TerminalReadiness.has_value() &&
			(job.Preparation.Initialized || job.retainedScene != nullptr ||
				job.NumericalOperation != nullptr || job.CompletedImageReady || job.CompletedImageTransparent);
	}

	/** Checks sprite eligibility in constant time without replacing the selected generation. */
	bool SectionScheduler::NeedsInitialSpriteVisit(
		const ::Services::Map::Buildings::BuildingMapSection* section,
		const BuildingCandidatePublication& publication) noexcept
	{
		if (section == nullptr || !section->renderingState_)
		{
			return true;
		}
		const auto& job = *static_cast<const SectionJob*>(section->renderingState_.get());
		if (job.publicationOwner->ContentHash() != publication.ContentHash())
		{
			return true;
		}
		const auto& progress = job.Preparation;
		return job.retainedScene == nullptr && !progress.TerminalReadiness.has_value() &&
			(!progress.Initialized || progress.NextOccurrenceIndex < progress.InitialSpriteCount);
	}

	/** Releases operation state before the prepared scene while retaining its publication owner. */
	void SectionScheduler::ResetSectionJobProgress(
		SectionJob& job,
		::Services::Map::Buildings::BuildingMapSection& section) noexcept
	{
		section.DiscardPreparingScene();
		job.NumericalOperation.reset();
		job.Preparation = {};
		job.retainedScene.reset();
		job.GeometryCursor = {};
		job.CompletedImage.clear();
		job.TerminalSplineOmissions = {};
		job.CompletedImageReady = false;
		job.CompletedImageTransparent = false;
		job.GeometryComplete = false;
		job.GeometryFailed = false;
		job.AttemptEvidence.reset();
	}

	/** Transfers one finished attempt to the caller and clears its retained progress. */
	void SectionScheduler::FinishSectionJobAttempt(
		::Services::Map::Buildings::BuildingMapSection& section,
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
		ResetSectionJobProgress(job, section);
	}

	/** Advances available geometry before optional image work, independent of image demand. */
	bool SectionScheduler::PrepareSectionJobAttempt(
		::Services::Map::Buildings::BuildingMapSection& section,
		SectionJob& job,
		std::span<const SplinePolicy> splinePolicies,
		const SectionPreparationVisit& visit,
		bool renderSectionImages,
		SectionJobAdvanceResult& result)
	{
		if (job.GeometryFailed)
		{
			result.CurrentState = SectionJobAdvanceResult::State::TerminalFailure;
			return false;
		}
		const bool sceneReady = PrepareSectionJobSceneForAttempt(
			section, job, splinePolicies, visit, result);
		if (!sceneReady)
		{
			if (!visit.InitialSpritesOnly)
			{
				AdvanceSectionJobGeometry(section, job, visit.Deadline, result);
			}
			return false;
		}
		if (!AdvanceSectionJobGeometry(section, job, visit.Deadline, result))
		{
			return false;
		}
		if (!renderSectionImages)
		{
			result.AwaitingImageDemand = true;
			return false;
		}
		if (!job.publicationOwner->BorrowCandidateData().occurrences.empty())
		{
			return std::chrono::steady_clock::now() < visit.Deadline;
		}
		if (std::chrono::steady_clock::now() >= visit.Deadline)
		{
			return false;
		}
		CommitEmptySection(section, *job.AttemptEvidence);
		++result.WorkUnitsConsumed;
		FinishSectionJobAttempt(section, job, result);
		return false;
	}

	/** Advances the Section-owned geometry cursor and atomically promotes only a complete replacement. */
	bool SectionScheduler::AdvanceSectionJobGeometry(
		::Services::Map::Buildings::BuildingMapSection& section,
		SectionJob& job,
		std::chrono::steady_clock::time_point deadline,
		SectionJobAdvanceResult& result)
	{
		if (job.GeometryFailed)
		{
			result.CurrentState = SectionJobAdvanceResult::State::TerminalFailure;
			return false;
		}
		if (job.GeometryComplete)
		{
			return true;
		}
		if (section.HasCompletedSceneForContentHash(job.publicationOwner->ContentHash()))
		{
			job.GeometryComplete = true;
			return true;
		}
		if (job.retainedScene == nullptr || std::chrono::steady_clock::now() >= deadline)
		{
			return false;
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		Performance::Timer continuation(&Performance::Game().Continuation);
#endif
		const BuildingCandidateData candidateData = job.publicationOwner->BorrowCandidateData();
		const std::size_t contributionCount = candidateData.occurrences.size();
		auto* output = section.BeginPreparingScene(
			job.publicationOwner->ContentHash(), contributionCount);
		const std::size_t drawableTriangleCount = output->drawableGpuTriangleCount_;
		const auto step = Visuals::BuildingGeometryPreparation::Advance(
			job.retainedScene->RenderSceneInput,
			job.publicationOwner->SectionRegion(),
			section.GetOrCreateGeometryContext(),
			*output,
			job.GeometryCursor,
			deadline);
		result.WorkUnitsConsumed += step.WorkUnitsConsumed;
		if (output->drawableGpuTriangleCount_ != drawableTriangleCount)
		{
			section.MarkGpuPresentationChanged();
		}
		if (step.CurrentStatus == Visuals::BuildingGeometryPreparation::Status::Pending)
		{
			return false;
		}
		if (step.CurrentStatus != Visuals::BuildingGeometryPreparation::Status::Complete &&
			step.CurrentStatus != Visuals::BuildingGeometryPreparation::Status::CompleteWithOmissions)
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			auto failure = job.GeometryCursor.FailureDetails();
			failure.PublicationCount = static_cast<long long>(candidateData.occurrences.size());
			failure.StatusName = step.CurrentStatus == Visuals::BuildingGeometryPreparation::Status::InvalidInputs
				? "InvalidInputs" : "NumericalFailure";
			section.ReportFailure(job.publicationOwner->ContentHash(), static_cast<int>(step.CurrentStatus), failure);
#endif
			job.GeometryFailed = true;
			result.CurrentState = SectionJobAdvanceResult::State::TerminalFailure;
			return false;
		}
		if (!section.CompletePreparingScene(job.publicationOwner->ContentHash()))
		{
			job.GeometryFailed = true;
			result.CurrentState = SectionJobAdvanceResult::State::TerminalFailure;
			return false;
		}
		job.GeometryComplete = true;
		const auto& spriteOutcomes = job.Preparation.SpritePreparation.OccurrenceOutcomes;
		for (std::size_t ordinal = 0; ordinal < spriteOutcomes.size(); ++ordinal)
		{
			if (spriteOutcomes[ordinal].PreparationStatus !=
				KnownSpritePreparationResult::OccurrenceOutcome::Status::TerminalSpriteGeometryFailure)
			{
				continue;
			}
			if (result.TerminalSplineOmissions.OrdinarySpriteGeometryFailureCount == 0)
			{
				result.TerminalSplineOmissions.FirstOrdinarySpriteFailureOrdinal = ordinal;
			}
			++result.TerminalSplineOmissions.OrdinarySpriteGeometryFailureCount;
		}
		const auto& outcomes = job.GeometryCursor.ContributorResults().SplineSurfaces;
		for (const auto& outcome : outcomes)
		{
			if (outcome.FailureStage != Visuals::BuildingVisualsFailureStage::Occlusion ||
				outcome.OcclusionStatus != Visuals::SplineOcclusionStatus::NumericalFailure)
			{
				continue;
			}
			++result.TerminalSplineOmissions.NumericalClippingFailureCount;
			if (result.TerminalSplineOmissions.HasNumericalClippingFailure || job.retainedScene == nullptr ||
				outcome.ContributorIndex >= job.retainedScene->RenderSceneInput.Splines.size())
			{
				continue;
			}
			const auto& spline = job.retainedScene->RenderSceneInput.Splines[outcome.ContributorIndex];
			result.TerminalSplineOmissions.HasNumericalClippingFailure = true;
			result.TerminalSplineOmissions.FirstFailedSectionLocalOrdinal = spline.SectionLocalOrdinal;
			result.TerminalSplineOmissions.FirstFailedDescriptorIndex = outcome.ContributorIndex;
			result.TerminalSplineOmissions.FirstFailedSurfaceIndex = outcome.SurfaceIndex;
			result.TerminalSplineOmissions.FirstOcclusionStatus = outcome.OcclusionStatus;
		}
		if (std::none_of(
			output->SplineSurfaces().begin(), output->SplineSurfaces().end(),
			[](const auto& surface) { return surface.IsTextured; }))
		{
			job.retainedScene.reset();
		}
		const auto& omittedOrdinals = output->TerminalSplineOmissionOrdinals();
		if (!omittedOrdinals.empty() || result.TerminalSplineOmissions.HasNumericalClippingFailure ||
			result.TerminalSplineOmissions.OrdinarySpriteGeometryFailureCount != 0)
		{
			result.TerminalSplineOmissions.SplineContributorCount = omittedOrdinals.size();
			if (!omittedOrdinals.empty())
			{
				result.TerminalSplineOmissions.FirstSectionLocalOrdinal = omittedOrdinals.front();
				result.TerminalSplineOmissions.FailureStage = result.TerminalSplineOmissions.HasNumericalClippingFailure
					? Visuals::BuildingVisualsFailureStage::Occlusion
					: Visuals::BuildingVisualsFailureStage::SurfacePreparation;
				result.TerminalSplineOmissions.AlternativeStatus =
					Visuals::BuildingSplineAlternativeStatus::NoDrawableAlternative;
			}
			result.TerminalSplineOmissions.Reason = output->TerminalSplineOmissionReason();
			if (omittedOrdinals.empty() && result.TerminalSplineOmissions.HasNumericalClippingFailure)
			{
				result.TerminalSplineOmissions.Reason =
					Visuals::BuildingSplineOmissionReason::NumericalClippingFailure;
			}
		}
		return true;
	}

	/** Resumes Section-owned occurrence preparation or accepts an already complete retained scene. */
	bool SectionScheduler::PrepareSectionJobSceneForAttempt(
		::Services::Map::Buildings::BuildingMapSection& section,
		SectionJob& job,
		std::span<const SplinePolicy> splinePolicies,
		const SectionPreparationVisit& visit,
		SectionJobAdvanceResult& result)
	{
		if (job.AttemptEvidence == std::nullopt)
		{
			job.AttemptEvidence.emplace();
		}
		if (job.retainedScene == nullptr)
		{
			if (TryReuseCompletedSceneWithoutResources(section, job))
			{
				result.SceneReadiness = SectionScenePreparationResult::Readiness::CompleteSceneInputs;
				job.AttemptEvidence->SceneReadiness = result.SceneReadiness;
				return true;
			}
			if (std::chrono::steady_clock::now() >= visit.Deadline)
			{
				return false;
			}
			if (!PrepareRetainedSceneForAttempt(section, job, splinePolicies, visit, result))
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
		const SectionPreparationVisit& visit,
		SectionJobAdvanceResult& result)
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const auto started = std::chrono::steady_clock::now();
#endif
		bool deadlineDeferred = false;
		const auto readiness = PrepareRetainedSectionScene(
			section, job, splinePolicies, visit, deadlineDeferred);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		job.AttemptEvidence->Timing.ScenePreparationMs +=
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
#endif
		++result.WorkUnitsConsumed;
		result.HasSpriteRetries = job.Preparation.SpriteRetryHead.has_value();
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
				ResetSectionJobProgress(job, section);
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
		if (job.AttemptEvidence.has_value() && job.AttemptEvidence->NumericalStatus.has_value())
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
		if (section.completedScene_ != nullptr &&
			job.NumericalOperation->RequiresSceneInput(*section.completedScene_))
		{
			if (job.retainedScene == nullptr)
			{
				ResetSectionJobProgress(job, section);
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
		if (section.completedScene_ == nullptr ||
			!section.HasCompletedSceneForContentHash(job.publicationOwner->ContentHash()))
		{
			result.CurrentState = SectionJobAdvanceResult::State::RestartRequired;
			return false;
		}
		const auto step = job.NumericalOperation->Advance(
			sceneInput,
			*section.completedScene_,
			section.GetOrCreateVisualsContext(),
			deadline);
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
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			section.ReportFailure(job.publicationOwner->ContentHash(), static_cast<int>(step.TerminalStatus.value()),
				job.NumericalOperation->FailureDetails());
#endif
			job.AttemptEvidence->TerminalSplineOmissions = {};
			FinishSectionJobAttempt(section, job, result);
			return false;
		}
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
		FinishSectionJobAttempt(section, job, result);
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
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
				section.ReportFailure(job.publicationOwner->ContentHash(),
					static_cast<int>(SectionImageCommitResult::Status::NumericalRenderingFailed),
					job.NumericalOperation->FailureDetails());
#endif
				evidence.CommitStatus = SectionImageCommitResult::Status::NumericalRenderingFailed;
				evidence.TerminalSplineOmissions = {};
				FinishSectionJobAttempt(section, job, result);
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
			FinishSectionJobAttempt(section, job, result);
			return false;
		}
		return true;
	}

	/** Advances the Section's already-ensured job until the shared deadline. */
	SectionScheduler::SectionJobAdvanceResult SectionScheduler::AdvanceSectionJob(
		::Services::Map::Buildings::BuildingMapSection& section,
		std::span<const SplinePolicy> splinePolicies,
		const SectionPreparationVisit& visit,
		bool renderSectionImages)
	{
		SectionJobAdvanceResult result;
		if (std::chrono::steady_clock::now() >= visit.Deadline)
		{
			return result;
		}
		SectionJob& job = *static_cast<SectionJob*>(section.renderingState_.get());
		if (!PrepareSectionJobAttempt(
			section, job, splinePolicies, visit, renderSectionImages, result))
		{
			return result;
		}
		if (AdvanceSectionJobNumerical(section, job, visit.Deadline, result))
		{
			CommitSectionJobImage(section, job, visit.Deadline, result);
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

	/** Delegates explicit cache deletion to the service-owned manager's disk cache. */
	bool SectionScheduler::ClearCache()
	{
		return spriteSourceManager_ != nullptr && spriteSourceManager_->DiskCache() != nullptr &&
			spriteSourceManager_->DiskCache()->ClearCache();
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

	/** Copies aligned candidate/source facts and allocates empty sparse-sprite indices per ordinal. */
	bool SectionScheduler::InitializeKnownSpriteOccurrences(
		const ::Services::Map::Buildings::BuildingMapSection& section,
		std::uint64_t sourceContentHash,
		const BuildingCandidateData& candidateData,
		const BuildingCandidateSourceData& sourceData,
		KnownSpritePreparationResult& output)
	{
		if (sourceData.records.size() != candidateData.occurrences.size())
		{
			REPORT_SECTION_FAILURE(section, sourceContentHash, SectionScenePreparationResult::Readiness::InvalidPublication,
				"source publication", "candidate occurrence and source record counts differ",
				-1, -1, candidateData.occurrences.size(), sourceData.records.size());
			return false;
		}
		output.OccurrenceOutcomes.reserve(candidateData.occurrences.size());
		for (std::size_t index = 0; index < candidateData.occurrences.size(); ++index)
		{
			if (candidateData.occurrences[index].publicationOrdinal != index)
			{
				REPORT_SECTION_FAILURE(section, sourceContentHash, SectionScenePreparationResult::Readiness::InvalidPublication,
					"source publication", "publication ordinal does not match source record slot",
					static_cast<long long>(candidateData.occurrences[index].publicationOrdinal),
					-1, index, candidateData.occurrences[index].publicationOrdinal);
				output.OccurrenceOutcomes.clear();
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

	/** Copies supported geometry values from one acquired authored alternative. */
	bool SectionScheduler::PrepareSplineProfileAlternative(
		const SplineProfileAlternative& source,
		Visuals::BuildingSplineSurfaceRenderInput& output)
	{
		if (source.Status != SplineProfileAlternativeStatus::Complete)
		{
			return false;
		}
		if (!source.HasBounds)
		{
			return false;
		}
		if (!CopySplineSurfaceValues(source, output))
		{
			return false;
		}
		if (source.ForwardAxis != 0 || source.CrossSection.StartRoll != 0.0 ||
			source.CrossSection.EndRoll != 0.0 ||
			source.CrossSection.StartScale != source.CrossSection.EndScale ||
			source.CrossSection.StartOffset != source.CrossSection.EndOffset)
		{
			return false;
		}
		return true;
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
		output.GeometryMode = source->splineGeometryMode;
		PrepareSplineCurve(*source, output.CurvePreparation);
		if (!CheckSplineSourcePrerequisites(*source, output) ||
			!CheckSplinePolicyPrerequisites(*source, resolvedPolicyIndex, resolvedPolicy, output) ||
			!TryPrepareSplineSamplingPolicy(output))
		{
			return;
		}
		const bool surfacesPrepared = output.GeometryMode ==
			BuildingCandidateSplineGeometryMode::SyntheticThinRibbon
			? AppendSyntheticThinRibbon(*resolvedPolicy,
				source->syntheticThinRibbonHalfWidthMapUnits, output)
			: AppendSplineRenderSurfaces(*source, *resolvedPolicy, output);
		if (!surfacesPrepared)
		{
			return;
		}
		output.PreparationStatus = SplineRenderInputPreparationResult::Status::Composed;
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
			std::isfinite(source.syntheticThinRibbonHalfWidthMapUnits) &&
			source.syntheticThinRibbonHalfWidthMapUnits > 0.0)
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

	/** Uses legacy minimum-map-resolution density without depending on Section image dimensions. */
	bool SectionScheduler::TryPrepareSplineSamplingPolicy(
		SplineRenderInputPreparationResult& output) const
	{
		using Status = SplineRenderInputPreparationResult::Status;
		constexpr double minimumMetersPerPixel = 0.125;
		constexpr double mapUnitsPerMeter = 100.0;
		constexpr double pixelsPerUnit = 1.0 / (mapUnitsPerMeter * minimumMetersPerPixel);
		if (!std::isfinite(pixelsPerUnit) || pixelsPerUnit <= 0.0)
		{
			output.PreparationStatus = Status::InvalidSamplingDensity;
			return false;
		}
		output.SamplingPolicy = Visuals::SplineSamplingPolicy{};
		output.SamplingPolicy.PixelsPerUnit = pixelsPerUnit;
		return true;
	}

	/** Appends eligible authored surfaces directly in their source alternative order. */
	bool SectionScheduler::AppendSplineRenderSurfaces(
		const BuildingCandidateSourceRecord& source,
		const SplinePolicy& policy,
		SplineRenderInputPreparationResult& output)
	{
		using Status = SplineRenderInputPreparationResult::Status;
		if (source.splineProfiles != nullptr)
		{
			for (const SplineProfileAlternative& alternative : source.splineProfiles->Alternatives)
			{
				Visuals::BuildingSplineSurfaceRenderInput surface;
				if (!PrepareSplineProfileAlternative(alternative, surface))
				{
					continue;
				}
				surface.Appearance.Mode = Visuals::SplineSurfaceAppearanceMode::SolidColor;
				surface.Appearance.SolidRgb = policy.SolidRgb;
				surface.Appearance.Opacity = policy.Opacity;
				output.SurfaceInputs.push_back(std::move(surface));
			}
		}
		if (output.SurfaceInputs.empty())
		{
			output.PreparationStatus = Status::NoPreparedSurface;
			return false;
		}
		return true;
	}

	/** Preserves acquisition outcomes and prepares one sprite from the readiness payload. */
	void SectionScheduler::PrepareSpriteRequirement(
		::Services::Map::Buildings::BuildingMapSection& section,
		SectionJob& job,
		const BuildingCandidateSourceRecord& requirement,
		std::size_t ordinal,
		bool sourcePreparationBlocked,
		bool updatePreparationCounts)
	{
		auto& output = job.Preparation.SpritePreparation;
		auto& occurrenceOutcome = output.OccurrenceOutcomes[ordinal];
		const auto& ownedKey = *job.Preparation.Occurrences[ordinal].SpriteKey;
		if (buildingScheduler_ == nullptr)
		{
			if (updatePreparationCounts)
			{
				++output.UnavailableCount;
			}
			occurrenceOutcome.PreparationStatus =
				KnownSpritePreparationResult::OccurrenceOutcome::Status::TemporarilyUnavailableSprite;
			return;
		}
		std::shared_ptr<const Cache::ProcessedSpritePayload> payload;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		++Performance::Game().ResourceLookups;
#endif
		const BuildingScheduler::RequirementStatus readiness =
			buildingScheduler_->EnsureSpriteReady(ownedKey, payload);
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
		if (payload == nullptr ||
			payload->Format() != Cache::ProcessedSpritePayload::PixelFormat::StraightRgba8SrgbDisplay)
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
			section,
			job,
			ordinal,
			requirement,
			std::move(payload),
			occurrenceOutcome,
			updatePreparationCounts,
			!sourcePreparationBlocked &&
			!section.HasCompletedSceneForContentHash(job.publicationOwner->ContentHash()));
	}

	/** Projects and clips one ready sprite directly into its authoritative Section occurrence. */
	void SectionScheduler::AppendReadySpriteInput(
		::Services::Map::Buildings::BuildingMapSection& section,
		SectionJob& job,
		std::size_t ordinal,
		const BuildingCandidateSourceRecord& requirement,
		std::shared_ptr<const Cache::ProcessedSpritePayload> payloadOwner,
		KnownSpritePreparationResult::OccurrenceOutcome& occurrenceOutcome,
		bool updatePreparationCounts,
		bool retainGeometry)
	{
		auto& output = job.Preparation.SpritePreparation;
		const Cache::ProcessedSpritePayload& payload = *payloadOwner;
		if (requirement.spritePass != BuildingCandidateSpritePass::FirstPass &&
			requirement.spritePass != BuildingCandidateSpritePass::SecondPass)
		{
			occurrenceOutcome.PreparationStatus =
				KnownSpritePreparationResult::OccurrenceOutcome::Status::UnknownSpritePass;
			return;
		}
		if (retainGeometry)
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			Performance::Timer preparation(Performance::ReadyGeometrySample(), &Performance::Game().Indivisible);
#endif
			Visuals::SpriteWorldQuad worldQuad;
			if (!Visuals::SpriteGeometry::BuildWorldQuad(
				requirement.placement, payload.CaptureSize(), payload.Bounds(), payload.Projection(), worldQuad))
			{
				if (requirement.spritePass == BuildingCandidateSpritePass::SecondPass)
				{
					occurrenceOutcome.PreparationStatus =
						KnownSpritePreparationResult::OccurrenceOutcome::Status::TerminalSpriteGeometryFailure;
				}
				else if (updatePreparationCounts)
				{
					++output.UnresolvedSpriteGeometryCount;
				}
				if (requirement.spritePass != BuildingCandidateSpritePass::SecondPass)
				{
					occurrenceOutcome.PreparationStatus =
						KnownSpritePreparationResult::OccurrenceOutcome::Status::UnresolvedSpriteGeometry;
				}
				return;
			}
			auto* geometry = section.BeginPreparingScene(
				job.publicationOwner->ContentHash(), job.Preparation.Occurrences.size());
			if (geometry->contributionStates_[ordinal] !=
				Visuals::PreparedBuildingGeometry::ContributionStatus::NotProcessed)
			{
				occurrenceOutcome.PreparationStatus =
					KnownSpritePreparationResult::OccurrenceOutcome::Status::UnresolvedSpriteGeometry;
				return;
			}
			Visuals::PreparedBuildingGeometry::SpriteOccurrence occurrence;
			occurrence.SectionLocalOrdinal = ordinal;
			occurrence.SourceWorldQuad = worldQuad;
			occurrence.Payload = std::move(payloadOwner);
			std::array<::Services::Map::Buildings::TexturedTriangle, 6> clippedTriangles{};
			std::size_t clippedTriangleCount = 0;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			++Performance::Game().ClipAttempts;
#endif
			const bool sectionClipSucceeded = Visuals::BuildingGeometryPreparation::TryClipSpriteToSection(
				worldQuad, job.publicationOwner->SectionRegion(), clippedTriangles, clippedTriangleCount);
			if (!sectionClipSucceeded)
			{
				clippedTriangleCount = 0;
			}
			Visuals::PreparedBuildingGeometry::PrepareSpriteDrawLayout(
				occurrence, clippedTriangles, clippedTriangleCount);
			occurrence.SourceKey = std::move(*job.Preparation.Occurrences[ordinal].SpriteKey);
			occurrence.IsOccluder = requirement.spritePass == BuildingCandidateSpritePass::FirstPass &&
				Visuals::SpriteGeometry::TryGetCaptureBoundsTopHeight(
					requirement.placement, payload.Bounds(), occurrence.OccluderTopHeight);
			auto& destination = requirement.spritePass == BuildingCandidateSpritePass::SecondPass
				? geometry->overlayLastSprites_ : geometry->ordinarySprites_;
			const bool hasDrawableGeometry = occurrence.SectionTriangleCount != 0;
			destination.push_back(std::move(occurrence));
			if (!geometry->TrySetSpriteOccurrenceLocation(
				ordinal, requirement.spritePass == BuildingCandidateSpritePass::SecondPass, destination.size() - 1))
			{
				destination.pop_back();
				geometry->TrySetContributionStatus(ordinal,
					Visuals::PreparedBuildingGeometry::ContributionStatus::Failed);
				occurrenceOutcome.PreparationStatus =
					KnownSpritePreparationResult::OccurrenceOutcome::Status::UnresolvedSpriteGeometry;
				return;
			}
			const bool clippedToSection = clippedTriangleCount != 0;
			const auto contributionStatus = !sectionClipSucceeded
				? Visuals::PreparedBuildingGeometry::ContributionStatus::TerminalOmission
				: (hasDrawableGeometry
					? Visuals::PreparedBuildingGeometry::ContributionStatus::Prepared
					: Visuals::PreparedBuildingGeometry::ContributionStatus::GeometricOmission);
			if (!geometry->TrySetContributionStatus(ordinal, contributionStatus))
			{
				occurrenceOutcome.PreparationStatus =
					KnownSpritePreparationResult::OccurrenceOutcome::Status::UnresolvedSpriteGeometry;
				return;
			}
			if (clippedToSection)
			{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
				++Performance::Game().DrawableSprites;
				if (!geometry->diagnosticRecordedPendingSprite_)
				{
					geometry->diagnosticRecordedPendingSprite_ = true;
					Performance::RecordDrawable(section.GetGridX(), section.GetGridY(),
						geometry->SourceContentHash(), geometry->DiagnosticGeneration());
				}
#endif
				section.MarkGpuPresentationChanged();
			}
			occurrenceOutcome.PreparationStatus = !sectionClipSucceeded
				? KnownSpritePreparationResult::OccurrenceOutcome::Status::TerminalSpriteGeometryFailure
				: KnownSpritePreparationResult::OccurrenceOutcome::Status::ReadySprite;
		}
	}

}

#undef REPORT_SECTION_FAILURE

#endif // MODLOADER_CLIENT_BUILD
