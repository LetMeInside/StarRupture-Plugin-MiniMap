// Builds reusable XY spline surface envelopes and triangle geometry from neutral numerical inputs.
// Source segments and prepared samples remain caller-owned; output storage is reusable by its caller.
#pragma once

#include "SplineGeometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace Services::Map::Buildings::Visuals
{
	/** Stores one canonical world-space surface vertex and its longitudinal/lateral UV pair. */
	struct SplineSurfaceVertex
	{
		std::array<double, 3> Position{};
		std::array<double, 2> TextureCoordinates{};
		bool operator==(const SplineSurfaceVertex&) const = default;
		/** Orders vertices by position and then UV for deterministic clipping-edge provenance. */
		bool operator<(const SplineSurfaceVertex& other) const noexcept
		{
			return Position < other.Position || (Position == other.Position && TextureCoordinates < other.TextureCoordinates);
		}
	};

	/** Describes a numerical transform used to orient cross-sections around world-space spline controls. */
	struct SplineSurfaceTransform
	{
		std::array<double, 3> Translation{};
		std::array<double, 3> Scale{1.0, 1.0, 1.0};
		std::array<double, 4> Rotation{0.0, 0.0, 0.0, 1.0};
	};

	/** Holds numerical local bounds used to project a spline cross-section into the world frame. */
	struct SplineSurfaceBounds
	{
		std::array<double, 3> Origin{};
		std::array<double, 3> Extent{};
	};

	/** Supplies the fixed cross-section dimensions, offset and up direction used by ribbon projection. */
	struct SplineSurfaceCrossSection
	{
		std::array<double, 2> Scale{1.0, 1.0};
		std::array<double, 2> Offset{};
		std::array<double, 3> Up{0.0, 0.0, 1.0};
	};

	/** Stores one projected ring's world-space path values, cumulative UV distance, and lateral envelope. */
	struct SplineSurfaceRing
	{
		std::array<double, 2> Position{};
		double PathHeight = 0.0;
		double LongitudinalDistance = 0.0;
		std::array<double, 2> LateralExtent{};
		std::array<double, 2> LateralDirection{};
		std::array<std::array<double, 2>, 2> Edges{};
	};

	/** Maps one accepted ribbon cell to its source interval and reusable vertex/index output ranges. */
	struct SplineSurfaceCell
	{
		std::size_t SourceSegment = 0;
		std::size_t FirstVertex = 0;
		std::size_t VertexCount = 0;
		std::size_t FirstIndex = 0;
		std::size_t IndexCount = 0;
		double StartParameter = 0.0;
		double EndParameter = 1.0;
		SplineSurfaceRing Start;
		SplineSurfaceRing End;
	};

	/** Reports whether surface preparation rejected structurally invalid numerical inputs. */
	enum class SplineSurfaceIssue : std::uint8_t
	{
		None,
		InvalidInput
	};

	/** Owns caller-reusable XYZ/UV surface vertices, counter-clockwise indices, and accepted projected cell metadata.
	 * XY carries the projected ribbon boundary; Z carries the sampled spline centerline height at the
	 * corresponding longitudinal endpoint, matching established occlusion semantics. U stores cumulative
	 * sampled XYZ centerline distance and V spans the lateral edges from zero to one.
	 */
	struct PreparedSplineSurface
	{
		std::vector<SplineSurfaceVertex> Vertices;
		std::vector<std::uint32_t> Indices;
		std::vector<SplineSurfaceCell> Cells;
		SplineSurfaceIssue Issue = SplineSurfaceIssue::None;
		std::size_t RejectedCells = 0;
		bool SamplingComplete = false;
	};

	/** Projects prepared spline samples into validated reusable height-aware ribbon cells. */
	class SplineSurfaceGeometry final
	{
		/** Holds validated transform values and inverse data for one Prepare invocation. */
		struct PreparedTransform
		{
			std::array<double, 3> Translation{};
			std::array<double, 3> Scale{1.0, 1.0, 1.0};
			std::array<double, 4> Rotation{0.0, 0.0, 0.0, 1.0};
			std::array<double, 4> InverseRotation{0.0, 0.0, 0.0, 1.0};
			bool CanInvertScale = true;
		};

	public:
		/**
		 * Builds independent height-aware ribbon cells from neutral spline inputs and retains output capacity.
		 * Source segment storage must remain valid during this call; no input is retained.
		 *
		 * @param segments Caller-owned source segments corresponding positionally to prepared sections.
		 * @param spline Reusable sampled geometry whose source parameters define cell boundaries.
		 * @param root Transform mapping cross-section offsets into the world frame.
		 * @param component Transform mapping fixed section coordinates into the root frame.
		 * @param bounds Local cross-section bounds in the component coordinate frame.
		 * @param crossSection Fixed numerical width, offset and up-direction parameters.
		 * @param output Receives canonical XYZ/UV vertices, indices and accepted cells while retaining capacity.
		 */
		static void Prepare(
			std::span<const SplineCurveSegment> segments,
			const PreparedSpline& spline,
			const SplineSurfaceTransform& root,
			const SplineSurfaceTransform& component,
			const SplineSurfaceBounds& bounds,
			const SplineSurfaceCrossSection& crossSection,
			PreparedSplineSurface& output)
		{
			Reset(output);
			output.SamplingComplete = spline.IsComplete();
			PreparedTransform preparedRoot;
			PreparedTransform preparedComponent;
			if (!PrepareTransform(root, preparedRoot) || !PrepareTransform(component, preparedComponent) ||
				!HasValidInputs(segments, spline, bounds, crossSection))
			{
				output.Issue = SplineSurfaceIssue::InvalidInput;
				return;
			}

			for (std::size_t segmentIndex = 0; segmentIndex < segments.size(); ++segmentIndex)
			{
				PrepareSegment(segments, spline, segmentIndex, preparedRoot, preparedComponent,
					bounds, crossSection, output);
			}
		}

	private:
		/** Clears prior geometry and status without releasing reusable vector storage.
		 * @param output Caller-owned geometry storage to reset.
		 */
		static void Reset(PreparedSplineSurface& output) noexcept
		{
			output.Vertices.clear();
			output.Indices.clear();
			output.Cells.clear();
			output.Issue = SplineSurfaceIssue::None;
			output.RejectedCells = 0;
			output.SamplingComplete = false;
		}

		/** Checks source/sample correspondence and finite projection inputs.
		 * @param segments Source segments corresponding to sampled sections.
		 * @param spline Prepared sample and section arrays.
		 * @param bounds Local cross-section bounds.
		 * @param crossSection Fixed section scale, offset and up direction.
		 * @return True when the inputs can be processed.
		 */
		static bool HasValidInputs(
			std::span<const SplineCurveSegment> segments,
			const PreparedSpline& spline,
			const SplineSurfaceBounds& bounds,
			const SplineSurfaceCrossSection& crossSection) noexcept
		{
			if (segments.empty() || spline.Issue != SplineNumericalIssue::None ||
				spline.Sections.size() != segments.size() || !IsValidBounds(bounds) || !IsFinite(crossSection.Up))
			{
				return false;
			}
			return std::isfinite(crossSection.Scale[0]) && std::isfinite(crossSection.Scale[1]) &&
				std::isfinite(crossSection.Offset[0]) && std::isfinite(crossSection.Offset[1]);
		}

		/** Validates one public transform and prepares its invariant forward and inverse values.
		 * @param source Caller-supplied transform to validate.
		 * @param prepared Receives transform values reused by every sample in this invocation.
		 * @return True when transform values and quaternion are valid.
		 */
		static bool PrepareTransform(
			const SplineSurfaceTransform& source,
			PreparedTransform& prepared) noexcept
		{
			if (!IsValidTransform(source))
			{
				return false;
			}
			prepared.Translation = source.Translation;
			prepared.Scale = source.Scale;
			prepared.Rotation = source.Rotation;
			prepared.InverseRotation = {
				-source.Rotation[0], -source.Rotation[1], -source.Rotation[2], source.Rotation[3]};
			for (std::size_t axis = 0; axis < 3; ++axis)
			{
				if (source.Scale[axis] == 0.0)
				{
					prepared.CanInvertScale = false;
					continue;
				}
				const double inverseScale = 1.0 / source.Scale[axis];
				if (!std::isfinite(inverseScale))
				{
					prepared.CanInvertScale = false;
				}
			}
			return true;
		}

		/** Verifies finite transform values and the unit quaternion convention used by geometry transforms.
		 * @param transform Translation, signed scale and rotation to validate.
		 * @return True when its values and quaternion are valid.
		 */
		static bool IsValidTransform(const SplineSurfaceTransform& transform) noexcept
		{
			if (!IsFinite(transform.Translation) || !IsFinite(transform.Scale))
			{
				return false;
			}
			double norm = 0.0;
			for (const double value : transform.Rotation)
			{
				if (!std::isfinite(value))
				{
					return false;
				}
				norm += value * value;
			}
			return std::isfinite(norm) && std::abs(norm - 1.0) <= 1e-6;
		}

		/** Validates finite local bounds with nonnegative extents.
		 * @param bounds Local origin and half extents to validate.
		 * @return True when all extents are nonnegative and all values are finite.
		 */
		static bool IsValidBounds(const SplineSurfaceBounds& bounds) noexcept
		{
			if (!IsFinite(bounds.Origin) || !IsFinite(bounds.Extent))
			{
				return false;
			}
			return bounds.Extent[0] >= 0.0 && bounds.Extent[1] >= 0.0 && bounds.Extent[2] >= 0.0;
		}

		/** Tests whether an XYZ point contains only finite coordinates.
		 * @param point XYZ value to check.
		 * @return True when every coordinate is finite.
		 */
		static bool IsFinite(const std::array<double, 3>& point) noexcept
		{
			return std::isfinite(point[0]) && std::isfinite(point[1]) && std::isfinite(point[2]);
		}

		/** Processes every adjacent prepared sample pair for one source segment.
		 * @param segments Source segments whose ordering matches prepared sections.
		 * @param spline Prepared sample and section data.
		 * @param segmentIndex Segment and section index to process.
		 * @param root World transform for projected offsets.
		 * @param component Local component transform.
		 * @param bounds Local cross-section bounds.
		 * @param crossSection Fixed section scale, offset and up direction.
		 * @param output Receives accepted cells and rejected-cell counts.
		 */
		static void PrepareSegment(
			std::span<const SplineCurveSegment> segments,
			const PreparedSpline& spline,
			std::size_t segmentIndex,
			const PreparedTransform& root,
			const PreparedTransform& component,
			const SplineSurfaceBounds& bounds,
			const SplineSurfaceCrossSection& crossSection,
			PreparedSplineSurface& output)
		{
			const auto& segment = segments[segmentIndex];
			const auto& section = spline.Sections[segmentIndex];
			if (section.Discontinuous || segment.Interpolation == SplineInterpolation::Constant)
			{
				return;
			}
			if (!HasValidSampleRange(section, spline.Samples.size()))
			{
				output.Issue = SplineSurfaceIssue::InvalidInput;
				return;
			}

			SplineSurfaceRing previousRing;
			const auto& firstSample = spline.Samples[section.FirstSample];
			double previousParameter = firstSample.Parameter;
			bool previousValid = ProjectSample(
				segment, firstSample, root, component, bounds, crossSection, previousRing);
			for (std::size_t sampleIndex = 1; sampleIndex < section.SampleCount; ++sampleIndex)
			{
				const auto& currentSample = spline.Samples[section.FirstSample + sampleIndex];
				SplineSurfaceRing currentRing;
				const bool currentValid = ProjectSample(
					segment, currentSample, root, component, bounds, crossSection, currentRing);
				if (!previousValid || !currentValid)
				{
					output.RejectedCells += 1;
				}
				else
				{
					PrepareCell(segment, segmentIndex, previousParameter, currentSample.Parameter,
						previousRing, currentRing, root, component, crossSection, output);
				}
				previousRing = currentRing;
				previousParameter = currentSample.Parameter;
				previousValid = currentValid;
			}
		}

		/** Verifies a section's sample range without overflowing the source sample count.
		 * @param section Prepared sample range to validate.
		 * @param sampleCount Number of samples available.
		 * @return True when at least two samples fit inside the available range.
		 */
		static bool HasValidSampleRange(const SplineCurveSection& section, std::size_t sampleCount) noexcept
		{
			return section.FirstSample <= sampleCount &&
				section.SampleCount <= sampleCount - section.FirstSample && section.SampleCount >= 2;
		}

		/** Evaluates one prepared sample and projects its reusable ring geometry.
		 * @param segment Source segment evaluated at the sample parameter.
		 * @param sample Prepared parameter for this ring.
		 * @param root World transform for projected offsets.
		 * @param component Local component transform.
		 * @param bounds Local cross-section bounds.
		 * @param crossSection Fixed section scale, offset and up direction.
		 * @param ring Receives the projected ring on success.
		 * @return True when evaluation and projection produce a valid ring.
		 */
		static bool ProjectSample(
			const SplineCurveSegment& segment,
			const SplineSample& sample,
			const PreparedTransform& root,
			const PreparedTransform& component,
			const SplineSurfaceBounds& bounds,
			const SplineSurfaceCrossSection& crossSection,
			SplineSurfaceRing& ring) noexcept
		{
			SplineEvaluation evaluation;
			return SplineGeometry::Evaluate(segment, sample.Parameter, evaluation) &&
				ProjectRing(root, component, bounds, crossSection, evaluation, sample.CumulativeDistance, ring);
		}

		/** Appends one pair of adjacent projected rings when its interval and ribbon are valid.
		 * @param segment Source segment used for cubic interior-frame validation.
		 * @param segmentIndex Source segment index stored in the cell metadata.
		 * @param firstParameter Normalized parameter at the start ring.
		 * @param lastParameter Normalized parameter at the end ring.
		 * @param firstRing Projected start boundary ring.
		 * @param lastRing Projected end boundary ring.
		 * @param root World transform for frame validation.
		 * @param component Local component transform for frame validation.
		 * @param crossSection Up direction for frame validation.
		 * @param output Receives the cell or a rejected-cell count.
		 */
		static void PrepareCell(
			const SplineCurveSegment& segment,
			std::size_t segmentIndex,
			double firstParameter,
			double lastParameter,
			const SplineSurfaceRing& firstRing,
			const SplineSurfaceRing& lastRing,
			const PreparedTransform& root,
			const PreparedTransform& component,
			const SplineSurfaceCrossSection& crossSection,
			PreparedSplineSurface& output)
		{
			if (!IsValidParameterPair(firstParameter, lastParameter))
			{
				output.RejectedCells += 1;
				return;
			}

			SplineSurfaceCell cell;
			cell.SourceSegment = segmentIndex;
			cell.StartParameter = firstParameter;
			cell.EndParameter = lastParameter;
			cell.Start = firstRing;
			cell.End = lastRing;

			if (segment.Interpolation == SplineInterpolation::Cubic &&
				!HasValidInteriorFrame(root, component, crossSection.Up,
					Subcurve(segment.Controls, firstParameter, lastParameter)))
			{
				output.RejectedCells += 1;
				return;
			}
			if (!AppendRibbon(cell, output))
			{
				output.RejectedCells += 1;
			}
		}

		/** Checks the strictly increasing normalized sample interval used by one ribbon cell.
		 * @param first Normalized start parameter.
		 * @param last Normalized end parameter.
		 * @return True when the finite interval lies within [0, 1] and has positive span.
		 */
		static bool IsValidParameterPair(double first, double last) noexcept
		{
			return std::isfinite(first) && std::isfinite(last) && first >= 0.0 && last <= 1.0 && first < last;
		}

		/** Projects one evaluated tangent and its fixed local bounds into a world-XY ring.
		 * @param root Transform from spline world vectors into the root frame and back.
		 * @param component Transform for the local cross-section frame.
		 * @param bounds Local cross-section bounds.
		 * @param crossSection Fixed scale, offset and up direction.
		 * @param evaluation Position and source-key tangent at one sample.
		 * @param ring Receives the projected ring.
		 * @return True when the tangent frame and ring are valid.
		 */
		static bool ProjectRing(
			const PreparedTransform& root,
			const PreparedTransform& component,
			const SplineSurfaceBounds& bounds,
			const SplineSurfaceCrossSection& crossSection,
			const SplineEvaluation& evaluation,
			double longitudinalDistance,
			SplineSurfaceRing& ring) noexcept
		{
			std::array<double, 3> localTangent;
			std::array<double, 3> frameY;
			std::array<double, 3> frameZ;
			if (!evaluation.HasTangent || !Inverse(root, evaluation.Tangent, localTangent, true) ||
				!Inverse(component, localTangent, localTangent, true) ||
				!BuildFrame(localTangent, crossSection.Up, frameY, frameZ))
			{
				return false;
			}

			const double tangentLength = std::hypot(evaluation.Tangent[0], evaluation.Tangent[1]);
			if (!std::isfinite(tangentLength) || tangentLength == 0.0)
			{
				return false;
			}
			return BuildRing(root, component, bounds, crossSection, evaluation,
				frameY, frameZ, tangentLength, longitudinalDistance, ring);
		}

		/** Calculates normalized cross-section frame axes using the established up-cross-tangent order.
		 * @param tangent Local tangent to normalize in place.
		 * @param up Reference up direction.
		 * @param frameY Receives the normalized lateral axis.
		 * @param frameZ Receives the normalized cross-section depth axis.
		 * @return True when both frame axes are nondegenerate.
		 */
		static bool BuildFrame(
			std::array<double, 3> tangent,
			const std::array<double, 3>& up,
			std::array<double, 3>& frameY,
			std::array<double, 3>& frameZ) noexcept
		{
			std::array<double, 3> reference = up;
			if (!Normalize(tangent) || !Normalize(reference) || !Cross(reference, tangent, frameY) ||
				!Normalize(frameY))
			{
				return false;
			}
			return Cross(tangent, frameY, frameZ) && Normalize(frameZ);
		}

		/** Builds lateral extent and boundary edges from the four projected cross-section corners.
		 * @param root World transform for projected offsets.
		 * @param component Local component transform.
		 * @param bounds Local cross-section bounds.
		 * @param crossSection Fixed scale and offset.
		 * @param evaluation World-space path position and tangent.
		 * @param frameY Normalized lateral frame axis.
		 * @param frameZ Normalized depth frame axis.
		 * @param tangentLength XY magnitude used for the world lateral direction.
		 * @param longitudinalDistance Cumulative sampled XYZ centerline distance at this ring.
		 * @param ring Receives lateral extents and boundary edges.
		 * @return True when all projected corners form a finite envelope.
		 */
		static bool BuildRing(
			const PreparedTransform& root,
			const PreparedTransform& component,
			const SplineSurfaceBounds& bounds,
			const SplineSurfaceCrossSection& crossSection,
			const SplineEvaluation& evaluation,
			const std::array<double, 3>& frameY,
			const std::array<double, 3>& frameZ,
			double tangentLength,
			double longitudinalDistance,
			SplineSurfaceRing& ring) noexcept
		{
			if (!std::isfinite(evaluation.Position[2]))
			{
				return false;
			}

			ring.Position = {evaluation.Position[0], evaluation.Position[1]};
			ring.PathHeight = evaluation.Position[2];
			ring.LongitudinalDistance = longitudinalDistance;
			ring.LateralDirection = {-evaluation.Tangent[1] / tangentLength, evaluation.Tangent[0] / tangentLength};
			ring.LateralExtent = {std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
			const auto rangeY = Dimension(bounds, 1, crossSection.Scale[0]);
			const auto rangeZ = Dimension(bounds, 2, crossSection.Scale[1]);
			if (!IsValidInterval(rangeY) || !IsValidInterval(rangeZ))
			{
				return false;
			}
			return AccumulateCorners(root, component, crossSection, evaluation, frameY, frameZ,
				rangeY, rangeZ, ring) && BuildEdges(ring);
		}

		/** Accumulates projected corner displacements into a ring's lateral envelope.
		 * @param root World transform for projected offsets.
		 * @param component Local component transform.
		 * @param crossSection Fixed local offsets.
		 * @param evaluation World-space path position.
		 * @param frameY Normalized lateral frame axis.
		 * @param frameZ Normalized depth frame axis.
		 * @param rangeY Scaled low/high bounds in the lateral coordinate.
		 * @param rangeZ Scaled low/high bounds in the depth coordinate.
		 * @param ring Envelope expanded by each projected corner.
		 * @return True when every corner projects to finite values.
		 */
		static bool AccumulateCorners(
			const PreparedTransform& root,
			const PreparedTransform& component,
			const SplineSurfaceCrossSection& crossSection,
			const SplineEvaluation& evaluation,
			const std::array<double, 3>& frameY,
			const std::array<double, 3>& frameZ,
			const std::array<double, 2>& rangeY,
			const std::array<double, 2>& rangeZ,
			SplineSurfaceRing& ring) noexcept
		{
			for (std::size_t cornerIndex = 0; cornerIndex < 4; ++cornerIndex)
			{
				if (!AccumulateCorner(root, component, crossSection, evaluation, frameY, frameZ,
					rangeY, rangeZ, cornerIndex, ring))
				{
					return false;
				}
			}
			return IsValidInterval(ring.LateralExtent);
		}

		/** Transforms one cross-section corner and expands the ring's signed lateral extent.
		 * @param root World transform for projected offsets.
		 * @param component Local component transform.
		 * @param crossSection Fixed local offset.
		 * @param evaluation World-space path position.
		 * @param frameY Normalized lateral frame axis.
		 * @param frameZ Normalized depth frame axis.
		 * @param rangeY Scaled lateral coordinate bounds.
		 * @param rangeZ Scaled depth coordinate bounds.
		 * @param cornerIndex Selects one of the four bounds corners.
		 * @param ring Envelope expanded with this corner's displacement.
		 * @return True when the corner transforms to finite world coordinates.
		 */
		static bool AccumulateCorner(
			const PreparedTransform& root,
			const PreparedTransform& component,
			const SplineSurfaceCrossSection& crossSection,
			const SplineEvaluation& evaluation,
			const std::array<double, 3>& frameY,
			const std::array<double, 3>& frameZ,
			const std::array<double, 2>& rangeY,
			const std::array<double, 2>& rangeZ,
			std::size_t cornerIndex,
			SplineSurfaceRing& ring) noexcept
		{
			const double coordinateY = rangeY[(cornerIndex & 1) ? 1 : 0] + crossSection.Offset[0];
			const double coordinateZ = rangeZ[(cornerIndex & 2) ? 1 : 0] + crossSection.Offset[1];
			std::array<double, 3> local{};
			for (std::size_t axis = 0; axis < 3; ++axis)
			{
				local[axis] = frameY[axis] * coordinateY + frameZ[axis] * coordinateZ;
			}
			std::array<double, 3> componentOffset;
			std::array<double, 3> worldOffset;
			if (!TransformPoint(component, local, componentOffset) ||
				!TransformVector(root, componentOffset, worldOffset))
			{
				return false;
			}
			const double x = evaluation.Position[0] + worldOffset[0];
			const double y = evaluation.Position[1] + worldOffset[1];
			const double displacement = worldOffset[0] * ring.LateralDirection[0] +
				worldOffset[1] * ring.LateralDirection[1];
			if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(displacement))
			{
				return false;
			}
			ring.LateralExtent[0] = (std::min)(ring.LateralExtent[0], displacement);
			ring.LateralExtent[1] = (std::max)(ring.LateralExtent[1], displacement);
			return true;
		}

		/** Converts lateral extrema into the two world-XY ribbon boundary points.
		 * @param ring Ring whose extent and center define the edge positions.
		 * @return True when both boundary points are finite.
		 */
		static bool BuildEdges(SplineSurfaceRing& ring) noexcept
		{
			for (std::size_t edgeIndex = 0; edgeIndex < 2; ++edgeIndex)
			{
				const double displacement = ring.LateralExtent[edgeIndex];
				ring.Edges[edgeIndex] = {
					ring.Position[0] + ring.LateralDirection[0] * displacement,
					ring.Position[1] + ring.LateralDirection[1] * displacement};
				if (!std::isfinite(ring.Edges[edgeIndex][0]) || !std::isfinite(ring.Edges[edgeIndex][1]))
				{
					return false;
				}
			}
			return true;
		}

		/** Appends one non-folded ribbon cell using the established counter-clockwise diagonal selection.
		 * @param cell Source interval and projected start/end ring data.
		 * @param output Caller-owned geometry arrays receiving the accepted cell.
		 * @return True when valid counter-clockwise triangles were appended.
		 */
		static bool AppendRibbon(SplineSurfaceCell cell, PreparedSplineSurface& output)
		{
			std::array<std::array<double, 2>, 4> points{
				cell.Start.Edges[0], cell.End.Edges[0], cell.End.Edges[1], cell.Start.Edges[1]};
			if (!HasValidRibbonBoundaries(cell, points))
			{
				return false;
			}

			std::array<std::uint32_t, 6> indices;
			if (HasPositiveTrianglePair(points, {0, 1, 2}, {0, 2, 3}))
			{
				indices = {0, 1, 2, 0, 2, 3};
			}
			else if (HasPositiveTrianglePair(points, {0, 1, 3}, {1, 2, 3}))
			{
				indices = {0, 1, 3, 1, 2, 3};
			}
			else
			{
				return false;
			}
			return AppendCellGeometry(cell, points, indices, output);
		}

		/** Rejects collapsed, non-finite, folded or self-intersecting ribbon boundaries.
		 * @param cell Start/end ring data used to check progress and width.
		 * @param points Ordered start-low, end-low, end-high, start-high boundary points.
		 * @return True when the four points bound a non-folded simple ribbon.
		 */
		static bool HasValidRibbonBoundaries(
			const SplineSurfaceCell& cell,
			const std::array<std::array<double, 2>, 4>& points) noexcept
		{
			if (!IsValidInterval(cell.Start.LateralExtent) || !IsValidInterval(cell.End.LateralExtent) ||
				cell.Start.LateralExtent[0] == cell.Start.LateralExtent[1] ||
				cell.End.LateralExtent[0] == cell.End.LateralExtent[1] ||
				cell.Start.Position == cell.End.Position)
			{
				return false;
			}
			for (const auto& point : points)
			{
				if (!std::isfinite(point[0]) || !std::isfinite(point[1]))
				{
					return false;
				}
			}
			return AdvancesAcrossCell(cell) && !EdgesIntersect(points);
		}

		/** Ensures both cell sides advance from the start ring to the end ring without crossing their planes.
		 * @param cell Cell rings and tangent-normal directions to compare.
		 * @return True when both sides advance through their respective ring planes.
		 */
		static bool AdvancesAcrossCell(const SplineSurfaceCell& cell) noexcept
		{
			for (std::size_t side = 0; side < 2; ++side)
			{
				const double atStart = ForwardDistance(cell.Start, cell.End.Edges[side]);
				const double atEnd = ForwardDistance(cell.End, cell.Start.Edges[side]);
				if (!std::isfinite(atStart) || !std::isfinite(atEnd) || atStart <= 0.0 || atEnd >= 0.0)
				{
					return false;
				}
			}
			return true;
		}

		/** Measures signed forward distance from a ring center along its left-facing tangent normal.
		 * @param ring Center and lateral direction defining the signed distance.
		 * @param point World-XY boundary point to measure.
		 * @return Signed forward distance in world units.
		 */
		static double ForwardDistance(const SplineSurfaceRing& ring, const std::array<double, 2>& point) noexcept
		{
			return (point[0] - ring.Position[0]) * ring.LateralDirection[1] -
				(point[1] - ring.Position[1]) * ring.LateralDirection[0];
		}

		/** Tests whether either pair of opposite ribbon boundary edges intersects.
		 * @param points Ordered ribbon boundary points.
		 * @return True when either pair of opposite edges crosses or touches.
		 */
		static bool EdgesIntersect(const std::array<std::array<double, 2>, 4>& points) noexcept
		{
			return SegmentsIntersect(points[0], points[1], points[2], points[3]) ||
				SegmentsIntersect(points[1], points[2], points[3], points[0]);
		}

		/** Detects proper and collinear intersections between two finite XY line segments.
		 * @param firstStart First segment's start point.
		 * @param firstEnd First segment's end point.
		 * @param secondStart Second segment's start point.
		 * @param secondEnd Second segment's end point.
		 * @return True when segments intersect, touch, or produce non-finite area arithmetic.
		 */
		static bool SegmentsIntersect(
			const std::array<double, 2>& firstStart,
			const std::array<double, 2>& firstEnd,
			const std::array<double, 2>& secondStart,
			const std::array<double, 2>& secondEnd) noexcept
		{
			const double firstSideStart = SignedArea(firstStart, firstEnd, secondStart);
			const double firstSideEnd = SignedArea(firstStart, firstEnd, secondEnd);
			const double secondSideStart = SignedArea(secondStart, secondEnd, firstStart);
			const double secondSideEnd = SignedArea(secondStart, secondEnd, firstEnd);
			if (!std::isfinite(firstSideStart) || !std::isfinite(firstSideEnd) ||
				!std::isfinite(secondSideStart) || !std::isfinite(secondSideEnd))
			{
				return true;
			}
			return (HasOppositeSigns(firstSideStart, firstSideEnd) &&
				HasOppositeSigns(secondSideStart, secondSideEnd) ||
				IsCollinearPointOnSegment(firstStart, firstEnd, secondStart, firstSideStart) ||
				IsCollinearPointOnSegment(firstStart, firstEnd, secondEnd, firstSideEnd) ||
				IsCollinearPointOnSegment(secondStart, secondEnd, firstStart, secondSideStart) ||
				IsCollinearPointOnSegment(secondStart, secondEnd, firstEnd, secondSideEnd));
		}

		/** Calculates the signed doubled area of a triangle in XY.
		 * @param first First triangle point.
		 * @param second Second triangle point.
		 * @param third Third triangle point.
		 * @return Positive for counter-clockwise order and negative for clockwise order.
		 */
		static double SignedArea(
			const std::array<double, 2>& first,
			const std::array<double, 2>& second,
			const std::array<double, 2>& third) noexcept
		{
			return (second[0] - first[0]) * (third[1] - first[1]) -
				(second[1] - first[1]) * (third[0] - first[0]);
		}

		/** Reports strict opposite signs for finite signed-area values.
		 * @param first First signed-area value.
		 * @param second Second signed-area value.
		 * @return True when one value is positive and the other negative.
		 */
		static bool HasOppositeSigns(double first, double second) noexcept
		{
			return (first < 0.0 && second > 0.0) || (first > 0.0 && second < 0.0);
		}

		/** Tests whether a collinear point lies inside the inclusive bounds of a segment.
		 * @param start Segment start.
		 * @param end Segment end.
		 * @param point Candidate point.
		 * @param signedArea Area value used to confirm collinearity.
		 * @return True when the point lies on the closed segment.
		 */
		static bool IsCollinearPointOnSegment(
			const std::array<double, 2>& start,
			const std::array<double, 2>& end,
			const std::array<double, 2>& point,
			double signedArea) noexcept
		{
			return signedArea == 0.0 && point[0] >= (std::min)(start[0], end[0]) &&
				point[0] <= (std::max)(start[0], end[0]) &&
				point[1] >= (std::min)(start[1], end[1]) &&
				point[1] <= (std::max)(start[1], end[1]);
		}

		/** Selects one of the two established quad diagonals only when both triangles face counter-clockwise.
		 * @param points Ordered quad corners.
		 * @param first First candidate triangle indices.
		 * @param second Second candidate triangle indices.
		 * @return True when both triangles have finite positive winding.
		 */
		static bool HasPositiveTrianglePair(
			const std::array<std::array<double, 2>, 4>& points,
			const std::array<std::uint32_t, 3>& first,
			const std::array<std::uint32_t, 3>& second) noexcept
		{
			return IsPositiveTriangle(points[first[0]], points[first[1]], points[first[2]]) &&
				IsPositiveTriangle(points[second[0]], points[second[1]], points[second[2]]);
		}

		/** Verifies a finite, strictly positive triangle winding in world XY.
		 * @param first First triangle point.
		 * @param second Second triangle point.
		 * @param third Third triangle point.
		 * @return True when signed doubled area is finite and positive.
		 */
		static bool IsPositiveTriangle(
			const std::array<double, 2>& first,
			const std::array<double, 2>& second,
			const std::array<double, 2>& third) noexcept
		{
			const double area = SignedArea(first, second, third);
			return std::isfinite(area) && area > 0.0;
		}

		/** Appends one accepted four-vertex, six-index XYZ cell and records its output ranges.
		 * @param cell Source interval and boundary ring metadata.
		 * @param points Ordered start-low, end-low, end-high, start-high world-XY boundary vertices.
		 * @param indices Counter-clockwise local quad indices.
		 * @param output Caller-owned vectors receiving geometry and cell metadata.
		 * @return True when the vertex count remains representable by 32-bit indices.
		 */
		static bool AppendCellGeometry(
			SplineSurfaceCell cell,
			const std::array<std::array<double, 2>, 4>& points,
			const std::array<std::uint32_t, 6>& indices,
			PreparedSplineSurface& output)
		{
			if (output.Vertices.size() > UINT32_MAX - points.size())
			{
				return false;
			}

			// The legacy planner assigns sample-centerline Z by longitudinal endpoint,
			// not by the projected lateral extrema that supplied each XY boundary.
			const std::array<SplineSurfaceVertex, 4> worldPoints{{
				{{points[0][0], points[0][1], cell.Start.PathHeight}, {cell.Start.LongitudinalDistance, 0.0}},
				{{points[1][0], points[1][1], cell.End.PathHeight}, {cell.End.LongitudinalDistance, 0.0}},
				{{points[2][0], points[2][1], cell.End.PathHeight}, {cell.End.LongitudinalDistance, 1.0}},
				{{points[3][0], points[3][1], cell.Start.PathHeight}, {cell.Start.LongitudinalDistance, 1.0}}}};
			cell.FirstVertex = output.Vertices.size();
			cell.VertexCount = points.size();
			cell.FirstIndex = output.Indices.size();
			const auto baseIndex = static_cast<std::uint32_t>(cell.FirstVertex);
			output.Vertices.insert(output.Vertices.end(), worldPoints.begin(), worldPoints.end());
			for (const auto index : indices)
			{
				output.Indices.push_back(baseIndex + index);
			}
			cell.IndexCount = output.Indices.size() - cell.FirstIndex;
			output.Cells.push_back(cell);
			return true;
		}

		/** Checks whether one cubic cell's derivative controls exclude zero or an up-parallel tangent.
		 * @param root World transform used to recover the root-local tangent.
		 * @param component Component transform used to recover the section-local tangent.
		 * @param up Reference up direction for frame construction.
		 * @param controls Cubic controls for the cell's restricted subcurve.
		 * @return True when the derivative frame is nondegenerate across the cell.
		 */
		static bool HasValidInteriorFrame(
			const PreparedTransform& root,
			const PreparedTransform& component,
			const std::array<double, 3>& up,
			const std::array<std::array<double, 3>, 4>& controls) noexcept
		{
			std::array<double, 3> reference = up;
			if (!Normalize(reference))
			{
				return false;
			}
			std::array<std::array<double, 2>, 3> cone;
			for (auto& interval : cone)
			{
				interval = {INFINITY, -INFINITY};
			}
			return AccumulateFrameCone(root, component, reference, controls, cone) &&
				ExcludesDegenerateFrame(cone);
		}

		/** Accumulates component-space cross-product bounds for the cubic derivative control vectors.
		 * @param root World transform used to recover the root-local tangent.
		 * @param component Component transform used to recover the section-local tangent.
		 * @param reference Normalized up direction.
		 * @param controls Cubic control points bounding the derivative.
		 * @param cone Mutable low/high interval for each cross-product component.
		 * @return True when each derivative control transforms to a finite normal.
		 */
		static bool AccumulateFrameCone(
			const PreparedTransform& root,
			const PreparedTransform& component,
			const std::array<double, 3>& reference,
			const std::array<std::array<double, 3>, 4>& controls,
			std::array<std::array<double, 2>, 3>& cone) noexcept
		{
			for (std::size_t index = 0; index < 3; ++index)
			{
				std::array<double, 3> tangent{};
				for (std::size_t axis = 0; axis < 3; ++axis)
				{
					tangent[axis] = controls[index + 1][axis] - controls[index][axis];
				}
				std::array<double, 3> local;
				std::array<double, 3> componentLocal;
				std::array<double, 3> normal;
				if (!Inverse(root, tangent, local, true) || !Inverse(component, local, componentLocal, true) ||
					!Cross(reference, componentLocal, normal) || !IsFinite(normal))
				{
					return false;
				}
				ExpandIntervals(normal, cone);
			}
			return true;
		}

		/** Expands each frame-cone interval to include one transformed normal control.
		 * @param value One transformed cross-product control.
		 * @param intervals Per-axis low/high intervals expanded in place.
		 */
		static void ExpandIntervals(
			const std::array<double, 3>& value,
			std::array<std::array<double, 2>, 3>& intervals) noexcept
		{
			for (std::size_t axis = 0; axis < 3; ++axis)
			{
				intervals[axis][0] = (std::min)(intervals[axis][0], value[axis]);
				intervals[axis][1] = (std::max)(intervals[axis][1], value[axis]);
			}
		}

		/** Rejects frame cones whose component intervals cannot exclude zero or an up-parallel tangent.
		 * @param cone Low/high ranges for the three frame-normal components.
		 * @return True when the lower norm bound proves nondegeneracy.
		 */
		static bool ExcludesDegenerateFrame(
			const std::array<std::array<double, 2>, 3>& cone) noexcept
		{
			const double lower = std::hypot(
				MinimumMagnitude(cone[0]), MinimumMagnitude(cone[1]), MinimumMagnitude(cone[2]));
			return std::isfinite(lower) && lower > 0.0 && std::isfinite(1.0 / lower);
		}

		/** Extracts the requested closed subcurve with two De Casteljau splits.
		 * @param controls Source cubic Bezier controls.
		 * @param firstParameter Inclusive normalized start parameter.
		 * @param lastParameter Inclusive normalized end parameter greater than the start.
		 * @return Four controls describing only the requested source interval.
		 */
		static std::array<std::array<double, 3>, 4> Subcurve(
			const std::array<std::array<double, 3>, 4>& controls,
			double firstParameter,
			double lastParameter) noexcept
		{
			std::array<std::array<double, 3>, 4> left;
			std::array<std::array<double, 3>, 4> right;
			Split(controls, lastParameter, left, right);
			if (firstParameter == 0.0)
			{
				return left;
			}
			std::array<std::array<double, 3>, 4> result;
			Split(left, firstParameter / lastParameter, right, result);
			return result;
		}

		/** Splits cubic Bezier controls at one parameter while preserving the source control order.
		 * @param controls Original cubic controls.
		 * @param parameter Normalized split parameter.
		 * @param left Receives the [0, parameter] controls.
		 * @param right Receives the [parameter, 1] controls.
		 */
		static void Split(
			const std::array<std::array<double, 3>, 4>& controls,
			double parameter,
			std::array<std::array<double, 3>, 4>& left,
			std::array<std::array<double, 3>, 4>& right) noexcept
		{
			const auto first = Mix(controls[0], controls[1], parameter);
			const auto second = Mix(controls[1], controls[2], parameter);
			const auto third = Mix(controls[2], controls[3], parameter);
			const auto leftMiddle = Mix(first, second, parameter);
			const auto rightMiddle = Mix(second, third, parameter);
			const auto middle = Mix(leftMiddle, rightMiddle, parameter);
			left = {controls[0], first, leftMiddle, middle};
			right = {middle, rightMiddle, third, controls[3]};
		}

		/** Interpolates two XYZ points without allocating temporary storage.
		 * @param first First point.
		 * @param second Second point.
		 * @param parameter Interpolation amount.
		 * @return Interpolated XYZ value.
		 */
		static std::array<double, 3> Mix(
			const std::array<double, 3>& first,
			const std::array<double, 3>& second,
			double parameter) noexcept
		{
			std::array<double, 3> result{};
			for (std::size_t axis = 0; axis < 3; ++axis)
			{
				result[axis] = first[axis] * (1.0 - parameter) + second[axis] * parameter;
			}
			return result;
		}

		/** Returns the minimum absolute value in a closed interval, or zero when it straddles zero.
		 * @param interval Ordered low/high values.
		 * @return Minimum magnitude represented by the interval.
		 */
		static double MinimumMagnitude(const std::array<double, 2>& interval) noexcept
		{
			if (interval[0] > 0.0)
			{
				return interval[0];
			}
			return interval[1] < 0.0 ? -interval[1] : 0.0;
		}

		/** Rotates and scales a vector without adding transform translation.
		 * @param transform Numerical transform supplying signed scale and rotation.
		 * @param value Input vector.
		 * @param result Receives the transformed vector.
		 * @return True when the input and transformed vector are finite.
		 */
		static bool TransformVector(
			const PreparedTransform& transform,
			const std::array<double, 3>& value,
			std::array<double, 3>& result) noexcept
		{
			return Transform(transform, value, true, result);
		}

		/** Applies signed scale, rotation and translation to a local point.
		 * @param transform Numerical transform supplying translation, signed scale and rotation.
		 * @param value Input local point.
		 * @param result Receives the transformed point.
		 * @return True when the input and transformed point are finite.
		 */
		static bool TransformPoint(
			const PreparedTransform& transform,
			const std::array<double, 3>& value,
			std::array<double, 3>& result) noexcept
		{
			return Transform(transform, value, false, result);
		}

		/** Applies the existing quaternion vector transform with optional translation.
		 * @param transform Numerical translation, signed scale and unit quaternion.
		 * @param value Input point or vector.
		 * @param vector True to omit translation; false to apply it.
		 * @param result Receives the transformed value.
		 * @return True when transform and result values are valid.
		 */
		static bool Transform(
			const PreparedTransform& transform,
			const std::array<double, 3>& value,
			bool vector,
			std::array<double, 3>& result) noexcept
		{
			if (!IsFinite(value))
			{
				return false;
			}
			std::array<double, 3> scaled{};
			for (std::size_t axis = 0; axis < 3; ++axis)
			{
				scaled[axis] = value[axis] * transform.Scale[axis];
			}
			std::array<double, 3> rotated;
			Rotate(transform.Rotation, scaled, rotated);
			for (std::size_t axis = 0; axis < 3; ++axis)
			{
				result[axis] = rotated[axis] + (vector ? 0.0 : transform.Translation[axis]);
			}
			return IsFinite(result);
		}

		/** Applies inverse translation, inverse quaternion rotation and inverse signed scale.
		 * @param transform Original forward transform.
		 * @param value Input point or vector in the transformed frame.
		 * @param result Receives the inverse-transformed value.
		 * @param vector True to omit translation removal; false to remove it.
		 * @return True when the transform is invertible and the result is finite.
		 */
		static bool Inverse(
			const PreparedTransform& transform,
			const std::array<double, 3>& value,
			std::array<double, 3>& result,
			bool vector) noexcept
		{
			if (!transform.CanInvertScale || !IsFinite(value))
			{
				return false;
			}
			return ApplyInverseRotationAndScale(transform, value, vector, result);
		}

		/** Applies inverse rotation and signed scale after optional point translation removal.
		 * @param transform Original forward transform.
		 * @param value Input point or vector.
		 * @param vector True when value is a vector and has no translation.
		 * @param result Receives the inverse-transformed value.
		 * @return True when the inverse result is finite.
		 */
		static bool ApplyInverseRotationAndScale(
			const PreparedTransform& transform,
			const std::array<double, 3>& value,
			bool vector,
			std::array<double, 3>& result) noexcept
		{
			std::array<double, 3> delta = value;
			if (!vector)
			{
				for (std::size_t axis = 0; axis < 3; ++axis)
				{
					delta[axis] -= transform.Translation[axis];
				}
			}
			std::array<double, 3> rotated;
			if (!IsFinite(delta))
			{
				return false;
			}
			Rotate(transform.InverseRotation, delta, rotated);
			for (std::size_t axis = 0; axis < 3; ++axis)
			{
				result[axis] = rotated[axis] / transform.Scale[axis];
			}
			return IsFinite(result);
		}

		/** Applies a prevalidated unit quaternion without transform validation or translation.
		 * @param rotation Prepared unit quaternion or its conjugate.
		 * @param value Finite XYZ vector to rotate.
		 * @param result Receives the quaternion-rotated vector.
		 */
		static void Rotate(
			const std::array<double, 4>& rotation,
			const std::array<double, 3>& value,
			std::array<double, 3>& result) noexcept
		{
			const std::array<double, 3> firstCross{
				rotation[1] * value[2] - rotation[2] * value[1],
				rotation[2] * value[0] - rotation[0] * value[2],
				rotation[0] * value[1] - rotation[1] * value[0]};
			const std::array<double, 3> secondCross{
				rotation[1] * firstCross[2] - rotation[2] * firstCross[1],
				rotation[2] * firstCross[0] - rotation[0] * firstCross[2],
				rotation[0] * firstCross[1] - rotation[1] * firstCross[0]};
			for (std::size_t axis = 0; axis < 3; ++axis)
			{
				result[axis] = value[axis] + 2.0 * (rotation[3] * firstCross[axis] + secondCross[axis]);
			}
		}

		/** Computes the ordered scaled local interval for one bounds axis.
		 * @param bounds Local origin and half extents.
		 * @param axis Bounds coordinate to project.
		 * @param scale Signed scale for the selected coordinate.
		 * @return Ordered low/high interval after signed scale.
		 */
		static std::array<double, 2> Dimension(
			const SplineSurfaceBounds& bounds,
			std::size_t axis,
			double scale) noexcept
		{
			const double first = (bounds.Origin[axis] - bounds.Extent[axis]) * scale;
			const double second = (bounds.Origin[axis] + bounds.Extent[axis]) * scale;
			return {(std::min)(first, second), (std::max)(first, second)};
		}

		/** Checks that an interval is finite and ordered.
		 * @param interval Low/high interval to check.
		 * @return True when both endpoints are finite and low does not exceed high.
		 */
		static bool IsValidInterval(const std::array<double, 2>& interval) noexcept
		{
			return std::isfinite(interval[0]) && std::isfinite(interval[1]) && interval[0] <= interval[1];
		}

		/** Normalizes a finite nonzero XYZ vector in place.
		 * @param value Vector to normalize.
		 * @return True when its length is finite and nonzero.
		 */
		static bool Normalize(std::array<double, 3>& value) noexcept
		{
			const double length = std::hypot(value[0], value[1], value[2]);
			if (!std::isfinite(length) || length == 0.0)
			{
				return false;
			}
			for (double& component : value)
			{
				component /= length;
			}
			return IsFinite(value);
		}

		/** Calculates the three-dimensional cross product of two XYZ vectors.
		 * @param first First vector.
		 * @param second Second vector.
		 * @param result Receives the cross product.
		 * @return True when the resulting coordinates are finite.
		 */
		static bool Cross(
			const std::array<double, 3>& first,
			const std::array<double, 3>& second,
			std::array<double, 3>& result) noexcept
		{
			result = {
				first[1] * second[2] - first[2] * second[1],
				first[2] * second[0] - first[0] * second[2],
				first[0] * second[1] - first[1] * second[0]};
			return IsFinite(result);
		}
	};
}
