// Reports aggregate and positional outcomes from one synchronous BuildingVisuals scene pass.
#pragma once

#include "SplineGeometry.h"
#include "SplineOcclusionGeometry.h"
#include "SplineSurfaceGeometry.h"
#include "../IImageDestination.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Services::Map::Buildings::Visuals
{
	class BuildingVisualsResult;

	/** Identifies the operation that prevented one supplied contributor from drawing completely. */
	enum class BuildingVisualsFailureStage
	{
		None,
		SpriteGeometry,
		DestinationSubmission,
		OccluderGeometry,
		SplineSampling,
		SurfacePreparation,
		Occlusion,
		AppearanceValidation,
		SurfaceSubmission
	};

	/** Describes how one positional surface outcome participates in ordered alternative selection. */
	enum class BuildingSplineAlternativeStatus
	{
		NotApplicable,
		CandidateRejected,
		Selected,
		NoDrawableAlternative
	};

	/** Records actual numerical and destination outcomes for one positional scene contributor. */
	struct BuildingVisualsContributorOutcome
	{
		/** Position of this contributor in its scene input span. */
		std::size_t ContributorIndex = 0;
		/** Position of the supplied surface; meaningful only when HasSurfaceIndex is true. */
		std::size_t SurfaceIndex = 0;
		/** False only for the contributor-level NoDrawableAlternative summary outcome. */
		bool HasSurfaceIndex = true;
		/** Selection status; Selected means chosen, not that clipping or submission succeeded. */
		BuildingSplineAlternativeStatus AlternativeStatus = BuildingSplineAlternativeStatus::NotApplicable;
		BuildingVisualsFailureStage FailureStage = BuildingVisualsFailureStage::None;
		/** Submission result; inspect SubmissionAttempted before interpreting this value. */
		ImageSubmission Submission = ImageSubmission::InvalidInput;
		SplineNumericalIssue SamplingIssue = SplineNumericalIssue::None;
		SplineSurfaceIssue SurfaceIssue = SplineSurfaceIssue::None;
		/** Clip result; inspect OcclusionAttempted when this is a surface outcome. */
		SplineOcclusionStatus OcclusionStatus = SplineOcclusionStatus::Complete;
		std::size_t RejectedCells = 0;
		bool GeometryPrepared = false;
		/** True when valid prepared geometry has no area inside its owning Section. */
		bool SectionClippedAway = false;
		bool OccluderGeometryPrepared = false;
		bool SamplingComplete = true;
		bool SubmissionAttempted = false;
		bool OcclusionAttempted = false;
	};

	/** Summarizes terminal spline omissions without retaining per-contributor diagnostic storage. */
	struct BuildingVisualsTerminalOmissionSummary
	{
		/** Number of spline contributors that had no accepted drawable alternative. */
		std::size_t SplineContributorCount = 0;
		/** First omitted candidate's Section-local publication ordinal. */
		std::uint64_t FirstSectionLocalOrdinal = 0;
		/** Failure and alternative values shared by the currently permitted omission category. */
		BuildingVisualsFailureStage FailureStage = BuildingVisualsFailureStage::None;
		BuildingSplineAlternativeStatus AlternativeStatus = BuildingSplineAlternativeStatus::NotApplicable;
	};

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
	/** Stores the first failed operation as static text and scalar evidence, without borrowing scene data. */
	struct SectionFailureDetails
	{
		const char* Stage = nullptr;
		const char* Condition = nullptr;
		long long PublicationOrdinal = -1;
		long long DescriptorIndex = -1;
		unsigned long long Expected = 0;
		unsigned long long Actual = 0;
		long long SurfaceIndex = -1;
		int OperationStatus = -1;
		long long PublicationCount = -1;
		const char* StatusName = nullptr;
	};

	/** Carries Debug-only numerical phase timings and bounded drawing-failure evidence. */
	struct BuildingVisualsPhaseTimings
	{
		/** Describes whether one synchronous phase ran and its elapsed CPU wall time. */
		struct Phase
		{
			double ElapsedMs = 0.0;
			bool Invoked = false;
		};
		/** Counts failures in one scene category and copies only its first positional outcome. */
		struct FailureCategory
		{
			std::size_t Count = 0;
			bool HasFirst = false;
			BuildingVisualsContributorOutcome First{};
		};
		Phase DestinationConstruction;
		Phase OrdinarySprites;
		Phase OcclusionIndexPreparation;
		Phase SplineDrawing;
		Phase OverlaySprites;
		Phase DestinationFinalization;
		SplineOcclusionStatus OcclusionPreparationStatus = SplineOcclusionStatus::NotPrepared;
		FailureCategory OrdinarySpriteFailures;
		FailureCategory SplineSurfaceFailures;
		FailureCategory OverlaySpriteFailures;
		std::size_t RejectedSplineAlternativeCount = 0;
		std::size_t FatalSplineOutcomeCount = 0;
		bool HasFirstFatalSplineOutcome = false;
		BuildingVisualsContributorOutcome FirstFatalSplineOutcome{};

		/** Captures fixed-size positional failure evidence only for an incomplete draw result.
		 * @param result Existing positional outcomes from one synchronous scene draw.
		 */
		void CaptureFailureEvidence(const BuildingVisualsResult& result) noexcept;

	private:
		/** Counts one category and applies spline alternative fatality rules when requested.
		 * @param outcomes Existing positional outcomes owned by the draw result.
		 * @param category Fixed-size summary for this scene category.
		 * @param isSplineCategory Enables the renderer's alternative-selection rules.
		 */
		void CaptureCategory(
			const std::vector<BuildingVisualsContributorOutcome>& outcomes,
			FailureCategory& category,
			bool isSplineCategory) noexcept
		{
			for (const BuildingVisualsContributorOutcome& outcome : outcomes)
			{
				if (outcome.FailureStage == BuildingVisualsFailureStage::None)
				{
					continue;
				}
				++category.Count;
				if (!category.HasFirst)
				{
					category.First = outcome;
					category.HasFirst = true;
				}
				if (!isSplineCategory)
				{
					continue;
				}
				if (outcome.AlternativeStatus == BuildingSplineAlternativeStatus::CandidateRejected)
				{
					++RejectedSplineAlternativeCount;
					continue;
				}
				const bool fatalAlternative =
					outcome.AlternativeStatus == BuildingSplineAlternativeStatus::NoDrawableAlternative ||
					((outcome.AlternativeStatus == BuildingSplineAlternativeStatus::Selected ||
						outcome.AlternativeStatus == BuildingSplineAlternativeStatus::NotApplicable) &&
						outcome.FailureStage != BuildingVisualsFailureStage::None);
				if (fatalAlternative)
				{
					++FatalSplineOutcomeCount;
					if (!HasFirstFatalSplineOutcome)
					{
						FirstFatalSplineOutcome = outcome;
						HasFirstFatalSplineOutcome = true;
					}
				}
			}
		}
	};
#endif

	/** Owns reusable positional reports and marks whether each required scene contributor completed.
	 * A rejected alternative does not fail the scene when a later drawable alternative is selected.
	 */
	class BuildingVisualsResult final
	{
	public:
		/** Reports whether required contributors completed without numerical or destination failure. */
		bool Complete = true;
		/** Status of the one ordinary-sprite occlusion-index preparation attempt. */
		SplineOcclusionStatus OcclusionPreparationStatus = SplineOcclusionStatus::NotPrepared;
		/** Outcomes align with BuildingRenderSceneInput::OrdinarySprites. */
		std::vector<BuildingVisualsContributorOutcome> OrdinarySprites;
		/** All-surfaces outcomes align with supplied surface order. Alternative outcomes report examined
		 * candidates in order, the selected candidate if any, and a no-drawable summary when none qualifies.
		 */
		std::vector<BuildingVisualsContributorOutcome> SplineSurfaces;
		/** Outcomes align with BuildingRenderSceneInput::OverlayLastSprites. */
		std::vector<BuildingVisualsContributorOutcome> OverlayLastSprites;
	};

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
	/** Copies only bounded failure evidence from the existing positional draw results. */
	inline void BuildingVisualsPhaseTimings::CaptureFailureEvidence(
		const BuildingVisualsResult& result) noexcept
	{
		if (result.Complete)
		{
			return;
		}
		OcclusionPreparationStatus = result.OcclusionPreparationStatus;
		OrdinarySpriteFailures = {};
		SplineSurfaceFailures = {};
		OverlaySpriteFailures = {};
		RejectedSplineAlternativeCount = 0;
		FatalSplineOutcomeCount = 0;
		HasFirstFatalSplineOutcome = false;
		FirstFatalSplineOutcome = {};
		CaptureCategory(result.OrdinarySprites, OrdinarySpriteFailures, false);
		CaptureCategory(result.SplineSurfaces, SplineSurfaceFailures, true);
		CaptureCategory(result.OverlayLastSprites, OverlaySpriteFailures, false);
	}
#endif
}
