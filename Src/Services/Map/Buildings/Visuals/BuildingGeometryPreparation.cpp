// Implements deadline-resumable, renderer-independent Building Map geometry preparation.
#if defined(MODLOADER_CLIENT_BUILD)

#include "BuildingGeometryPreparation.h"

#include "BuildingVisuals.h"
#include "SpriteGeometry.h"
#include "SplineGeometry.h"
#include "SplineOcclusionGeometry.h"
#include "SplineSurfaceGeometry.h"

#include <chrono>
#include <cmath>
#include <utility>

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
#define RECORD_GEOMETRY_FAILURE(...) (cursor.Failure = SectionFailureDetails{__VA_ARGS__})
#else
#define RECORD_GEOMETRY_FAILURE(...) ((void)0)
#endif

namespace Services::Map::Buildings::Visuals
{
	/** Resets only a new or terminal cursor; callers own and reset the output separately. */
	void BuildingGeometryPreparation::PreparationCursor::Reset() noexcept
	{
		if (Initialized && TerminalStatus == Status::Pending)
		{
			return;
		}
		CurrentPhase = Phase::OrdinarySprites;
		OrdinarySpriteIndex = 0;
		SplineIndex = 0;
		SplineSurfaceIndex = 0;
		OverlaySpriteIndex = 0;
		PendingOpacity = 0;
		Initialized = false;
		SplineSamplingPrepared = false;
		HasAcceptedOmissions = false;
		TerminalStatus = Status::Pending;
		Results = {};
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		Failure = {};
#endif
	}

	/** Returns the cursor-owned positional outcomes without copying their vectors. */
	const BuildingVisualsResult& BuildingGeometryPreparation::PreparationCursor::ContributorResults() const noexcept
	{
		return Results;
	}

	/** Returns Pending until a terminal preparation result has been established. */
	BuildingGeometryPreparation::Status
		BuildingGeometryPreparation::PreparationCursor::CurrentStatus() const noexcept
	{
		return TerminalStatus;
	}

	/** Advances one numerical unit at a time and retains completed geometry across deadline yields. */
	BuildingGeometryPreparation::StepResult BuildingGeometryPreparation::Advance(
		const BuildingRenderSceneInput& scene,
		BuildingVisualsContext& scratch,
		PreparedBuildingGeometry& output,
		PreparationCursor& cursor,
		std::chrono::steady_clock::time_point deadline)
	{
		if (cursor.TerminalStatus != Status::Pending)
		{
			return {cursor.TerminalStatus, 0};
		}
		std::size_t workUnitsConsumed = 0;
		try
		{
			if (!cursor.Initialized && !Initialize(scene, scratch, output, cursor))
			{
				return Terminate(cursor, Status::InvalidInputs, 0);
			}
			while (std::chrono::steady_clock::now() < deadline)
			{
				std::size_t stepWorkUnits = 0;
				const Status status = AdvanceOne(scene, scratch, output, cursor, stepWorkUnits);
				workUnitsConsumed += stepWorkUnits;
				if (status == Status::Complete)
				{
					return Complete(output, cursor, workUnitsConsumed);
				}
				if (status != Status::Pending)
				{
					return Terminate(cursor, status, workUnitsConsumed);
				}
			}
			return {Status::Pending, workUnitsConsumed};
		}
		catch (...)
		{
			RECORD_GEOMETRY_FAILURE(cursor.Initialized ? "geometry preparation" : "geometry initialization",
				"exception in geometry operation", -1, -1, 0, 0, -1,
				static_cast<int>(cursor.CurrentPhase));
			return Terminate(cursor, Status::NumericalFailure, workUnitsConsumed);
		}
	}

	/** Advances one phase boundary or one sprite, index, spline or terminal unit. */
	BuildingGeometryPreparation::Status BuildingGeometryPreparation::AdvanceOne(
		const BuildingRenderSceneInput& scene,
		BuildingVisualsContext& scratch,
		PreparedBuildingGeometry& output,
		PreparationCursor& cursor,
		std::size_t& workUnitsConsumed)
	{
		workUnitsConsumed = 0;
		if (cursor.CurrentPhase == PreparationCursor::Phase::OrdinarySprites)
		{
			if (cursor.OrdinarySpriteIndex == scene.OrdinarySprites.size())
			{
				cursor.CurrentPhase = PreparationCursor::Phase::OcclusionIndex;
				return Status::Pending;
			}
			const auto status = PrepareSprite(scene.OrdinarySprites[cursor.OrdinarySpriteIndex],
				cursor.OrdinarySpriteIndex, false, scratch, output, cursor);
			++cursor.OrdinarySpriteIndex;
			workUnitsConsumed = 1;
			return status;
		}
		if (cursor.CurrentPhase == PreparationCursor::Phase::OcclusionIndex)
		{
			cursor.Results.OcclusionPreparationStatus = BuildingVisuals::PrepareOcclusionIndex(scratch);
			cursor.CurrentPhase = PreparationCursor::Phase::Splines;
			workUnitsConsumed = 1;
			if (cursor.Results.OcclusionPreparationStatus != SplineOcclusionStatus::Complete)
			{
				RECORD_GEOMETRY_FAILURE("occlusion index construction", "PrepareOcclusionIndex did not complete",
					-1, -1, 0, 0, -1, static_cast<int>(cursor.Results.OcclusionPreparationStatus));
				return Status::NumericalFailure;
			}
			return Status::Pending;
		}
		return AdvanceRemainingPhase(scene, scratch, output, cursor, workUnitsConsumed);
	}

	/** Advances spline, overlay and terminal phases after ordinary occluders are indexed. */
	BuildingGeometryPreparation::Status BuildingGeometryPreparation::AdvanceRemainingPhase(
		const BuildingRenderSceneInput& scene,
		BuildingVisualsContext& scratch,
		PreparedBuildingGeometry& output,
		PreparationCursor& cursor,
		std::size_t& workUnitsConsumed)
	{
		if (cursor.CurrentPhase == PreparationCursor::Phase::Splines)
		{
			if (cursor.SplineIndex == scene.Splines.size())
			{
				cursor.CurrentPhase = PreparationCursor::Phase::OverlayLastSprites;
				return Status::Pending;
			}
			workUnitsConsumed = 1;
			return AdvanceSpline(scene.Splines[cursor.SplineIndex], cursor.SplineIndex,
				scratch, output, cursor);
		}
		if (cursor.CurrentPhase == PreparationCursor::Phase::OverlayLastSprites)
		{
			if (cursor.OverlaySpriteIndex == scene.OverlayLastSprites.size())
			{
				cursor.CurrentPhase = PreparationCursor::Phase::Complete;
				return Status::Pending;
			}
			const auto status = PrepareSprite(scene.OverlayLastSprites[cursor.OverlaySpriteIndex],
				cursor.OverlaySpriteIndex, true, scratch, output, cursor);
			++cursor.OverlaySpriteIndex;
			workUnitsConsumed = 1;
			return status;
		}
		return Status::Complete;
	}

	/** Validates fixed Section ownership and prepares scratch without retaining scene input views. */
	bool BuildingGeometryPreparation::Initialize(
		const BuildingRenderSceneInput& scene,
		BuildingVisualsContext& scratch,
		PreparedBuildingGeometry& output,
		PreparationCursor& cursor)
	{
		if (output.complete_ || !output.splineSurfaces_.empty() ||
			!ValidateContributionOwnership(scene, output, cursor))
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			if (output.complete_ || !output.splineSurfaces_.empty())
			{
				RECORD_GEOMETRY_FAILURE("geometry initialization", output.complete_
					? "output is already complete" : "output already contains spline surfaces",
					-1, -1, 0, output.splineSurfaces_.size());
			}
#endif
			return false;
		}
		scratch.preparedSplines_.clear();
		scratch.preparedSplines_.resize(scene.Splines.size());
		scratch.occluders_.clear();
		scratch.occlusionIndex_ = SplineOcclusionIndex{};
		if (!SeedPreparedOccluders(output, scratch))
		{
			RECORD_GEOMETRY_FAILURE("geometry initialization", "SeedPreparedOccluders failed");
			return false;
		}
		cursor.Results.OrdinarySprites.reserve(scene.OrdinarySprites.size());
		cursor.Results.SplineSurfaces.reserve(scene.Splines.size());
		cursor.Results.OverlayLastSprites.reserve(scene.OverlayLastSprites.size());
		cursor.Initialized = true;
		return true;
	}

	/** Validates all source ordinals against the caller-established publication cardinality. */
	bool BuildingGeometryPreparation::ValidateContributionOwnership(
		const BuildingRenderSceneInput& scene,
		const PreparedBuildingGeometry& output,
		PreparationCursor& cursor)
	{
		const std::size_t count = output.contributionStates_.size();
		std::vector<const BuildingSpriteRenderInput*> ordinaryInputs(count, nullptr);
		std::vector<const BuildingSplineRenderInput*> splineInputs(count, nullptr);
		std::vector<const BuildingSpriteRenderInput*> overlayInputs(count, nullptr);
		return IndexSceneInputs(scene, ordinaryInputs, splineInputs, overlayInputs, cursor) &&
			ValidatePreparedSpriteRecords(output, ordinaryInputs, overlayInputs, cursor) &&
			ValidateSceneOccurrenceStates(output, ordinaryInputs, splineInputs, overlayInputs, cursor);
	}

	/** Indexes the call-local views and rejects duplicate contributors across all three passes. */
	bool BuildingGeometryPreparation::IndexSceneInputs(
		const BuildingRenderSceneInput& scene,
		std::vector<const BuildingSpriteRenderInput*>& ordinaryInputs,
		std::vector<const BuildingSplineRenderInput*>& splineInputs,
		std::vector<const BuildingSpriteRenderInput*>& overlayInputs,
		PreparationCursor& cursor)
	{
		for (const auto& sprite : scene.OrdinarySprites)
		{
			if (sprite.SectionLocalOrdinal >= ordinaryInputs.size() ||
				ordinaryInputs[static_cast<std::size_t>(sprite.SectionLocalOrdinal)] != nullptr)
			{
				RECORD_GEOMETRY_FAILURE("geometry initialization / ordinary ownership",
					sprite.SectionLocalOrdinal >= ordinaryInputs.size()
						? "ordinal outside contribution slots" : "duplicate ordinal",
					static_cast<long long>(sprite.SectionLocalOrdinal),
					static_cast<long long>(&sprite - scene.OrdinarySprites.data()),
					ordinaryInputs.size(), sprite.SectionLocalOrdinal);
				return false;
			}
			ordinaryInputs[static_cast<std::size_t>(sprite.SectionLocalOrdinal)] = &sprite;
		}
		for (const auto& spline : scene.Splines)
		{
			if (spline.SectionLocalOrdinal >= splineInputs.size() ||
				splineInputs[static_cast<std::size_t>(spline.SectionLocalOrdinal)] != nullptr ||
				ordinaryInputs[static_cast<std::size_t>(spline.SectionLocalOrdinal)] != nullptr)
			{
				RECORD_GEOMETRY_FAILURE("geometry initialization / spline ownership",
					spline.SectionLocalOrdinal >= splineInputs.size()
						? "ordinal outside contribution slots" : "ordinal already owned",
					static_cast<long long>(spline.SectionLocalOrdinal),
					static_cast<long long>(&spline - scene.Splines.data()),
					splineInputs.size(), spline.SectionLocalOrdinal);
				return false;
			}
			splineInputs[static_cast<std::size_t>(spline.SectionLocalOrdinal)] = &spline;
		}
		return IndexOverlayInputs(scene, overlayInputs, ordinaryInputs, splineInputs, cursor);
	}

	/** Indexes overlay sprite views and rejects ordinals already owned by another pass. */
	bool BuildingGeometryPreparation::IndexOverlayInputs(
		const BuildingRenderSceneInput& scene,
		std::vector<const BuildingSpriteRenderInput*>& overlayInputs,
		const std::vector<const BuildingSpriteRenderInput*>& ordinaryInputs,
		const std::vector<const BuildingSplineRenderInput*>& splineInputs,
		PreparationCursor& cursor)
	{
		for (const auto& sprite : scene.OverlayLastSprites)
		{
			if (sprite.SectionLocalOrdinal >= overlayInputs.size())
			{
				RECORD_GEOMETRY_FAILURE("geometry initialization / overlay ownership", "ordinal outside contribution slots",
					static_cast<long long>(sprite.SectionLocalOrdinal),
					static_cast<long long>(&sprite - scene.OverlayLastSprites.data()),
					overlayInputs.size(), sprite.SectionLocalOrdinal);
				return false;
			}
			const auto ordinal = static_cast<std::size_t>(sprite.SectionLocalOrdinal);
			if (overlayInputs[ordinal] != nullptr ||
				ordinaryInputs[ordinal] != nullptr || splineInputs[ordinal] != nullptr)
			{
				RECORD_GEOMETRY_FAILURE("geometry initialization / overlay ownership", "ordinal already owned",
					static_cast<long long>(ordinal), static_cast<long long>(&sprite - scene.OverlayLastSprites.data()));
				return false;
			}
			overlayInputs[ordinal] = &sprite;
		}
		return true;
	}

	/** Confirms progressive sprite geometry has one matching input key and pass occurrence. */
	bool BuildingGeometryPreparation::ValidatePreparedSpriteRecords(
		const PreparedBuildingGeometry& output,
		const std::vector<const BuildingSpriteRenderInput*>& ordinaryInputs,
		const std::vector<const BuildingSpriteRenderInput*>& overlayInputs,
		PreparationCursor& cursor)
	{
		std::vector<bool> owned(output.contributionStates_.size(), false);
		for (const auto& occurrence : output.ordinarySprites_)
		{
			const auto ordinal = occurrence.SectionLocalOrdinal;
			if (!MatchesPreparedSprite(ordinal, occurrence, ordinaryInputs, owned, output, cursor))
			{
				return false;
			}
		}
		for (const auto& occurrence : output.overlayLastSprites_)
		{
			const auto ordinal = occurrence.SectionLocalOrdinal;
			if (ordinal >= overlayInputs.size())
			{
				RECORD_GEOMETRY_FAILURE("geometry initialization / progressive overlay validation",
					"ordinal outside contribution slots", static_cast<long long>(ordinal), -1, overlayInputs.size());
				return false;
			}
			const auto index = static_cast<std::size_t>(ordinal);
			if (overlayInputs[index] == nullptr || owned[index] || output.contributionStates_[index] !=
				PreparedBuildingGeometry::ContributionStatus::Prepared ||
				occurrence.SourceKey.Bytes() != overlayInputs[index]->CanonicalSpriteKey)
			{
				RECORD_GEOMETRY_FAILURE("geometry initialization / progressive overlay validation", overlayInputs[index] == nullptr
					? "overlay descriptor absent" : owned[index] ? "duplicate prepared record" :
					output.contributionStates_[index] != PreparedBuildingGeometry::ContributionStatus::Prepared
					? "contribution state is not Prepared" : "canonical sprite key mismatch", static_cast<long long>(ordinal),
					-1, static_cast<unsigned>(PreparedBuildingGeometry::ContributionStatus::Prepared),
					static_cast<unsigned>(output.contributionStates_[index]));
				return false;
			}
			owned[index] = true;
		}
		for (std::size_t ordinal = 0; ordinal < owned.size(); ++ordinal)
		{
			if ((ordinaryInputs[ordinal] != nullptr || overlayInputs[ordinal] != nullptr) &&
				output.contributionStates_[ordinal] == PreparedBuildingGeometry::ContributionStatus::Prepared &&
				!owned[ordinal])
			{
				RECORD_GEOMETRY_FAILURE("geometry initialization / progressive sprite validation",
					"Prepared state has no matching geometry record", static_cast<long long>(ordinal));
				return false;
			}
		}
		return true;
	}

	/** Checks one owned sprite record against its exact indexed input without retaining the view. */
	bool BuildingGeometryPreparation::MatchesPreparedSprite(
		std::uint64_t ordinal,
		const PreparedBuildingGeometry::SpriteOccurrence& occurrence,
		const std::vector<const BuildingSpriteRenderInput*>& inputs,
		std::vector<bool>& owned,
		const PreparedBuildingGeometry& output,
		PreparationCursor& cursor)
	{
		if (ordinal >= inputs.size())
		{
			RECORD_GEOMETRY_FAILURE("geometry initialization / progressive ordinary validation",
					"ordinal outside contribution slots", static_cast<long long>(ordinal), -1, inputs.size());
			return false;
		}
		const auto index = static_cast<std::size_t>(ordinal);
		if (owned[index] || inputs[index] == nullptr ||
			output.contributionStates_[index] != PreparedBuildingGeometry::ContributionStatus::Prepared ||
			occurrence.SourceKey.Bytes() != inputs[index]->CanonicalSpriteKey)
		{
			RECORD_GEOMETRY_FAILURE("geometry initialization / progressive ordinary validation", owned[index]
					? "duplicate prepared record" : inputs[index] == nullptr ? "ordinary descriptor absent" :
					output.contributionStates_[index] != PreparedBuildingGeometry::ContributionStatus::Prepared
					? "contribution state is not Prepared" : "canonical sprite key mismatch", static_cast<long long>(ordinal),
					-1, static_cast<unsigned>(PreparedBuildingGeometry::ContributionStatus::Prepared),
					static_cast<unsigned>(output.contributionStates_[index]));
			return false;
		}
		owned[index] = true;
		return true;
	}

	/** Rejects failed states and mismatched terminal omission ownership before any work begins. */
	bool BuildingGeometryPreparation::ValidateSceneOccurrenceStates(
		const PreparedBuildingGeometry& output,
		const std::vector<const BuildingSpriteRenderInput*>& ordinaryInputs,
		const std::vector<const BuildingSplineRenderInput*>& splineInputs,
		const std::vector<const BuildingSpriteRenderInput*>& overlayInputs,
		PreparationCursor& cursor)
	{
		for (std::size_t ordinal = 0; ordinal < output.contributionStates_.size(); ++ordinal)
		{
			const auto state = output.contributionStates_[ordinal];
			const std::size_t owners = static_cast<std::size_t>(ordinaryInputs[ordinal] != nullptr) +
				static_cast<std::size_t>(splineInputs[ordinal] != nullptr) +
				static_cast<std::size_t>(overlayInputs[ordinal] != nullptr);
			if (owners > 1 || state == PreparedBuildingGeometry::ContributionStatus::Failed ||
				(state == PreparedBuildingGeometry::ContributionStatus::TerminalOmission && owners != 0))
			{
				RECORD_GEOMETRY_FAILURE("geometry initialization / occurrence validation", owners > 1
					? "multiple descriptor owners" : state == PreparedBuildingGeometry::ContributionStatus::Failed
					? "contribution already Failed" : "terminal omission still has a descriptor",
					static_cast<long long>(ordinal), -1, 0, owners, -1, static_cast<int>(state));
				return false;
			}
		}
		return true;
	}

	/** Seeds world-space occluder inputs from already retained ordinary sprite values. */
	bool BuildingGeometryPreparation::SeedPreparedOccluders(
		const PreparedBuildingGeometry& output,
		BuildingVisualsContext& scratch)
	{
		for (const auto& occurrence : output.ordinarySprites_)
		{
			if (!occurrence.IsOccluder)
			{
				continue;
			}
			SplineOcclusionSpriteQuad occluder;
			occluder.TopHeight = occurrence.OccluderTopHeight;
			occluder.Key = occurrence.SectionLocalOrdinal;
			for (std::size_t corner = 0; corner < occurrence.WorldQuad.Corners.size(); ++corner)
			{
				occluder.Corners[corner] = {
					occurrence.WorldQuad.Corners[corner][0], occurrence.WorldQuad.Corners[corner][1]};
			}
			scratch.occluders_.push_back(occluder);
		}
		return true;
	}

	/** Builds one owned sprite quad and, for ordinary sprites, its independent occluder value. */
	BuildingGeometryPreparation::Status BuildingGeometryPreparation::PrepareSprite(
		const BuildingSpriteRenderInput& sprite,
		std::size_t sceneIndex,
		bool overlay,
		BuildingVisualsContext& scratch,
		PreparedBuildingGeometry& output,
		PreparationCursor& cursor)
	{
		BuildingVisualsContributorOutcome outcome;
		outcome.ContributorIndex = sceneIndex;
		const auto currentStatus = output.contributionStates_[
			static_cast<std::size_t>(sprite.SectionLocalOrdinal)];
		if (currentStatus == PreparedBuildingGeometry::ContributionStatus::Prepared)
		{
			return Status::Pending;
		}
		if (currentStatus != PreparedBuildingGeometry::ContributionStatus::NotProcessed)
		{
			RECORD_GEOMETRY_FAILURE(overlay ? "overlay sprite preparation" : "ordinary sprite preparation",
				"contribution state is not NotProcessed", static_cast<long long>(sprite.SectionLocalOrdinal),
				static_cast<long long>(sceneIndex), static_cast<unsigned>(PreparedBuildingGeometry::ContributionStatus::NotProcessed),
				static_cast<unsigned>(currentStatus));
			return Status::InvalidInputs;
		}
		if (!SetContributionStatus(output, sprite.SectionLocalOrdinal,
			PreparedBuildingGeometry::ContributionStatus::Unresolved))
		{
			outcome.FailureStage = BuildingVisualsFailureStage::SpriteGeometry;
			RECORD_GEOMETRY_FAILURE(overlay ? "overlay sprite preparation" : "ordinary sprite preparation",
				"could not set contribution Unresolved", static_cast<long long>(sprite.SectionLocalOrdinal), static_cast<long long>(sceneIndex));
			return RecordSpriteFailure(sprite, overlay, outcome, output, cursor, Status::InvalidInputs);
		}
		SpriteWorldQuad worldQuad;
		if (!SpriteGeometry::BuildWorldQuad(sprite.Placement, sprite.ProjectionSourceSize,
			sprite.CaptureBounds, sprite.Projection, worldQuad))
		{
			outcome.FailureStage = BuildingVisualsFailureStage::SpriteGeometry;
			RECORD_GEOMETRY_FAILURE(overlay ? "overlay sprite preparation" : "ordinary sprite preparation",
				"BuildWorldQuad failed", static_cast<long long>(sprite.SectionLocalOrdinal), static_cast<long long>(sceneIndex));
			return RecordSpriteFailure(sprite, overlay, outcome, output, cursor, Status::NumericalFailure);
		}
		return RetainSprite(sprite, sceneIndex, overlay, worldQuad, scratch, output, cursor, outcome);
	}

	/** Owns a valid sprite occurrence and computes ordinary-sprite occlusion independently of raster output. */
	BuildingGeometryPreparation::Status BuildingGeometryPreparation::RetainSprite(
		const BuildingSpriteRenderInput& sprite,
		std::size_t sceneIndex,
		bool overlay,
		const SpriteWorldQuad& worldQuad,
		BuildingVisualsContext& scratch,
		PreparedBuildingGeometry& output,
		PreparationCursor& cursor,
		BuildingVisualsContributorOutcome& outcome)
	{
		PreparedBuildingGeometry::SpriteOccurrence occurrence;
		occurrence.SectionLocalOrdinal = sprite.SectionLocalOrdinal;
		occurrence.SceneIndex = sceneIndex;
		occurrence.PlacementPosition = sprite.Placement.Position;
		occurrence.WorldQuad = worldQuad;
		if (!Cache::OwnedSpriteKey::TryCopy(sprite.CanonicalSpriteKey, occurrence.SourceKey))
		{
			outcome.FailureStage = BuildingVisualsFailureStage::SpriteGeometry;
			RECORD_GEOMETRY_FAILURE(overlay ? "overlay sprite preparation" : "ordinary sprite preparation",
				"could not copy canonical sprite key", static_cast<long long>(sprite.SectionLocalOrdinal), static_cast<long long>(sceneIndex));
			return RecordSpriteFailure(sprite, overlay, outcome, output, cursor, Status::InvalidInputs);
		}
		if (!overlay)
		{
			outcome.OccluderGeometryPrepared = BuildingVisuals::AddOccluder(
				sprite, worldQuad, sceneIndex, scratch);
			if (outcome.OccluderGeometryPrepared)
			{
				occurrence.IsOccluder = true;
				occurrence.OccluderTopHeight = scratch.occluders_.back().TopHeight;
			}
		}
		outcome.GeometryPrepared = true;
		(overlay ? output.overlayLastSprites_ : output.ordinarySprites_).push_back(std::move(occurrence));
		SetContributionStatus(output, sprite.SectionLocalOrdinal,
			outcome.FailureStage == BuildingVisualsFailureStage::None
				? PreparedBuildingGeometry::ContributionStatus::Prepared
				: PreparedBuildingGeometry::ContributionStatus::Failed);
		(overlay ? cursor.Results.OverlayLastSprites : cursor.Results.OrdinarySprites).push_back(outcome);
		if (outcome.FailureStage != BuildingVisualsFailureStage::None)
		{
			cursor.Results.Complete = false;
			RECORD_GEOMETRY_FAILURE(overlay ? "overlay sprite preparation" : "ordinary sprite preparation",
				"contributor has a failure stage", static_cast<long long>(sprite.SectionLocalOrdinal),
				static_cast<long long>(sceneIndex), 0, 0, -1, static_cast<int>(outcome.FailureStage));
			return Status::NumericalFailure;
		}
		return Status::Pending;
	}

	/** Records failure evidence and prevents an invalid sprite from becoming drawable output. */
	BuildingGeometryPreparation::Status BuildingGeometryPreparation::RecordSpriteFailure(
		const BuildingSpriteRenderInput& sprite,
		bool overlay,
		BuildingVisualsContributorOutcome outcome,
		PreparedBuildingGeometry& output,
		PreparationCursor& cursor,
		Status status)
	{
		SetContributionStatus(output, sprite.SectionLocalOrdinal,
			PreparedBuildingGeometry::ContributionStatus::Failed);
		cursor.Results.Complete = false;
		(overlay ? cursor.Results.OverlayLastSprites : cursor.Results.OrdinarySprites).push_back(outcome);
		return status;
	}

	/** Samples one spline once, then advances one ordered surface or selection result. */
	BuildingGeometryPreparation::Status BuildingGeometryPreparation::AdvanceSpline(
		const BuildingSplineRenderInput& spline,
		std::size_t splineIndex,
		BuildingVisualsContext& scratch,
		PreparedBuildingGeometry& output,
		PreparationCursor& cursor)
	{
		const bool drawAll = spline.SurfaceSelectionMode ==
			BuildingSplineSurfaceSelectionMode::DrawAllSurfaces;
		const bool selectAlternative = spline.SurfaceSelectionMode ==
			BuildingSplineSurfaceSelectionMode::FirstNumericallyDrawableAlternative;
		if (!drawAll && !selectAlternative)
		{
			RECORD_GEOMETRY_FAILURE("spline preparation", "unsupported surface selection mode",
				static_cast<long long>(spline.SectionLocalOrdinal), static_cast<long long>(splineIndex),
				0, 0, -1, static_cast<int>(spline.SurfaceSelectionMode));
			RecordInvalidSplineInput(splineIndex, spline.SectionLocalOrdinal, output, cursor);
			return Status::InvalidInputs;
		}
		if (drawAll && spline.Surfaces.empty())
		{
			if (!SetContributionStatus(output, spline.SectionLocalOrdinal,
				PreparedBuildingGeometry::ContributionStatus::Unresolved))
			{
				RECORD_GEOMETRY_FAILURE("spline preparation", "could not set contribution Unresolved",
					static_cast<long long>(spline.SectionLocalOrdinal), static_cast<long long>(splineIndex));
				RecordInvalidSplineInput(splineIndex, spline.SectionLocalOrdinal, output, cursor);
				return Status::InvalidInputs;
			}
			FinishSpline(spline.SectionLocalOrdinal, output, cursor, false);
			return Status::Pending;
		}
		if (!cursor.SplineSamplingPrepared)
		{
			if (!SetContributionStatus(output, spline.SectionLocalOrdinal,
				PreparedBuildingGeometry::ContributionStatus::Unresolved))
			{
				RECORD_GEOMETRY_FAILURE("spline preparation", "could not set contribution Unresolved",
					static_cast<long long>(spline.SectionLocalOrdinal), static_cast<long long>(splineIndex));
				RecordInvalidSplineInput(splineIndex, spline.SectionLocalOrdinal, output, cursor);
				return Status::InvalidInputs;
			}
			SplineGeometry::Prepare(spline.Segments, spline.SamplingPolicy,
				scratch.preparedSplines_[splineIndex]);
			cursor.SplineSamplingPrepared = true;
			return Status::Pending;
		}
		if (spline.Surfaces.empty())
		{
			return CompleteSplineWithoutSurface(spline, splineIndex, scratch, output, cursor);
		}
		if (cursor.SplineSurfaceIndex >= spline.Surfaces.size())
		{
			return CompleteSplineWithoutSurface(spline, splineIndex, scratch, output, cursor);
		}
		return PrepareSplineCandidate(spline, splineIndex, cursor.SplineSurfaceIndex,
			selectAlternative, scratch, output, cursor);
	}

	/** Records a source correspondence failure without claiming drawable spline geometry. */
	void BuildingGeometryPreparation::RecordInvalidSplineInput(
		std::size_t splineIndex,
		std::uint64_t ordinal,
		PreparedBuildingGeometry& output,
		PreparationCursor& cursor)
	{
		BuildingVisualsContributorOutcome outcome;
		outcome.ContributorIndex = splineIndex;
		outcome.HasSurfaceIndex = false;
		outcome.FailureStage = BuildingVisualsFailureStage::SplineSampling;
		SetContributionStatus(output, ordinal, PreparedBuildingGeometry::ContributionStatus::Failed);
		cursor.Results.SplineSurfaces.push_back(outcome);
		cursor.Results.Complete = false;
	}

	/** Prepares one ordered surface candidate and retains it only after clipping and conversion succeed. */
	BuildingGeometryPreparation::Status BuildingGeometryPreparation::PrepareSplineCandidate(
		const BuildingSplineRenderInput& spline,
		std::size_t splineIndex,
		std::size_t surfaceIndex,
		bool alternative,
		BuildingVisualsContext& scratch,
		PreparedBuildingGeometry& output,
		PreparationCursor& cursor)
	{
		const auto& preparedSpline = scratch.preparedSplines_[splineIndex];
		const auto& surface = spline.Surfaces[surfaceIndex];
		BuildingVisualsContributorOutcome outcome;
		const bool drawable = PrepareSplineCandidateGeometry(
			spline, preparedSpline, surface, splineIndex, surfaceIndex, scratch, outcome);
		if (alternative && !drawable)
		{
			cursor.Results.SplineSurfaces.push_back(outcome);
			++cursor.SplineSurfaceIndex;
			return Status::Pending;
		}
		if (!drawable)
		{
			cursor.Results.SplineSurfaces.push_back(outcome);
			SetContributionStatus(output, spline.SectionLocalOrdinal,
				PreparedBuildingGeometry::ContributionStatus::Failed);
			cursor.Results.Complete = false;
			RECORD_GEOMETRY_FAILURE(!outcome.SamplingComplete || outcome.SamplingIssue != SplineNumericalIssue::None
				? "spline numerical preparation" : "spline surface preparation",
				"surface candidate is not drawable",
				static_cast<long long>(spline.SectionLocalOrdinal), static_cast<long long>(splineIndex),
				0, outcome.RejectedCells, static_cast<long long>(surfaceIndex),
				!outcome.SamplingComplete || outcome.SamplingIssue != SplineNumericalIssue::None
					? static_cast<int>(outcome.SamplingIssue) : static_cast<int>(outcome.SurfaceIssue));
			return Status::NumericalFailure;
		}
		if (outcome.FailureStage != BuildingVisualsFailureStage::None)
		{
			cursor.Results.SplineSurfaces.push_back(outcome);
			SetContributionStatus(output, spline.SectionLocalOrdinal,
				PreparedBuildingGeometry::ContributionStatus::Failed);
			cursor.Results.Complete = false;
			RECORD_GEOMETRY_FAILURE(!outcome.SamplingComplete || outcome.SamplingIssue != SplineNumericalIssue::None
				? "spline numerical preparation" : "spline surface preparation",
				"drawable surface candidate reports a numerical failure",
				static_cast<long long>(spline.SectionLocalOrdinal), static_cast<long long>(splineIndex),
				0, outcome.RejectedCells, static_cast<long long>(surfaceIndex),
				!outcome.SamplingComplete || outcome.SamplingIssue != SplineNumericalIssue::None
					? static_cast<int>(outcome.SamplingIssue) : static_cast<int>(outcome.SurfaceIssue));
			return Status::NumericalFailure;
		}
		return FinishSplineCandidate(spline, splineIndex, surfaceIndex, alternative,
			scratch, output, cursor, outcome);
	}

	/** Retains one selected/all-surface result and advances its source-ordered contributor cursor. */
	BuildingGeometryPreparation::Status BuildingGeometryPreparation::FinishSplineCandidate(
		const BuildingSplineRenderInput& spline,
		std::size_t splineIndex,
		std::size_t surfaceIndex,
		bool alternative,
		BuildingVisualsContext& scratch,
		PreparedBuildingGeometry& output,
		PreparationCursor& cursor,
		BuildingVisualsContributorOutcome& outcome)
	{
		const auto& surface = spline.Surfaces[surfaceIndex];
		if (alternative)
		{
			outcome.AlternativeStatus = BuildingSplineAlternativeStatus::Selected;
		}
		const Status status = RetainPreparedSurface(spline, surface, splineIndex, surfaceIndex,
			scratch, output, cursor, outcome);
		if (status != Status::Pending)
		{
			cursor.Results.SplineSurfaces.push_back(outcome);
			SetContributionStatus(output, spline.SectionLocalOrdinal,
				PreparedBuildingGeometry::ContributionStatus::Failed);
			cursor.Results.Complete = false;
			return status;
		}
		output.splineSurfaces_.back().SourceDescriptorIndex = splineIndex;
		output.splineSurfaces_.back().SelectedAlternative = alternative;
		cursor.Results.SplineSurfaces.push_back(outcome);
		if (outcome.FailureStage != BuildingVisualsFailureStage::None)
		{
			SetContributionStatus(output, spline.SectionLocalOrdinal,
				PreparedBuildingGeometry::ContributionStatus::Failed);
			cursor.Results.Complete = false;
			RECORD_GEOMETRY_FAILURE("spline surface preparation", "retained surface has a failure stage",
				static_cast<long long>(spline.SectionLocalOrdinal), static_cast<long long>(splineIndex),
				0, 0, static_cast<long long>(surfaceIndex), static_cast<int>(outcome.FailureStage));
			return Status::NumericalFailure;
		}
		if (alternative)
		{
			FinishSpline(spline.SectionLocalOrdinal, output, cursor, false);
		}
		else
		{
			++cursor.SplineSurfaceIndex;
			if (cursor.SplineSurfaceIndex == spline.Surfaces.size())
			{
				FinishSpline(spline.SectionLocalOrdinal, output, cursor, false);
			}
		}
		return Status::Pending;
	}

	/** Reuses the active renderer's surface-preparation algorithm for this candidate. */
	bool BuildingGeometryPreparation::PrepareSplineCandidateGeometry(
		const BuildingSplineRenderInput& spline,
		const PreparedSpline& preparedSpline,
		const BuildingSplineSurfaceRenderInput& surface,
		std::size_t splineIndex,
		std::size_t surfaceIndex,
		BuildingVisualsContext& scratch,
		BuildingVisualsContributorOutcome& outcome)
	{
		outcome.ContributorIndex = splineIndex;
		outcome.SurfaceIndex = surfaceIndex;
		outcome.AlternativeStatus = spline.SurfaceSelectionMode ==
			BuildingSplineSurfaceSelectionMode::FirstNumericallyDrawableAlternative
			? BuildingSplineAlternativeStatus::CandidateRejected
			: BuildingSplineAlternativeStatus::NotApplicable;
		outcome.SamplingIssue = preparedSpline.Issue;
		outcome.SamplingComplete = preparedSpline.IsComplete();
		if (preparedSpline.Issue != SplineNumericalIssue::None || !outcome.SamplingComplete)
		{
			outcome.FailureStage = BuildingVisualsFailureStage::SplineSampling;
		}
		BuildingVisuals::PrepareSplineSurfaceGeometry(spline, preparedSpline, surface, scratch, outcome);
		return outcome.GeometryPrepared && BuildingVisuals::HasDrawableSplineSurface(scratch.preparedSurface_);
	}

	/** Clips visible triangles and moves only the selected appearance's converted vector to output. */
	BuildingGeometryPreparation::Status BuildingGeometryPreparation::RetainPreparedSurface(
		const BuildingSplineRenderInput& spline,
		const BuildingSplineSurfaceRenderInput& surface,
		std::size_t splineIndex,
		std::size_t surfaceIndex,
		BuildingVisualsContext& scratch,
		PreparedBuildingGeometry& output,
		PreparationCursor& cursor,
		BuildingVisualsContributorOutcome& outcome)
	{
		if (!PrepareVisibleTriangles(surface, scratch, cursor, outcome))
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			cursor.Failure.PublicationOrdinal = static_cast<long long>(spline.SectionLocalOrdinal);
			cursor.Failure.DescriptorIndex = static_cast<long long>(splineIndex);
			cursor.Failure.SurfaceIndex = static_cast<long long>(surfaceIndex);
#endif
			return Status::NumericalFailure;
		}
		AppendPreparedSurface(spline, surface, surfaceIndex, scratch, output, cursor);
		outcome.GeometryPrepared = true;
		outcome.ContributorIndex = splineIndex;
		return Status::Pending;
	}

	/** Clips one surface and converts only its active numeric appearance into scratch vectors. */
	bool BuildingGeometryPreparation::PrepareVisibleTriangles(
		const BuildingSplineSurfaceRenderInput& surface,
		BuildingVisualsContext& scratch,
		PreparationCursor& cursor,
		BuildingVisualsContributorOutcome& outcome)
	{
		outcome.OcclusionAttempted = true;
		outcome.OcclusionStatus = SplineOcclusionGeometry::ClipSurface(
			scratch.preparedSurface_.Vertices,
			scratch.preparedSurface_.Indices,
			scratch.preparedSurface_.Cells,
			scratch.occlusionIndex_,
			scratch.occlusionWorkspace_);
		if (outcome.OcclusionStatus != SplineOcclusionStatus::Complete)
		{
			outcome.FailureStage = BuildingVisualsFailureStage::Occlusion;
			RECORD_GEOMETRY_FAILURE("height-aware clipping", "ClipSurface did not complete",
				-1, -1, 0, 0, -1, static_cast<int>(outcome.OcclusionStatus));
			return false;
		}
		if (!ValidateAppearanceMetadata(surface))
		{
			outcome.FailureStage = BuildingVisualsFailureStage::AppearanceValidation;
			RECORD_GEOMETRY_FAILURE("spline appearance validation", "invalid numeric appearance metadata");
			return false;
		}
		const bool solid = surface.Appearance.Mode == SplineSurfaceAppearanceMode::SolidColor;
		const bool textured = surface.Appearance.Mode == SplineSurfaceAppearanceMode::Textured;
		const bool converted = solid
			? BuildingVisuals::ConvertSolidSurfaceTriangles(surface, scratch, cursor.PendingOpacity)
			: textured && BuildingVisuals::ConvertTexturedSurfaceTriangles(
				surface, scratch, cursor.PendingOpacity);
		if (!converted)
		{
			outcome.FailureStage = BuildingVisualsFailureStage::AppearanceValidation;
			RECORD_GEOMETRY_FAILURE("triangle conversion", "could not convert visible triangles");
			return false;
		}
		return true;
	}

	/** Validates only numeric texture metadata; borrowed source pixels remain caller-owned. */
	bool BuildingGeometryPreparation::ValidateAppearanceMetadata(
		const BuildingSplineSurfaceRenderInput& surface) noexcept
	{
		if (surface.Appearance.Mode == SplineSurfaceAppearanceMode::SolidColor)
		{
			return true;
		}
		if (surface.Appearance.Mode != SplineSurfaceAppearanceMode::Textured)
		{
			return false;
		}
		return std::isfinite(surface.Appearance.UvScale[0]) &&
			std::isfinite(surface.Appearance.UvScale[1]) &&
			std::isfinite(surface.Appearance.UvOffset[0]) &&
			std::isfinite(surface.Appearance.UvOffset[1]) &&
			(surface.Appearance.AddressU == TextureAddressMode::Clamp ||
				surface.Appearance.AddressU == TextureAddressMode::Repeat) &&
			(surface.Appearance.AddressV == TextureAddressMode::Clamp ||
				surface.Appearance.AddressV == TextureAddressMode::Repeat);
	}

	/** Moves the active scratch vector into a self-contained surface value. */
	void BuildingGeometryPreparation::AppendPreparedSurface(
		const BuildingSplineRenderInput& spline,
		const BuildingSplineSurfaceRenderInput& surface,
		std::size_t surfaceIndex,
		BuildingVisualsContext& scratch,
		PreparedBuildingGeometry& output,
		PreparationCursor& cursor)
	{
		const bool textured = surface.Appearance.Mode == SplineSurfaceAppearanceMode::Textured;
		PreparedBuildingGeometry::SplineSurface prepared;
		prepared.SectionLocalOrdinal = spline.SectionLocalOrdinal;
		prepared.SurfaceIndex = surfaceIndex;
		prepared.IsTextured = textured;
		prepared.SolidRgb = surface.Appearance.SolidRgb;
		prepared.ModulationRgb = surface.Appearance.ModulationRgb;
		prepared.UvScale = surface.Appearance.UvScale;
		prepared.UvOffset = surface.Appearance.UvOffset;
		prepared.AddressU = surface.Appearance.AddressU;
		prepared.AddressV = surface.Appearance.AddressV;
		prepared.Opacity = cursor.PendingOpacity;
		if (!textured)
		{
			prepared.SolidTriangles = std::move(scratch.solidTriangles_);
		}
		else
		{
			prepared.TexturedTriangles = std::move(scratch.texturedTriangles_);
		}
		output.splineSurfaces_.push_back(std::move(prepared));
	}

	/** Records a valid no-surface contributor or an exhausted ordered alternative set. */
	BuildingGeometryPreparation::Status BuildingGeometryPreparation::CompleteSplineWithoutSurface(
		const BuildingSplineRenderInput& spline,
		std::size_t splineIndex,
		BuildingVisualsContext& scratch,
		PreparedBuildingGeometry& output,
		PreparationCursor& cursor)
	{
		BuildingVisualsContributorOutcome outcome;
		outcome.ContributorIndex = splineIndex;
		outcome.HasSurfaceIndex = false;
		outcome.AlternativeStatus = BuildingSplineAlternativeStatus::NoDrawableAlternative;
		outcome.FailureStage = BuildingVisualsFailureStage::SurfacePreparation;
		outcome.SamplingIssue = scratch.preparedSplines_[splineIndex].Issue;
		outcome.SamplingComplete = scratch.preparedSplines_[splineIndex].IsComplete();
		cursor.Results.SplineSurfaces.push_back(outcome);
		cursor.Results.Complete = false;
		cursor.HasAcceptedOmissions = true;
		output.terminalSplineOmissionOrdinals_.push_back(spline.SectionLocalOrdinal);
		FinishSpline(spline.SectionLocalOrdinal, output, cursor, true);
		return Status::Pending;
	}

	/** Marks one spline occurrence terminal and advances to the next contributor. */
	void BuildingGeometryPreparation::FinishSpline(
		std::uint64_t ordinal,
		PreparedBuildingGeometry& output,
		PreparationCursor& cursor,
		bool omitted)
	{
		SetContributionStatus(output, ordinal, omitted
			? PreparedBuildingGeometry::ContributionStatus::TerminalOmission
			: PreparedBuildingGeometry::ContributionStatus::Prepared);
		++cursor.SplineIndex;
		cursor.SplineSurfaceIndex = 0;
		cursor.SplineSamplingPrepared = false;
	}

	/** Reserves one ordinal slot and rejects ordinals already owned by another contributor. */
	bool BuildingGeometryPreparation::SetContributionStatus(
		PreparedBuildingGeometry& output,
		std::uint64_t ordinal,
		PreparedBuildingGeometry::ContributionStatus status)
	{
		return output.TrySetContributionStatus(ordinal, status);
	}

	/** Stores a terminal status without certifying partial output as a complete scene. */
	BuildingGeometryPreparation::StepResult BuildingGeometryPreparation::Terminate(
		PreparationCursor& cursor,
		Status status,
		std::size_t workUnitsConsumed) noexcept
	{
		cursor.TerminalStatus = status;
		return {status, workUnitsConsumed};
	}

	/** Marks the caller's existing output complete only after every scene pass terminates. */
	BuildingGeometryPreparation::StepResult BuildingGeometryPreparation::Complete(
		PreparedBuildingGeometry& output,
		PreparationCursor& cursor,
		std::size_t workUnitsConsumed) noexcept
	{
		if (output.failedContributionCount_ != 0)
		{
			RECORD_GEOMETRY_FAILURE("geometry completion", "failed contributions remain",
				-1, -1, 0, output.failedContributionCount_);
			return Terminate(cursor, Status::NumericalFailure, workUnitsConsumed);
		}
		if (output.unfinishedContributionCount_ != 0)
		{
			return {Status::Pending, workUnitsConsumed};
		}
		output.complete_ = true;
		cursor.TerminalStatus = output.terminalOmissionCount_ != 0
			? Status::CompleteWithOmissions
			: Status::Complete;
		return {cursor.TerminalStatus, workUnitsConsumed};
	}
}

#undef RECORD_GEOMETRY_FAILURE

#endif // MODLOADER_CLIENT_BUILD
