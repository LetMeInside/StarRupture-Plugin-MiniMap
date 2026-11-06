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

	/** Records actual numerical and destination outcomes for one positional scene contributor. */
	struct BuildingVisualsContributorOutcome
	{
		std::size_t ContributorIndex = 0;
		std::size_t SurfaceIndex = 0;
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

	/** Owns reusable positional reports for one scene call and marks whether the supplied scene completed. */
	class BuildingVisualsResult final
	{
	public:
		/** Reports whether every supplied contributor completed without numerical or destination failure. */
		bool Complete = true;
		/** Status of the one ordinary-sprite occlusion-index preparation attempt. */
		SplineOcclusionStatus OcclusionPreparationStatus = SplineOcclusionStatus::NotPrepared;
		/** Outcomes align with BuildingRenderSceneInput::OrdinarySprites. */
		std::vector<BuildingVisualsContributorOutcome> OrdinarySprites;
		/** Outcomes align with spline contributors, then each contributor's supplied surface order. */
		std::vector<BuildingVisualsContributorOutcome> SplineSurfaces;
		/** Outcomes align with BuildingRenderSceneInput::OverlayLastSprites. */
		std::vector<BuildingVisualsContributorOutcome> OverlayLastSprites;
	};
}
