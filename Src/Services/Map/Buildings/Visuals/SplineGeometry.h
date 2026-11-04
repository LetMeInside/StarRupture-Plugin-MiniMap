// Provides game-independent spline evaluation, sampling, and prepared curve data.
// The caller owns source segments and reusable prepared-output storage.
#pragma once

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
	/** Identifies the interpolation behavior required by numerical curve evaluation. */
	enum class SplineInterpolation : std::uint8_t
	{
		Linear,
		Constant,
		Cubic
	};

	/** Reports numerical input and preparation failures without game-specific source status. */
	enum class SplineNumericalIssue : std::uint8_t
	{
		None,
		InvalidCurve,
		UnsupportedInterpolation,
		NonFinite
	};

	/** Holds four XYZ Bezier controls, their source-key interval, and interpolation semantics. */
	struct SplineCurveSegment
	{
		std::array<std::array<double, 3>, 4> Controls{};
		double StartKey = 0.0;
		double EndKey = 1.0;
		SplineInterpolation Interpolation = SplineInterpolation::Linear;
	};

	/** Supplies pixel and height error tolerances plus bounded subdivision resources. */
	struct SplineSamplingPolicy
	{
		double PixelsPerUnit = 1.0;
		double XYErrorPixels = 0.75;
		double HeightErrorUnits = 1.0;
		unsigned Depth = 8;
		std::size_t SampleLimit = (std::numeric_limits<std::size_t>::max)();
	};

	/** Stores sampled XYZ position, source parameter, and cumulative distance within its continuous run. */
	struct SplineSample
	{
		std::array<double, 3> Position{};
		double Parameter = 0.0;
		double CumulativeDistance = 0.0;
		bool operator==(const SplineSample&) const = default;
	};

	/** Identifies a contiguous sample range and continuity behavior for one source segment. */
	struct SplineCurveSection
	{
		std::size_t FirstSample = 0;
		std::size_t SampleCount = 0;
		bool Discontinuous = false;
		bool StartsNewRun = true;
		bool operator==(const SplineCurveSection&) const = default;
	};

	/** Owns reusable sampled output and reports whether requested sampling tolerances were met. */
	struct PreparedSpline
	{
		std::vector<SplineSample> Samples;
		std::vector<SplineCurveSection> Sections;
		SplineNumericalIssue Issue = SplineNumericalIssue::None;
		std::size_t LimitedLeaves = 0;

		/** Reports complete sampling only when every source section exists and no leaf hit its limit. */
		bool IsComplete() const noexcept
		{
			return !Sections.empty() && Issue == SplineNumericalIssue::None && LimitedLeaves == 0;
		}
	};

	/** Holds evaluated XYZ position and the derivative with respect to the source key. */
	struct SplineEvaluation
	{
		std::array<double, 3> Position{};
		std::array<double, 3> Tangent{};
		bool HasTangent = false;
	};

	/** Performs game-independent numerical evaluation and adaptive preparation of spline curves. */
	class SplineGeometry final
	{
	public:
		/**
		 * Evaluates one source segment and leaves output unchanged on invalid input.
		 * Constant segments have no tangent; other segment tangents are derivatives per source key.
		 *
		 * @param segment Numerical controls, interpolation semantics, and source-key interval.
		 * @param parameter Normalized segment parameter in the inclusive range [0, 1].
		 * @param output Receives the evaluated position and tangent on success.
		 * @return True when the segment and evaluated numerical values are valid.
		 */
		static bool Evaluate(
			const SplineCurveSegment& segment,
			double parameter,
			SplineEvaluation& output) noexcept
		{
			if (!std::isfinite(parameter) || parameter < 0.0 || parameter > 1.0 ||
				ValidateSegment(segment) != SplineNumericalIssue::None)
			{
				return false;
			}

			SplineEvaluation result;
			const double keySpan = segment.EndKey - segment.StartKey;
			if (segment.Interpolation == SplineInterpolation::Constant)
			{
				result.Position = segment.Controls[parameter == 1.0 ? 3 : 0];
			}
			else if (segment.Interpolation == SplineInterpolation::Linear)
			{
				result.Position = Mix(segment.Controls[0], segment.Controls[3], parameter);
				for (std::size_t axis = 0; axis < 3; ++axis)
				{
					result.Tangent[axis] =
						(segment.Controls[3][axis] - segment.Controls[0][axis]) / keySpan;
				}
				result.HasTangent = true;
			}
			else
			{
				EvaluateCubic(segment, parameter, keySpan, result);
			}

			PreserveExactEndpoint(segment, parameter, result.Position);
			if (!IsFinitePoint(result.Position) || !IsFinitePoint(result.Tangent))
			{
				return false;
			}
			output = result;
			return true;
		}

		/**
		 * Samples a non-owning segment span into caller-owned output, retaining its vector capacities.
		 * The source span must remain valid for this synchronous call and is never retained.
		 *
		 * @param segments Ordered caller-owned numerical source segments to validate and sample.
		 * @param policy Pixel-space and height tolerances with depth and sample limits.
		 * @param output Receives prepared samples and section metadata, reusing existing capacity.
		 */
		static void Prepare(
			std::span<const SplineCurveSegment> segments,
			SplineSamplingPolicy policy,
			PreparedSpline& output)
		{
			ResetPreparedOutput(output);
			if (segments.empty())
			{
				output.Issue = SplineNumericalIssue::InvalidCurve;
				return;
			}

			policy.Depth = (std::min)(policy.Depth, MaximumDepth);
			if (!IsValidPolicy(policy) || segments.size() > policy.SampleLimit / 2)
			{
				output.Issue = SplineNumericalIssue::InvalidCurve;
				return;
			}

			if (!ValidateSegments(segments, output))
			{
				return;
			}

			PrepareStorage(segments, policy, output);
			PrepareSegments(segments, policy, output);
			CalculateCumulativeDistances(output);
		}

	private:
		static constexpr unsigned MaximumDepth = 8;
		static constexpr std::size_t MaximumLeavesPerSegment = 257;

		/** Tests whether all three coordinates of a point are finite. */
		static bool IsFinitePoint(const std::array<double, 3>& point) noexcept
		{
			return std::isfinite(point[0]) && std::isfinite(point[1]) && std::isfinite(point[2]);
		}

		/**
		 * Linearly interpolates two XYZ points without allocating intermediate storage.
		 *
		 * @param first First XYZ endpoint.
		 * @param second Second XYZ endpoint.
		 * @param parameter Interpolation amount, normally in [0, 1].
		 * @return The interpolated XYZ point.
		 */
		static std::array<double, 3> Mix(
			const std::array<double, 3>& first,
			const std::array<double, 3>& second,
			double parameter) noexcept
		{
			std::array<double, 3> result{};
			for (std::size_t axis = 0; axis < result.size(); ++axis)
			{
				result[axis] = first[axis] * (1.0 - parameter) + second[axis] * parameter;
			}
			return result;
		}

		/**
		 * Validates interpolation semantics, a positive key span, and finite controls.
		 *
		 * @param segment Segment data to validate.
		 * @return The numerical issue found, or None when the segment is valid.
		 */
		static SplineNumericalIssue ValidateSegment(const SplineCurveSegment& segment) noexcept
		{
			if (segment.Interpolation != SplineInterpolation::Linear &&
				segment.Interpolation != SplineInterpolation::Constant &&
				segment.Interpolation != SplineInterpolation::Cubic)
			{
				return SplineNumericalIssue::UnsupportedInterpolation;
			}
			if (!std::isfinite(segment.StartKey) || !std::isfinite(segment.EndKey))
			{
				return SplineNumericalIssue::NonFinite;
			}
			const double keySpan = segment.EndKey - segment.StartKey;
			if (!std::isfinite(keySpan) || keySpan <= 0.0)
			{
				return SplineNumericalIssue::InvalidCurve;
			}
			for (const auto& control : segment.Controls)
			{
				if (!IsFinitePoint(control))
				{
					return SplineNumericalIssue::NonFinite;
				}
			}
			return SplineNumericalIssue::None;
		}

		/**
		 * Evaluates cubic Bezier position and derivative using De Casteljau interpolation.
		 *
		 * @param segment Cubic controls to evaluate.
		 * @param parameter Normalized segment parameter.
		 * @param keySpan Source-key interval length used to scale the derivative.
		 * @param result Receives position, tangent, and tangent availability.
		 */
		static void EvaluateCubic(
			const SplineCurveSegment& segment,
			double parameter,
			double keySpan,
			SplineEvaluation& result) noexcept
		{
			const auto first = Mix(segment.Controls[0], segment.Controls[1], parameter);
			const auto second = Mix(segment.Controls[1], segment.Controls[2], parameter);
			const auto third = Mix(segment.Controls[2], segment.Controls[3], parameter);
			const auto firstPair = Mix(first, second, parameter);
			const auto secondPair = Mix(second, third, parameter);
			result.Position = Mix(firstPair, secondPair, parameter);
			for (std::size_t axis = 0; axis < 3; ++axis)
			{
				result.Tangent[axis] =
					3.0 * (secondPair[axis] - firstPair[axis]) / keySpan;
			}
			result.HasTangent = true;
		}

		/**
		 * Restores exact source control endpoints, including their signed-zero representation.
		 *
		 * @param segment Source segment containing the exact endpoints.
		 * @param parameter Normalized parameter tested for an endpoint.
		 * @param position Position to replace when parameter is exactly zero or one.
		 */
		static void PreserveExactEndpoint(
			const SplineCurveSegment& segment,
			double parameter,
			std::array<double, 3>& position) noexcept
		{
			if (parameter == 0.0)
			{
				position = segment.Controls[0];
			}
			if (parameter == 1.0)
			{
				position = segment.Controls[3];
			}
		}

		/**
		 * Clears prior results and status while retaining vector capacities for reuse.
		 *
		 * @param output Prepared storage to reset for another curve publication.
		 */
		static void ResetPreparedOutput(PreparedSpline& output) noexcept
		{
			output.Samples.clear();
			output.Sections.clear();
			output.Issue = SplineNumericalIssue::None;
			output.LimitedLeaves = 0;
		}

		/**
		 * Checks finite positive tolerances before curve preparation starts.
		 *
		 * @param policy Sampling tolerances and resource limits to validate.
		 * @return True when all numerical tolerances are finite and positive.
		 */
		static bool IsValidPolicy(const SplineSamplingPolicy& policy) noexcept
		{
			return std::isfinite(policy.PixelsPerUnit) && policy.PixelsPerUnit > 0.0 &&
				std::isfinite(policy.XYErrorPixels) && policy.XYErrorPixels > 0.0 &&
				std::isfinite(policy.HeightErrorUnits) && policy.HeightErrorUnits > 0.0;
		}

		/**
		 * Validates every segment before output samples are appended.
		 *
		 * @param segments Source segments to validate in order.
		 * @param output Receives the first numerical validation issue, if any.
		 * @return True only when all source segments pass numerical validation.
		 */
		static bool ValidateSegments(
			std::span<const SplineCurveSegment> segments,
			PreparedSpline& output) noexcept
		{
			for (const auto& segment : segments)
			{
				const auto issue = ValidateSegment(segment);
				if (issue != SplineNumericalIssue::None)
				{
					output.Issue = issue;
					return false;
				}
			}
			return true;
		}

		/**
		 * Reserves modest initial output capacity and preserves larger reusable capacity.
		 *
		 * @param segments Source segment count used to reserve section metadata.
		 * @param policy Sample limit used to bound the initial sample reservation.
		 * @param output Caller-owned prepared vectors to reserve.
		 */
		static void PrepareStorage(
			std::span<const SplineCurveSegment> segments,
			const SplineSamplingPolicy& policy,
			PreparedSpline& output)
		{
			output.Samples.reserve((std::min)(std::size_t(64), policy.SampleLimit));
			output.Sections.reserve(segments.size());
		}

		/**
		 * Samples segments in source order and records continuity metadata for each section.
		 *
		 * @param segments Validated source segments to sample.
		 * @param policy Sampling tolerances and resource limits.
		 * @param output Receives samples and section ranges.
		 */
		static void PrepareSegments(
			std::span<const SplineCurveSegment> segments,
			const SplineSamplingPolicy& policy,
			PreparedSpline& output)
		{
			for (std::size_t index = 0; index < segments.size(); ++index)
			{
				PrepareSegment(segments, index, policy, output);
			}
		}

		/** Assigns sampled XYZ centerline distance, restarting at each disconnected run. */
		static void CalculateCumulativeDistances(PreparedSpline& output) noexcept
		{
			double distance = 0.0;
			for (const auto& section : output.Sections)
			{
				if (section.StartsNewRun)
				{
					distance = 0.0;
				}
				for (std::size_t offset = 0; offset < section.SampleCount; ++offset)
				{
					auto& sample = output.Samples[section.FirstSample + offset];
					if (offset != 0)
					{
						const auto& previous = output.Samples[section.FirstSample + offset - 1].Position;
						distance += std::hypot(sample.Position[0] - previous[0], sample.Position[1] - previous[1],
							sample.Position[2] - previous[2]);
						if (!std::isfinite(distance))
						{
							output.Issue = SplineNumericalIssue::NonFinite;
							return;
						}
					}
					sample.CumulativeDistance = distance;
				}
			}
		}

		/**
		 * Samples one segment with a fair share of the remaining sample budget.
		 *
		 * @param segments Validated source segments.
		 * @param segmentIndex Index of the source segment being prepared.
		 * @param policy Sampling tolerances and resource limits.
		 * @param output Receives this segment's samples and section metadata.
		 */
		static void PrepareSegment(
			std::span<const SplineCurveSegment> segments,
			std::size_t segmentIndex,
			const SplineSamplingPolicy& policy,
			PreparedSpline& output)
		{
			const auto& segment = segments[segmentIndex];
			const std::size_t remainingSegments = segments.size() - segmentIndex;
			const std::size_t remainingSamples = policy.SampleLimit - output.Samples.size();
			const std::size_t segmentBudget = (std::min)(
				MaximumLeavesPerSegment, remainingSamples / remainingSegments);
			SplineCurveSection section;
			section.FirstSample = output.Samples.size();
			section.Discontinuous = segment.Interpolation == SplineInterpolation::Constant;
			section.StartsNewRun = StartsNewRun(segments, segmentIndex, segment);
			AppendSample(output, segment.Controls[0], 0.0);
			if (segment.Interpolation == SplineInterpolation::Linear || section.Discontinuous)
			{
				AppendSample(output, segment.Controls[3], 1.0);
			}
			else
			{
				Subdivide(segment.Controls, 0.0, 1.0, policy, 0, segmentBudget - 1, output);
			}
			section.SampleCount = output.Samples.size() - section.FirstSample;
			output.Sections.push_back(section);
		}

		/**
		 * Determines whether a segment starts a new run from its source continuity.
		 *
		 * @param segments Ordered source segments containing the previous segment when present.
		 * @param segmentIndex Index of the segment being checked.
		 * @param segment Source segment whose continuity is checked.
		 * @return True when the segment starts a new or discontinuous run.
		 */
		static bool StartsNewRun(
			std::span<const SplineCurveSegment> segments,
			std::size_t segmentIndex,
			const SplineCurveSegment& segment) noexcept
		{
			if (segmentIndex == 0 || segment.Interpolation == SplineInterpolation::Constant)
			{
				return true;
			}
			const auto& previous = segments[segmentIndex - 1];
			return previous.Interpolation == SplineInterpolation::Constant ||
				previous.EndKey != segment.StartKey || previous.Controls[3] != segment.Controls[0];
		}

		/**
		 * Appends one sample to caller-owned storage without per-sample allocation.
		 *
		 * @param output Prepared sample vector receiving the value.
		 * @param position Sampled XYZ position.
		 * @param parameter Normalized parameter within the source segment.
		 */
		static void AppendSample(
			PreparedSpline& output,
			const std::array<double, 3>& position,
			double parameter)
		{
			output.Samples.push_back({position, parameter});
		}

		/**
		 * Tests cubic flatness in XY pixel error and Z chord error.
		 *
		 * @param controls Cubic Bezier controls for the current subdivision.
		 * @param policy XY and height error thresholds.
		 * @return True when both interior controls satisfy the requested tolerances.
		 */
		static bool IsFlat(
			const std::array<std::array<double, 3>, 4>& controls,
			const SplineSamplingPolicy& policy) noexcept
		{
			const double dx = controls[3][0] - controls[0][0];
			const double dy = controls[3][1] - controls[0][1];
			const double length = std::hypot(dx, dy);
			for (std::size_t index = 1; index <= 2; ++index)
			{
				const double distance = ControlDistance(controls, index, dx, dy, length);
				const double fraction = double(index) / 3.0;
				const double expectedHeight = controls[0][2] * (1.0 - fraction) + controls[3][2] * fraction;
				const double heightError = std::abs(controls[index][2] - expectedHeight);
				if (!std::isfinite(distance) || !std::isfinite(heightError) ||
					distance * policy.PixelsPerUnit > policy.XYErrorPixels ||
					heightError > policy.HeightErrorUnits)
				{
					return false;
				}
			}
			return true;
		}

		/**
		 * Measures one Bezier control point's XY distance from the endpoint chord.
		 *
		 * @param controls Cubic Bezier controls.
		 * @param controlIndex Index of the interior control point being measured.
		 * @param dx Chord X displacement.
		 * @param dy Chord Y displacement.
		 * @param chordLength XY chord length.
		 * @return Perpendicular distance to the nearest point on the endpoint chord.
		 */
		static double ControlDistance(
			const std::array<std::array<double, 3>, 4>& controls,
			std::size_t controlIndex,
			double dx,
			double dy,
			double chordLength) noexcept
		{
			double fraction = 0.0;
			if (chordLength > 0.0)
			{
				fraction = std::clamp(
					((controls[controlIndex][0] - controls[0][0]) * (dx / chordLength) +
						(controls[controlIndex][1] - controls[0][1]) * (dy / chordLength)) / chordLength,
					0.0,
					1.0);
			}
			const double chordX = controls[0][0] + fraction * dx;
			const double chordY = controls[0][1] + fraction * dy;
			return std::hypot(controls[controlIndex][0] - chordX, controls[controlIndex][1] - chordY);
		}

		/**
		 * Recursively subdivides cubic controls until flatness or resource limits are reached.
		 *
		 * @param controls Cubic Bezier controls for this subdivision.
		 * @param firstParameter Normalized parameter at the first endpoint.
		 * @param lastParameter Normalized parameter at the final endpoint.
		 * @param policy Flatness and subdivision limits.
		 * @param depth Current recursion depth.
		 * @param leafBudget Maximum child leaves allowed for this subcurve.
		 * @param output Receives accepted endpoint samples and limited-leaf counts.
		 */
		static void Subdivide(
			const std::array<std::array<double, 3>, 4>& controls,
			double firstParameter,
			double lastParameter,
			const SplineSamplingPolicy& policy,
			unsigned depth,
			std::size_t leafBudget,
			PreparedSpline& output)
		{
			const bool flat = IsFlat(controls, policy);
			if (flat || depth == policy.Depth || leafBudget == 1)
			{
				if (!flat)
				{
					++output.LimitedLeaves;
				}
				AppendSample(output, controls[3], lastParameter);
				return;
			}

			const auto firstMidpoint = Mix(controls[0], controls[1], 0.5);
			const auto secondMidpoint = Mix(controls[1], controls[2], 0.5);
			const auto thirdMidpoint = Mix(controls[2], controls[3], 0.5);
			const auto leftMidpoint = Mix(firstMidpoint, secondMidpoint, 0.5);
			const auto rightMidpoint = Mix(secondMidpoint, thirdMidpoint, 0.5);
			const auto center = Mix(leftMidpoint, rightMidpoint, 0.5);
			const double middleParameter = (firstParameter + lastParameter) * 0.5;
			const std::size_t leftLeafBudget = leafBudget / 2;
			Subdivide({controls[0], firstMidpoint, leftMidpoint, center}, firstParameter,
				middleParameter, policy, depth + 1, leftLeafBudget, output);
			Subdivide({center, rightMidpoint, thirdMidpoint, controls[3]}, middleParameter,
				lastParameter, policy, depth + 1, leafBudget - leftLeafBudget, output);
		}
	};
}
