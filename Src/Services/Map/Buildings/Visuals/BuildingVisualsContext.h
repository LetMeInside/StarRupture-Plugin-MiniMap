// Owns caller-scoped reusable numerical geometry, clipping, conversion, and outcome storage.
#pragma once

#include "BuildingVisualsResult.h"
#include "../IImageDestination.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace Services::Map::Buildings::Visuals
{
	class BuildingGeometryPreparation;

	/** Retains numerical values and vector capacity for one caller's BuildingVisuals scene operations.
	 * The caller owns this context and may retain it across calls; it contains no borrowed input pointers.
	 */
	class BuildingVisualsContext final
	{
		friend class BuildingVisuals;
		friend class BuildingGeometryPreparation;

	public:
		/** Creates an empty context whose reusable storage remains independent of all other contexts. */
		BuildingVisualsContext() = default;
		/** Prevents copying retained numerical and clipping workspaces. */
		BuildingVisualsContext(const BuildingVisualsContext&) = delete;
		/** Prevents copying retained numerical and clipping workspaces. */
		BuildingVisualsContext& operator=(const BuildingVisualsContext&) = delete;
		/** Transfers all retained numerical storage to another caller-owned context. */
		BuildingVisualsContext(BuildingVisualsContext&&) noexcept = default;
		/** Transfers all retained numerical storage to another caller-owned context. */
		BuildingVisualsContext& operator=(BuildingVisualsContext&&) noexcept = default;

	private:
		std::vector<PreparedSpline> preparedSplines_;
		PreparedSplineSurface preparedSurface_;
		std::vector<SplineOcclusionSpriteQuad> occluders_;
		SplineOcclusionIndex occlusionIndex_;
		SplineOcclusionWorkspace occlusionWorkspace_;
		std::vector<SolidTriangle> solidTriangles_;
		std::vector<TexturedTriangle> texturedTriangles_;
		BuildingVisualsResult result_;
	};
}
