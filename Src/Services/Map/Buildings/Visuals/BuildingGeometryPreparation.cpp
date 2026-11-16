// Implements deadline-resumable, renderer-independent Building Map geometry preparation.
#if defined(MODLOADER_CLIENT_BUILD)

#include "BuildingGeometryPreparation.h"
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
#include "../BuildingMapPerformanceDiagnostics.h"
#endif

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
		CurrentPhase = Phase::Complete;
		SplineIndex = 0;
		SplineSurfaceIndex = 0;
		PendingOpacity = 0;
		Initialized = false;
		SplineSamplingPrepared = false;
		SplineHasDrawableSurface = false;
		SplineHasNumericalCandidateRejection = false;
		SplineHasNumericalClippingFailure = false;
		PendingSplineSurfaces.clear();
		HasSectionRegion = false;
		SectionRegion = {};
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
		const BuildingCollectionRegion& sectionRegion,
		BuildingVisualsContext& scratch,
		PreparedBuildingGeometry& output,
		PreparationCursor& cursor,
		std::chrono::steady_clock::time_point deadline)
	{
		if (cursor.TerminalStatus != Status::Pending)
		{
			return {cursor.TerminalStatus, 0};
		}
		if (!IsValidSectionRegion(sectionRegion) ||
			(cursor.HasSectionRegion && !MatchesSectionRegion(cursor.SectionRegion, sectionRegion)))
		{
			return Terminate(cursor, Status::InvalidInputs, 0);
		}
		if (!cursor.HasSectionRegion)
		{
			cursor.SectionRegion = sectionRegion;
			cursor.HasSectionRegion = true;
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

	/** Advances one occlusion-index, spline or terminal unit. */
	BuildingGeometryPreparation::Status BuildingGeometryPreparation::AdvanceOne(
		const BuildingRenderSceneInput& scene,
		BuildingVisualsContext& scratch,
		PreparedBuildingGeometry& output,
		PreparationCursor& cursor,
		std::size_t& workUnitsConsumed)
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		BuildingMapPerformanceDiagnostics::Timer step(&BuildingMapPerformanceDiagnostics::Game().Indivisible);
#endif
		workUnitsConsumed = 0;
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

	/** Advances spline and terminal phases after ordinary occluders are indexed. */
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
				cursor.CurrentPhase = PreparationCursor::Phase::Complete;
				return Status::Pending;
			}
			workUnitsConsumed = 1;
			return AdvanceSpline(scene.Splines[cursor.SplineIndex], cursor.SplineIndex,
				scratch, output, cursor);
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
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		BuildingMapPerformanceDiagnostics::Timer initialization(&BuildingMapPerformanceDiagnostics::Game().Indivisible);
#endif
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
		if (!scene.Splines.empty() && !SeedPreparedOccluders(output, scratch))
		{
			RECORD_GEOMETRY_FAILURE("geometry initialization", "SeedPreparedOccluders failed");
			return false;
		}
		cursor.CurrentPhase = scene.Splines.empty()
			? PreparationCursor::Phase::Complete
			: PreparationCursor::Phase::OcclusionIndex;
		cursor.Results.SplineSurfaces.reserve(scene.Splines.size());
		cursor.Initialized = true;
		return true;
	}

	/** Checks the exact half-open map rectangle before it is attached to a preparation cursor. */
	bool BuildingGeometryPreparation::IsValidSectionRegion(
		const BuildingCollectionRegion& sectionRegion) noexcept
	{
		const double right = sectionRegion.xMapUnits + sectionRegion.widthMapUnits;
		const double bottom = sectionRegion.yMapUnits + sectionRegion.heightMapUnits;
		return std::isfinite(sectionRegion.xMapUnits) && std::isfinite(sectionRegion.yMapUnits) &&
			std::isfinite(sectionRegion.widthMapUnits) && std::isfinite(sectionRegion.heightMapUnits) &&
			sectionRegion.widthMapUnits > 0.0 && sectionRegion.heightMapUnits > 0.0 &&
			std::isfinite(right) && std::isfinite(bottom) &&
			right > sectionRegion.xMapUnits && bottom > sectionRegion.yMapUnits;
	}

	/** Requires every continuation to use the exact rectangle retained by its cursor. */
	bool BuildingGeometryPreparation::MatchesSectionRegion(
		const BuildingCollectionRegion& first,
		const BuildingCollectionRegion& second) noexcept
	{
		return first.xMapUnits == second.xMapUnits && first.yMapUnits == second.yMapUnits &&
			first.widthMapUnits == second.widthMapUnits && first.heightMapUnits == second.heightMapUnits;
	}

	/** Clips a quad and assigns its canonical source-image coordinates without allocation. */
	bool BuildingGeometryPreparation::TryClipSpriteToSection(
		const SpriteWorldQuad& worldQuad,
		const BuildingCollectionRegion& sectionRegion,
		std::array<TexturedTriangle, 6>& triangles,
		std::size_t& triangleCount) noexcept
	{
		triangleCount = 0;
		if (!IsValidSectionRegion(sectionRegion))
		{
			return false;
		}
		std::array<TexturedTriangleVertex, 8> polygon{};
		const std::array<std::array<double, 2>, 4> textureCoordinates{{
			{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}}};
		for (std::size_t index = 0; index < worldQuad.Corners.size(); ++index)
		{
			polygon[index] = {worldQuad.Corners[index], textureCoordinates[index]};
		}
		std::array<TexturedTriangleVertex, 8> clippedPolygon{};
		std::size_t clippedCount = 0;
		if (!TryClipPolygonToSection(polygon, worldQuad.Corners.size(), sectionRegion, clippedPolygon, clippedCount))
		{
			return false;
		}
		return TryTriangulateSpritePolygon(clippedPolygon, clippedCount, triangles, triangleCount);
	}

	/** Clips a finite polygon through left, right, top and bottom in stable order. */
	bool BuildingGeometryPreparation::TryClipPolygonToSection(
		const std::array<TexturedTriangleVertex, 8>& input,
		std::size_t inputCount,
		const BuildingCollectionRegion& sectionRegion,
		std::array<TexturedTriangleVertex, 8>& output,
		std::size_t& outputCount) noexcept
	{
		if (!IsValidSectionRegion(sectionRegion) || inputCount > input.size())
		{
			return false;
		}
		for (std::size_t index = 0; index < inputCount; ++index)
		{
			if (!IsFiniteClipVertex(input[index]))
			{
				return false;
			}
		}
		std::array<TexturedTriangleVertex, 8> current = input;
		std::size_t currentCount = inputCount;
		const std::array<SectionClipBoundary, 4> boundaries{
			SectionClipBoundary::Left, SectionClipBoundary::Right,
			SectionClipBoundary::Top, SectionClipBoundary::Bottom};
		for (const SectionClipBoundary boundary : boundaries)
		{
			std::array<TexturedTriangleVertex, 8> next{};
			std::size_t nextCount = 0;
			if (!TryClipPolygonAgainstBoundary(current, currentCount, sectionRegion, boundary, next, nextCount))
			{
				return false;
			}
			current = next;
			currentCount = nextCount;
		}
		output = current;
		outputCount = currentCount;
		return true;
	}

	/** Applies one Sutherland-Hodgman half-plane step with exact boundary inclusion. */
	bool BuildingGeometryPreparation::TryClipPolygonAgainstBoundary(
		const std::array<TexturedTriangleVertex, 8>& input,
		std::size_t inputCount,
		const BuildingCollectionRegion& sectionRegion,
		SectionClipBoundary boundary,
		std::array<TexturedTriangleVertex, 8>& output,
		std::size_t& outputCount) noexcept
	{
		outputCount = 0;
		if (inputCount > input.size())
		{
			return false;
		}
		if (inputCount == 0)
		{
			return true;
		}
		TexturedTriangleVertex previous = input[inputCount - 1];
		bool previousInside = IsInsideSectionBoundary(previous, sectionRegion, boundary);
		for (std::size_t index = 0; index < inputCount; ++index)
		{
			const TexturedTriangleVertex& current = input[index];
			const bool currentInside = IsInsideSectionBoundary(current, sectionRegion, boundary);
			if (currentInside != previousInside)
			{
				TexturedTriangleVertex intersection;
				if (!TryInterpolateSectionBoundaryVertex(previous, current, sectionRegion, boundary, intersection) ||
					!TryAppendClipVertex(intersection, output, outputCount))
				{
					return false;
				}
			}
			if (currentInside && !TryAppendClipVertex(current, output, outputCount))
			{
				return false;
			}
			previous = current;
			previousInside = currentInside;
		}
		if (outputCount > 1 && AreClipVerticesEqual(output.front(), output[outputCount - 1]))
		{
			--outputCount;
		}
		return true;
	}

	/** Interpolates all stored attributes and pins the clipped axis to the shared edge value. */
	bool BuildingGeometryPreparation::TryInterpolateSectionBoundaryVertex(
		const TexturedTriangleVertex& first,
		const TexturedTriangleVertex& second,
		const BuildingCollectionRegion& sectionRegion,
		SectionClipBoundary boundary,
		TexturedTriangleVertex& output) noexcept
	{
		const bool clipX = boundary == SectionClipBoundary::Left || boundary == SectionClipBoundary::Right;
		const std::size_t axis = clipX ? 0 : 1;
		const double denominator = second.Position[axis] - first.Position[axis];
		if (!IsFiniteClipVertex(first) || !IsFiniteClipVertex(second) || denominator == 0.0 || !std::isfinite(denominator))
		{
			return false;
		}
		const double coordinate = SectionBoundaryCoordinate(sectionRegion, boundary);
		const double amount = (coordinate - first.Position[axis]) / denominator;
		if (!std::isfinite(amount) || amount < 0.0 || amount > 1.0)
		{
			return false;
		}
		for (std::size_t component = 0; component < output.Position.size(); ++component)
		{
			output.Position[component] = first.Position[component] + amount *
				(second.Position[component] - first.Position[component]);
		}
		for (std::size_t component = 0; component < output.TextureCoordinates.size(); ++component)
		{
			output.TextureCoordinates[component] = first.TextureCoordinates[component] + amount *
				(second.TextureCoordinates[component] - first.TextureCoordinates[component]);
		}
		output.Position[axis] = coordinate;
		return IsFiniteClipVertex(output);
	}

	/** Appends one finite vertex without exceeding the convex-quad clipping capacity. */
	bool BuildingGeometryPreparation::TryAppendClipVertex(
		const TexturedTriangleVertex& vertex,
		std::array<TexturedTriangleVertex, 8>& output,
		std::size_t& outputCount) noexcept
	{
		if (!IsFiniteClipVertex(vertex))
		{
			return false;
		}
		if (outputCount != 0 && AreClipVerticesEqual(output[outputCount - 1], vertex))
		{
			return true;
		}
		if (outputCount >= output.size())
		{
			return false;
		}
		output[outputCount] = vertex;
		++outputCount;
		return true;
	}

	/** Triangulates a clipped convex sprite polygon while omitting zero-area XY pieces. */
	bool BuildingGeometryPreparation::TryTriangulateSpritePolygon(
		const std::array<TexturedTriangleVertex, 8>& polygon,
		std::size_t polygonCount,
		std::array<TexturedTriangle, 6>& triangles,
		std::size_t& triangleCount) noexcept
	{
		triangleCount = 0;
		if (polygonCount < 3)
		{
			return true;
		}
		for (std::size_t index = 1; index + 1 < polygonCount; ++index)
		{
			TexturedTriangle triangle;
			triangle.Vertices = {polygon[0], polygon[index], polygon[index + 1]};
			if (!HasPositiveFiniteProjectedArea(triangle))
			{
				if (!std::isfinite(ProjectedSignedArea(triangle)))
				{
					return false;
				}
				continue;
			}
			if (triangleCount >= triangles.size())
			{
				return false;
			}
			triangles[triangleCount] = triangle;
			++triangleCount;
		}
		return true;
	}

	/** Clips post-occlusion triangles into reusable Section-owned result vectors. */
	bool BuildingGeometryPreparation::ClipVisibleTrianglesToSection(
		const BuildingCollectionRegion& sectionRegion,
		BuildingVisualsContext& scratch)
	{
		if (!IsValidSectionRegion(sectionRegion))
		{
			return false;
		}
		scratch.sectionClippedSolidTriangles_.clear();
		scratch.sectionClippedTexturedTriangles_.clear();
		scratch.sectionClippedSolidTriangles_.reserve(scratch.solidTriangles_.size());
		scratch.sectionClippedTexturedTriangles_.reserve(scratch.texturedTriangles_.size());
		for (const SolidTriangle& triangle : scratch.solidTriangles_)
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			const std::size_t priorCount = scratch.sectionClippedSolidTriangles_.size();
#endif
			if (!ClipSolidTriangleToSection(triangle, sectionRegion, scratch.sectionClippedSolidTriangles_))
			{
				return false;
			}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			const std::size_t addedCount = scratch.sectionClippedSolidTriangles_.size() - priorCount;
			if (addedCount == 0)
			{
				++BuildingMapPerformanceDiagnostics::Game().SplineSectionClipRemovedTriangles;
			}
			else if (addedCount > 1)
			{
				BuildingMapPerformanceDiagnostics::Game().SplineSectionClipFragmentTriangles += addedCount - 1;
			}
#endif
		}
		for (const TexturedTriangle& triangle : scratch.texturedTriangles_)
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			const std::size_t priorCount = scratch.sectionClippedTexturedTriangles_.size();
#endif
			if (!ClipTexturedTriangleToSection(triangle, sectionRegion, scratch.sectionClippedTexturedTriangles_))
			{
				return false;
			}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			const std::size_t addedCount = scratch.sectionClippedTexturedTriangles_.size() - priorCount;
			if (addedCount == 0)
			{
				++BuildingMapPerformanceDiagnostics::Game().SplineSectionClipRemovedTriangles;
			}
			else if (addedCount > 1)
			{
				BuildingMapPerformanceDiagnostics::Game().SplineSectionClipFragmentTriangles += addedCount - 1;
			}
#endif
		}
		return true;
	}

	/** Clips one solid triangle and appends its nonzero fan pieces in source order. */
	bool BuildingGeometryPreparation::ClipSolidTriangleToSection(
		const SolidTriangle& triangle,
		const BuildingCollectionRegion& sectionRegion,
		std::vector<SolidTriangle>& output)
	{
		std::array<TexturedTriangleVertex, 8> input{};
		for (std::size_t index = 0; index < triangle.Vertices.size(); ++index)
		{
			input[index].Position = triangle.Vertices[index];
		}
		std::array<TexturedTriangleVertex, 8> polygon{};
		std::size_t polygonCount = 0;
		if (!TryClipPolygonToSection(input, triangle.Vertices.size(), sectionRegion, polygon, polygonCount))
		{
			return false;
		}
		std::array<TexturedTriangle, 6> fan{};
		std::size_t fanCount = 0;
		if (!TryTriangulateSpritePolygon(polygon, polygonCount, fan, fanCount))
		{
			return false;
		}
		for (std::size_t triangleIndex = 0; triangleIndex < fanCount; ++triangleIndex)
		{
			SolidTriangle clipped;
			for (std::size_t vertex = 0; vertex < clipped.Vertices.size(); ++vertex)
			{
				clipped.Vertices[vertex] = fan[triangleIndex].Vertices[vertex].Position;
			}
			output.push_back(clipped);
		}
		return true;
	}

	/** Clips one textured triangle while linearly preserving every world and UV attribute. */
	bool BuildingGeometryPreparation::ClipTexturedTriangleToSection(
		const TexturedTriangle& triangle,
		const BuildingCollectionRegion& sectionRegion,
		std::vector<TexturedTriangle>& output)
	{
		std::array<TexturedTriangleVertex, 8> input{};
		for (std::size_t index = 0; index < triangle.Vertices.size(); ++index)
		{
			input[index] = triangle.Vertices[index];
		}
		std::array<TexturedTriangleVertex, 8> polygon{};
		std::size_t polygonCount = 0;
		if (!TryClipPolygonToSection(input, triangle.Vertices.size(), sectionRegion, polygon, polygonCount))
		{
			return false;
		}
		std::array<TexturedTriangle, 6> fan{};
		std::size_t fanCount = 0;
		if (!TryTriangulateSpritePolygon(polygon, polygonCount, fan, fanCount))
		{
			return false;
		}
		for (std::size_t triangleIndex = 0; triangleIndex < fanCount; ++triangleIndex)
		{
			output.push_back(fan[triangleIndex]);
		}
		return true;
	}

	/** Tests one vertex against an inclusive geometric half-plane at a shared Section edge. */
	bool BuildingGeometryPreparation::IsInsideSectionBoundary(
		const TexturedTriangleVertex& vertex,
		const BuildingCollectionRegion& sectionRegion,
		SectionClipBoundary boundary) noexcept
	{
		const double coordinate = vertex.Position[boundary == SectionClipBoundary::Left ||
			boundary == SectionClipBoundary::Right ? 0 : 1];
		const double edge = SectionBoundaryCoordinate(sectionRegion, boundary);
		return boundary == SectionClipBoundary::Left || boundary == SectionClipBoundary::Top
			? coordinate >= edge
			: coordinate <= edge;
	}

	/** Resolves the exact rectangle edge selected by a clipping boundary. */
	double BuildingGeometryPreparation::SectionBoundaryCoordinate(
		const BuildingCollectionRegion& sectionRegion,
		SectionClipBoundary boundary) noexcept
	{
		if (boundary == SectionClipBoundary::Left)
		{
			return sectionRegion.xMapUnits;
		}
		if (boundary == SectionClipBoundary::Right)
		{
			return sectionRegion.xMapUnits + sectionRegion.widthMapUnits;
		}
		if (boundary == SectionClipBoundary::Top)
		{
			return sectionRegion.yMapUnits;
		}
		return sectionRegion.yMapUnits + sectionRegion.heightMapUnits;
	}

	/** Rejects non-finite coordinates or UVs before they can enter prepared geometry. */
	bool BuildingGeometryPreparation::IsFiniteClipVertex(const TexturedTriangleVertex& vertex) noexcept
	{
		for (double coordinate : vertex.Position)
		{
			if (!std::isfinite(coordinate))
			{
				return false;
			}
		}
		return std::isfinite(vertex.TextureCoordinates[0]) && std::isfinite(vertex.TextureCoordinates[1]);
	}

	/** Compares all attributes so repeated corner intersections collapse deterministically. */
	bool BuildingGeometryPreparation::AreClipVerticesEqual(
		const TexturedTriangleVertex& first,
		const TexturedTriangleVertex& second) noexcept
	{
		return first.Position == second.Position && first.TextureCoordinates == second.TextureCoordinates;
	}

	/** Calculates signed doubled XY area and rejects non-finite projection results. */
	double BuildingGeometryPreparation::ProjectedSignedArea(const TexturedTriangle& triangle) noexcept
	{
		const auto& first = triangle.Vertices[0].Position;
		const auto& second = triangle.Vertices[1].Position;
		const auto& third = triangle.Vertices[2].Position;
		return (second[0] - first[0]) * (third[1] - first[1]) -
			(second[1] - first[1]) * (third[0] - first[0]);
	}

	/** Reports whether a solid triangle has nonzero finite XY coverage. */
	bool BuildingGeometryPreparation::HasPositiveFiniteProjectedArea(const SolidTriangle& triangle) noexcept
	{
		const double area = (triangle.Vertices[1][0] - triangle.Vertices[0][0]) *
			(triangle.Vertices[2][1] - triangle.Vertices[0][1]) -
			(triangle.Vertices[1][1] - triangle.Vertices[0][1]) *
			(triangle.Vertices[2][0] - triangle.Vertices[0][0]);
		return std::isfinite(area) && area != 0.0;
	}

	/** Reports whether a textured triangle has nonzero finite XY coverage. */
	bool BuildingGeometryPreparation::HasPositiveFiniteProjectedArea(const TexturedTriangle& triangle) noexcept
	{
		const double area = ProjectedSignedArea(triangle);
		return std::isfinite(area) && area != 0.0;
	}

	/** Validates direct sprite ownership and unique spline ownership for the complete scene. */
	bool BuildingGeometryPreparation::ValidateContributionOwnership(
		const BuildingRenderSceneInput& scene,
		const PreparedBuildingGeometry& output,
		PreparationCursor& cursor)
	{
		const std::size_t count = output.contributionStates_.size();
		if (output.spriteOccurrenceLocationsByOrdinal_.size() != count ||
			output.spritePainterOrder_.size() != count ||
			output.spritePainterCount_ > count || output.validSpritePainterCount_ > output.spritePainterCount_)
		{
			return false;
		}
		std::vector<const BuildingSplineRenderInput*> splineInputs(count, nullptr);
		for (const BuildingSplineRenderInput& spline : scene.Splines)
		{
			if (spline.SectionLocalOrdinal >= count)
			{
				RECORD_GEOMETRY_FAILURE("geometry initialization / spline ownership",
					"ordinal outside contribution slots", static_cast<long long>(spline.SectionLocalOrdinal));
				return false;
			}
			const std::size_t ordinal = static_cast<std::size_t>(spline.SectionLocalOrdinal);
			bool isOverlayLast = false;
			if (splineInputs[ordinal] != nullptr ||
				output.SpriteOccurrenceAtOrdinal(ordinal, isOverlayLast) != nullptr)
			{
				RECORD_GEOMETRY_FAILURE("geometry initialization / spline ownership",
					"ordinal already has an owner", static_cast<long long>(ordinal));
				return false;
			}
			splineInputs[ordinal] = &spline;
		}
		for (std::size_t ordinal = 0; ordinal < count; ++ordinal)
		{
			if (!ValidateContributionOccurrence(ordinal, splineInputs[ordinal], output, cursor))
			{
				return false;
			}
		}
		return true;
	}

	/** Checks that one publication ordinal has exactly one valid prepared owner or omission. */
	bool BuildingGeometryPreparation::ValidateContributionOccurrence(
		std::size_t ordinal,
		const BuildingSplineRenderInput* spline,
		const PreparedBuildingGeometry& output,
		PreparationCursor& cursor)
	{
		const auto state = output.contributionStates_[ordinal];
		bool isOverlayLast = false;
		const auto* sprite = output.SpriteOccurrenceAtOrdinal(ordinal, isOverlayLast);
		const bool spriteState = state == PreparedBuildingGeometry::ContributionStatus::Prepared ||
			state == PreparedBuildingGeometry::ContributionStatus::GeometricOmission ||
			(state == PreparedBuildingGeometry::ContributionStatus::TerminalOmission && sprite != nullptr);
		if (state == PreparedBuildingGeometry::ContributionStatus::Failed ||
			state == PreparedBuildingGeometry::ContributionStatus::Unresolved ||
			(sprite != nullptr && spline != nullptr) ||
			(spriteState && sprite == nullptr) ||
			(state == PreparedBuildingGeometry::ContributionStatus::NotProcessed && spline == nullptr) ||
			(state == PreparedBuildingGeometry::ContributionStatus::TerminalOmission &&
				(spline != nullptr || (sprite != nullptr && sprite->SectionTriangleCount != 0))))
		{
			RECORD_GEOMETRY_FAILURE("geometry initialization / occurrence validation",
				"sprite, spline, or terminal state is inconsistent", static_cast<long long>(ordinal),
				-1, 0, static_cast<unsigned>(state));
			return false;
		}
		if (sprite != nullptr && (sprite->Payload == nullptr || sprite->SourceKey.Bytes().empty() ||
			(state == PreparedBuildingGeometry::ContributionStatus::Prepared && sprite->SectionTriangleCount == 0) ||
			((state == PreparedBuildingGeometry::ContributionStatus::GeometricOmission ||
				state == PreparedBuildingGeometry::ContributionStatus::TerminalOmission) &&
				sprite->SectionTriangleCount != 0)))
		{
			RECORD_GEOMETRY_FAILURE("geometry initialization / sprite occurrence validation",
				"payload, key, or clipped geometry is inconsistent", static_cast<long long>(ordinal));
			return false;
		}
		return true;
	}
	/** Seeds world-space occluder inputs from already retained ordinary sprite values. */
	bool BuildingGeometryPreparation::SeedPreparedOccluders(
		const PreparedBuildingGeometry& output,
		BuildingVisualsContext& scratch)
	{
		for (std::size_t orderIndex = 0; orderIndex < output.spritePainterCount_; ++orderIndex)
		{
			bool isOverlayLast = false;
			const auto* occurrence = output.SpriteOccurrenceAtOrdinal(
				output.spritePainterOrder_[orderIndex], isOverlayLast);
			if (occurrence == nullptr || isOverlayLast || !occurrence->IsOccluder)
			{
				continue;
			}
			SplineOcclusionSpriteQuad occluder;
			occluder.TopHeight = occurrence->OccluderTopHeight;
			occluder.Key = occurrence->SectionLocalOrdinal;
			for (std::size_t corner = 0; corner < occurrence->SourceWorldQuad.Corners.size(); ++corner)
			{
				occluder.Corners[corner] = {
					occurrence->SourceWorldQuad.Corners[corner][0], occurrence->SourceWorldQuad.Corners[corner][1]};
			}
			scratch.occluders_.push_back(occluder);
		}
		return true;
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
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			BuildingMapPerformanceDiagnostics::Game().SplineSamplingIntervals +=
				scratch.preparedSplines_[splineIndex].AcceptedIntervals;
#endif
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
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		if (alternative)
		{
			++BuildingMapPerformanceDiagnostics::Game().SplineCandidateAttempts;
		}
#endif
		const bool drawable = PrepareSplineCandidateGeometry(
			spline, preparedSpline, surface, splineIndex, surfaceIndex, scratch, outcome);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		BuildingMapPerformanceDiagnostics::Game().SplineRejectedCells += outcome.RejectedCells;
#endif
		if (alternative && outcome.GeometryPrepared && outcome.SurfaceIssue == SplineSurfaceIssue::None &&
			outcome.RejectedCells != 0)
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			++BuildingMapPerformanceDiagnostics::Game().SplineCandidateRejections;
#endif
			cursor.Results.SplineSurfaces.push_back(outcome);
			cursor.SplineHasNumericalCandidateRejection = true;
			++cursor.SplineSurfaceIndex;
			return Status::Pending;
		}
		if (alternative && !drawable)
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			++BuildingMapPerformanceDiagnostics::Game().SplineCandidateRejections;
#endif
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

	/** Records one isolated clipping rejection and advances the existing spline cursor. */
	BuildingGeometryPreparation::Status BuildingGeometryPreparation::HandleNumericalClipFailure(
		const BuildingSplineRenderInput& spline,
		std::size_t splineIndex,
		std::size_t surfaceIndex,
		bool alternative,
		PreparedBuildingGeometry& output,
		PreparationCursor& cursor,
		BuildingVisualsContributorOutcome& outcome)
	{
		cursor.Results.SplineSurfaces.push_back(outcome);
		cursor.SplineHasNumericalClippingFailure = true;
		cursor.SplineHasNumericalCandidateRejection = true;
		cursor.PendingSplineSurfaces.clear();
		if (alternative)
		{
			cursor.Results.SplineSurfaces.back().AlternativeStatus =
				BuildingSplineAlternativeStatus::CandidateRejected;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			++BuildingMapPerformanceDiagnostics::Game().SplineCandidateRejections;
#endif
			++cursor.SplineSurfaceIndex;
			return Status::Pending;
		}
		cursor.Results.Complete = false;
		cursor.HasAcceptedOmissions = true;
		output.terminalSplineOmissionReason_ = BuildingSplineOmissionReason::NumericalClippingFailure;
		output.terminalSplineOmissionOrdinals_.push_back(spline.SectionLocalOrdinal);
		if (!SetContributionStatus(output, spline.SectionLocalOrdinal,
			PreparedBuildingGeometry::ContributionStatus::TerminalOmission))
		{
			RECORD_GEOMETRY_FAILURE("spline omission", "could not set contribution TerminalOmission",
				static_cast<long long>(spline.SectionLocalOrdinal), static_cast<long long>(splineIndex),
				0, 0, static_cast<long long>(surfaceIndex));
			return Status::InvalidInputs;
		}
		FinishSpline(spline.SectionLocalOrdinal, output, cursor, true);
		return Status::Pending;
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
		if (status == Status::NumericalFailure &&
			outcome.FailureStage == BuildingVisualsFailureStage::Occlusion &&
			outcome.OcclusionStatus == SplineOcclusionStatus::NumericalFailure)
		{
			return HandleNumericalClipFailure(
				spline, splineIndex, surfaceIndex, alternative, output, cursor, outcome);
		}
		if (status != Status::Pending)
		{
			cursor.Results.SplineSurfaces.push_back(outcome);
			SetContributionStatus(output, spline.SectionLocalOrdinal,
				PreparedBuildingGeometry::ContributionStatus::Failed);
			cursor.Results.Complete = false;
			return status;
		}
		if (outcome.SectionClippedAway)
		{
			outcome.ContributorIndex = splineIndex;
			return FinishSplineSectionExclusion(
				spline, splineIndex, alternative, output, cursor, outcome);
		}
		cursor.PendingSplineSurfaces.back().SourceDescriptorIndex = splineIndex;
		cursor.PendingSplineSurfaces.back().SelectedAlternative = alternative;
		cursor.SplineHasDrawableSurface = true;
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
			CommitPendingSplineSurfaces(output, cursor);
			FinishSpline(spline.SectionLocalOrdinal, output, cursor, false);
		}
		else
		{
			++cursor.SplineSurfaceIndex;
			if (cursor.SplineSurfaceIndex == spline.Surfaces.size())
			{
				CommitPendingSplineSurfaces(output, cursor);
				FinishSpline(spline.SectionLocalOrdinal, output, cursor, false);
			}
		}
		return Status::Pending;
	}

	/** Advances a valid but Section-empty surface without publishing zero-area triangles. */
	BuildingGeometryPreparation::Status BuildingGeometryPreparation::FinishSplineSectionExclusion(
		const BuildingSplineRenderInput& spline,
		std::size_t splineIndex,
		bool alternative,
		PreparedBuildingGeometry& output,
		PreparationCursor& cursor,
		BuildingVisualsContributorOutcome& outcome)
	{
		cursor.Results.SplineSurfaces.push_back(outcome);
		if (alternative)
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			++BuildingMapPerformanceDiagnostics::Game().SplineCandidateRejections;
#endif
			outcome.AlternativeStatus = BuildingSplineAlternativeStatus::NoDrawableAlternative;
			cursor.Results.SplineSurfaces.back().AlternativeStatus = outcome.AlternativeStatus;
			cursor.Results.SplineSurfaces.back().FailureStage = BuildingVisualsFailureStage::SurfacePreparation;
			cursor.Results.Complete = false;
			cursor.HasAcceptedOmissions = true;
			if (cursor.SplineHasNumericalClippingFailure)
			{
				output.terminalSplineOmissionReason_ = BuildingSplineOmissionReason::NumericalClippingFailure;
			}
			else if (output.terminalSplineOmissionOrdinals_.empty() &&
				cursor.SplineHasNumericalCandidateRejection)
			{
				output.terminalSplineOmissionReason_ = BuildingSplineOmissionReason::NumericalRejection;
			}
			output.terminalSplineOmissionOrdinals_.push_back(spline.SectionLocalOrdinal);
			FinishSpline(spline.SectionLocalOrdinal, output, cursor, true);
			return Status::Pending;
		}
		++cursor.SplineSurfaceIndex;
		if (cursor.SplineSurfaceIndex == spline.Surfaces.size())
		{
			const bool omitted = !cursor.SplineHasDrawableSurface;
			if (omitted)
			{
				cursor.Results.Complete = false;
				cursor.HasAcceptedOmissions = true;
				output.terminalSplineOmissionOrdinals_.push_back(spline.SectionLocalOrdinal);
			}
			else
			{
				CommitPendingSplineSurfaces(output, cursor);
			}
			FinishSpline(spline.SectionLocalOrdinal, output, cursor, omitted);
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
		if (!PrepareVisibleTriangles(surface, cursor.SectionRegion, scratch, cursor, outcome))
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			cursor.Failure.PublicationOrdinal = static_cast<long long>(spline.SectionLocalOrdinal);
			cursor.Failure.DescriptorIndex = static_cast<long long>(splineIndex);
			cursor.Failure.SurfaceIndex = static_cast<long long>(surfaceIndex);
#endif
			return Status::NumericalFailure;
		}
		if (scratch.sectionClippedSolidTriangles_.empty() &&
			scratch.sectionClippedTexturedTriangles_.empty())
		{
			outcome.SectionClippedAway = true;
			outcome.GeometryPrepared = true;
			outcome.ContributorIndex = splineIndex;
			return Status::Pending;
		}
		AppendPreparedSurface(spline, surface, surfaceIndex, scratch, output, cursor);
		outcome.GeometryPrepared = true;
		outcome.ContributorIndex = splineIndex;
		return Status::Pending;
	}

	/** Clips one surface and converts only its active numeric appearance into scratch vectors. */
	bool BuildingGeometryPreparation::PrepareVisibleTriangles(
		const BuildingSplineSurfaceRenderInput& surface,
		const BuildingCollectionRegion& sectionRegion,
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
		if (!ClipVisibleTrianglesToSection(sectionRegion, scratch))
		{
			outcome.FailureStage = BuildingVisualsFailureStage::SurfacePreparation;
			RECORD_GEOMETRY_FAILURE("Section boundary clipping", "visible triangle clipping failed");
			return false;
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		BuildingMapPerformanceDiagnostics::Game().SplinePostSectionClipTriangles +=
			scratch.sectionClippedSolidTriangles_.size() +
			scratch.sectionClippedTexturedTriangles_.size();
#endif
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
			prepared.BindingSlot = SplineTextureBindingSlot::SharedOpaqueWhite;
			prepared.SolidTriangles = std::move(scratch.sectionClippedSolidTriangles_);
		}
		else
		{
			prepared.TexturedTriangles = std::move(scratch.sectionClippedTexturedTriangles_);
		}
		PreparedBuildingGeometry::PrepareSplineSurfaceDrawLayout(prepared);
		cursor.PendingSplineSurfaces.push_back(std::move(prepared));
	}

	/** Commits all staged surfaces only after a draw-all spline finishes successfully. */
	void BuildingGeometryPreparation::CommitPendingSplineSurfaces(
		PreparedBuildingGeometry& output,
		PreparationCursor& cursor)
	{
		const std::size_t requiredSize = output.splineSurfaces_.size() + cursor.PendingSplineSurfaces.size();
		output.splineSurfaces_.reserve(requiredSize);
		for (auto& surface : cursor.PendingSplineSurfaces)
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			const std::size_t selectedTriangleCount =
				surface.SolidTriangles.size() + surface.TexturedTriangles.size();
#endif
			output.drawableGpuTriangleCount_ += surface.DrawSplineTriangles.size();
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			const bool selectedAlternative = surface.SelectedAlternative;
#endif
			output.splineSurfaces_.push_back(std::move(surface));
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			BuildingMapPerformanceDiagnostics::Game().SplineSelectedTriangles += selectedTriangleCount;
			if (selectedAlternative)
			{
				++BuildingMapPerformanceDiagnostics::Game().SplineCandidateSelections;
			}
#endif
		}
		cursor.PendingSplineSurfaces.clear();
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
		outcome.FailureStage = cursor.SplineHasNumericalClippingFailure
			? BuildingVisualsFailureStage::Occlusion
			: BuildingVisualsFailureStage::SurfacePreparation;
		outcome.SamplingIssue = scratch.preparedSplines_[splineIndex].Issue;
		outcome.SamplingComplete = scratch.preparedSplines_[splineIndex].IsComplete();
		cursor.Results.SplineSurfaces.push_back(outcome);
		cursor.Results.Complete = false;
		cursor.HasAcceptedOmissions = true;
		if (cursor.SplineHasNumericalClippingFailure)
		{
			output.terminalSplineOmissionReason_ = BuildingSplineOmissionReason::NumericalClippingFailure;
		}
		else if (output.terminalSplineOmissionOrdinals_.empty() && cursor.SplineHasNumericalCandidateRejection)
		{
			output.terminalSplineOmissionReason_ = BuildingSplineOmissionReason::NumericalRejection;
		}
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
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		if (omitted)
		{
			++BuildingMapPerformanceDiagnostics::Game().SplineTerminalOmissions;
		}
#endif
		SetContributionStatus(output, ordinal, omitted
			? PreparedBuildingGeometry::ContributionStatus::TerminalOmission
			: PreparedBuildingGeometry::ContributionStatus::Prepared);
		++cursor.SplineIndex;
		cursor.SplineSurfaceIndex = 0;
		cursor.SplineSamplingPrepared = false;
		cursor.SplineHasDrawableSurface = false;
		cursor.SplineHasNumericalCandidateRejection = false;
		cursor.SplineHasNumericalClippingFailure = false;
		cursor.PendingSplineSurfaces.clear();
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
