// Reports aggregate and positional outcomes from one synchronous BuildingVisuals scene pass.
#pragma once

#include "SplineGeometry.h"
#include "SplineOcclusionGeometry.h"
#include "SplineSurfaceGeometry.h"
#include "../IImageDestination.h"

#include <cstddef>
#include <vector>

namespace Services::Map::Buildings::Visuals
{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
	/** Carries Debug-only CPU wall timings for the six existing numerical Section phases. */
	struct BuildingVisualsPhaseTimings
	{
		/** Describes whether one synchronous phase ran and its elapsed CPU wall time. */
		struct Phase
		{
			double ElapsedMs = 0.0;
			bool Invoked = false;
		};
		Phase DestinationConstruction;
		Phase OrdinarySprites;
		Phase OcclusionIndexPreparation;
		Phase SplineDrawing;
		Phase OverlaySprites;
		Phase DestinationFinalization;
	};
#endif

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
		bool OccluderGeometryPrepared = false;
		bool SamplingComplete = true;
		bool SubmissionAttempted = false;
		bool OcclusionAttempted = false;
	};

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
}
