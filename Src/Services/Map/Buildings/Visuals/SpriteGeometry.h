// Provides game-independent world-space quad construction from sprite source geometry.
// Source dimensions describe the image used for projection, not an atlas or destination.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Services::Map::Buildings::Visuals
{
	/** Holds source-image dimensions used to validate projected starts and map pixels to world space. */
	struct SpriteSourceSize
	{
		std::uint32_t Width = 0;
		std::uint32_t Height = 0;
	};

	/** Holds the 3D position, signed scale, and quaternion rotation of one sprite placement. */
	struct SpritePlacement
	{
		std::array<double, 3> Position{};
		std::array<double, 3> SignedScale{};
		std::array<double, 4> Rotation{};
	};

	/** Holds capture-space bounds used to map sprite pixels into world space. */
	struct SpriteCaptureBounds
	{
		std::array<double, 3> Minimum{};
		std::array<double, 3> Maximum{};
	};

	/** Holds half-open output-image edges mapped into original capture-pixel coordinates. */
	struct SpriteProjectionRectangle
	{
		double Left = 0.0;
		double Top = 0.0;
		double Right = 0.0;
		double Bottom = 0.0;
	};

	/** Stores the four world-space sprite corners in top-left, top-right, bottom-right, bottom-left order. */
	struct SpriteWorldQuad
	{
		std::array<std::array<double, 3>, 4> Corners{};
	};

	/** Calculates world-space corners for a projected sprite image. */
	class SpriteGeometry final
	{
	public:
		/**
		 * Maps projected image edges through capture bounds, signed scale, and quaternion rotation.
		 * Leaves output unchanged and returns false when inputs are invalid or results are non-finite.
		 *
		 * @param placement Position, signed scale, and XYZW quaternion rotation.
		 * @param sourceSize Original source-image width and height in pixels; neither may be zero.
		 * @param captureBounds Minimum and maximum capture-space XYZ bounds.
		 * @param projection Half-open projected output-image edges in original capture-pixel coordinates.
		 * Right and bottom may exceed sourceSize to retain ceil-rounded output texel footprints.
		 * @param output Receives the four world-space corners after successful calculation.
		 * @return True when inputs pass validation and all calculated corners are finite.
		 */
		static bool BuildWorldQuad(
			const SpritePlacement& placement,
			const SpriteSourceSize& sourceSize,
			const SpriteCaptureBounds& captureBounds,
			const SpriteProjectionRectangle& projection,
			SpriteWorldQuad& output) noexcept
		{
			std::array<double, 4> normalizedRotation{};
			if (!TryNormalizeRotation(placement.Rotation, normalizedRotation))
			{
				return false;
			}

			std::array<double, 3> captureCenter{};
			double halfWidth = 0.0;
			double halfHeight = 0.0;
			if (!TryGetCaptureProjection(placement, sourceSize, captureBounds, projection,
				captureCenter, halfWidth, halfHeight))
			{
				return false;
			}

			SpriteWorldQuad result;
			const auto projectionCorners = GetProjectionCorners(projection);
			const double sourcePixelFactorX = 2.0 / double(sourceSize.Width);
			const double sourcePixelFactorY = 2.0 / double(sourceSize.Height);
			for (std::size_t cornerIndex = 0; cornerIndex < projectionCorners.size(); ++cornerIndex)
			{
				if (!TryTransformCorner(placement, normalizedRotation, captureCenter, halfWidth,
					halfHeight, sourcePixelFactorX, sourcePixelFactorY, projectionCorners[cornerIndex],
					result.Corners[cornerIndex]))
				{
					return false;
				}
			}
			output = result;
			return true;
		}

		/** Calculates the maximum world Z of all eight transformed capture-bound corners.
		 * @param placement Source position, signed scale, and XYZW rotation.
		 * @param captureBounds Full source capture bounds, not the projected image footprint.
		 * @param topHeight Receives the conservative world-space maximum height on success.
		 * @return True when the transform and all eight transformed corners are finite and valid.
		 */
		static bool TryGetCaptureBoundsTopHeight(
			const SpritePlacement& placement,
			const SpriteCaptureBounds& captureBounds,
			double& topHeight) noexcept
		{
			std::array<double, 4> rotation{};
			if (!TryNormalizeRotation(placement.Rotation, rotation))
			{
				return false;
			}

			double maximumHeight = -INFINITY;
			for (std::size_t axis = 0; axis < 3; ++axis)
			{
				if (!std::isfinite(placement.Position[axis]) || !std::isfinite(placement.SignedScale[axis]) ||
					!std::isfinite(captureBounds.Minimum[axis]) || !std::isfinite(captureBounds.Maximum[axis]) ||
					captureBounds.Maximum[axis] < captureBounds.Minimum[axis])
				{
					return false;
				}
			}
			for (unsigned corner = 0; corner < 8; ++corner)
			{
				std::array<double, 3> local{};
				for (std::size_t axis = 0; axis < 3; ++axis)
				{
					const double bound = (corner & (1u << axis)) ? captureBounds.Maximum[axis] : captureBounds.Minimum[axis];
					local[axis] = bound * placement.SignedScale[axis];
				}
				std::array<double, 3> world{};
				if (!TransformScaledPoint(placement, rotation, local, world))
				{
					return false;
				}
				maximumHeight = (std::max)(maximumHeight, world[2]);
			}
			topHeight = maximumHeight;
			return true;
		}

	private:
		/** Applies the reference transform to one signed-scaled point.
		 * @param placement World translation and signed scale.
		 * @param rotation Normalized XYZW quaternion.
		 * @param local Signed-scaled source XYZ point.
		 * @param world Receives the transformed world point when finite.
		 * @return True when all resulting coordinates are finite.
		 */
		static bool TransformScaledPoint(
			const SpritePlacement& placement,
			const std::array<double, 4>& rotation,
			const std::array<double, 3>& local,
			std::array<double, 3>& world) noexcept
		{
			const double x = rotation[0];
			const double y = rotation[1];
			const double z = rotation[2];
			const double w = rotation[3];
			const double tx = 2.0 * (y * local[2] - z * local[1]);
			const double ty = 2.0 * (z * local[0] - x * local[2]);
			const double tz = 2.0 * (x * local[1] - y * local[0]);
			world = {{placement.Position[0] + local[0] + w * tx + y * tz - z * ty,
				placement.Position[1] + local[1] + w * ty + z * tx - x * tz,
				placement.Position[2] + local[2] + w * tz + x * ty - y * tx}};
			return std::isfinite(world[0]) && std::isfinite(world[1]) && std::isfinite(world[2]);
		}

		/**
		 * Normalizes a finite nonzero XYZW quaternion using the reference algorithm.
		 *
		 * @param rotation Source quaternion components in XYZW order.
		 * @param normalizedRotation Receives normalized components on success.
		 * @return True when the quaternion is finite and has a finite nonzero norm.
		 */
		static bool TryNormalizeRotation(
			const std::array<double, 4>& rotation,
			std::array<double, 4>& normalizedRotation) noexcept
		{
			double norm = 0.0;
			for (double component : rotation)
			{
				if (!std::isfinite(component))
				{
					return false;
				}
				norm += component * component;
			}
			if (!std::isfinite(norm) || !(norm > 0.0))
			{
				return false;
			}

			const double inverseNorm = 1.0 / std::sqrt(norm);
			for (std::size_t index = 0; index < rotation.size(); ++index)
			{
				normalizedRotation[index] = rotation[index] * inverseNorm;
			}
			return true;
		}

		/**
		 * Validates source size, placement, capture bounds, and projected starts, then computes spans.
		 *
		 * @param placement Position and signed scale used by the corner calculation.
		 * @param sourceSize Nonzero dimensions of the original source image in pixels.
		 * @param captureBounds Minimum and maximum XYZ capture-space bounds.
		 * @param projection Projected image edges; right and bottom may exceed source dimensions.
		 * @param captureCenter Receives the center of the capture bounds.
		 * @param halfWidth Receives the capture half-width after the reference float conversion.
		 * @param halfHeight Receives the vertical half-span adjusted for source aspect ratio.
		 * @return True when inputs are valid and both projection spans are finite and positive.
		 */
		static bool TryGetCaptureProjection(
			const SpritePlacement& placement,
			const SpriteSourceSize& sourceSize,
			const SpriteCaptureBounds& captureBounds,
			const SpriteProjectionRectangle& projection,
			std::array<double, 3>& captureCenter,
			double& halfWidth,
			double& halfHeight) noexcept
		{
			std::array<double, 3> extent{};
			for (std::size_t axis = 0; axis < 3; ++axis)
			{
				if (!std::isfinite(placement.Position[axis]) || !std::isfinite(placement.SignedScale[axis]) ||
					!std::isfinite(captureBounds.Minimum[axis]) || !std::isfinite(captureBounds.Maximum[axis]) ||
					captureBounds.Maximum[axis] < captureBounds.Minimum[axis])
				{
					return false;
				}
				captureCenter[axis] = captureBounds.Minimum[axis] * 0.5 + captureBounds.Maximum[axis] * 0.5;
				extent[axis] = captureBounds.Maximum[axis] * 0.5 - captureBounds.Minimum[axis] * 0.5;
			}
			if (!(extent[0] > 0.0 && extent[1] > 0.0))
			{
				return false;
			}

			halfWidth = double(float(2.0 * (std::max)(extent[0], extent[1]) * 1.1)) * 0.5;
			if (!std::isfinite(halfWidth) || !(halfWidth > 0.0))
			{
				return false;
			}
			if (!sourceSize.Width || !sourceSize.Height || !IsValidProjection(projection, sourceSize))
			{
				return false;
			}

			// OrthoWidth is the horizontal span; the vertical span follows image aspect ratio.
			halfHeight = halfWidth * (double(sourceSize.Height) / double(sourceSize.Width));
			return std::isfinite(halfHeight) && halfHeight > 0.0;
		}

		/**
		 * Checks finite projected edges, positive spans, and in-image starting positions.
		 *
		 * @param projection Half-open projected output-image rectangle in source-pixel coordinates.
		 * @param sourceSize Original source-image dimensions in pixels.
		 * @return True when starts lie inside sourceSize and both rectangle spans are positive.
		 */
		static bool IsValidProjection(
			const SpriteProjectionRectangle& projection,
			const SpriteSourceSize& sourceSize) noexcept
		{
			const std::array<double, 4> edges{
				projection.Left, projection.Top, projection.Right, projection.Bottom};
			for (double edge : edges)
			{
				if (!std::isfinite(edge))
				{
					return false;
				}
			}
			return projection.Left >= 0.0 && projection.Top >= 0.0 &&
				projection.Left < double(sourceSize.Width) && projection.Top < double(sourceSize.Height) &&
				projection.Right > projection.Left && projection.Bottom > projection.Top;
		}

		/**
		 * Returns projected corners in top-left, top-right, bottom-right, bottom-left order.
		 *
		 * @param projection Half-open projected image rectangle whose corners are returned.
		 * @return The four projected corners in sprite drawing order.
		 */
		static std::array<std::array<double, 2>, 4> GetProjectionCorners(
			const SpriteProjectionRectangle& projection) noexcept
		{
			return {{
				{projection.Left, projection.Top},
				{projection.Right, projection.Top},
				{projection.Right, projection.Bottom},
				{projection.Left, projection.Bottom}}};
		}

		/**
		 * Applies pixel projection, signed scale, and normalized quaternion rotation to one corner.
		 *
		 * @param placement Position and signed scale.
		 * @param rotation Normalized quaternion components in XYZW order.
		 * @param captureCenter Center of the sprite capture bounds.
		 * @param halfWidth Capture half-width after the reference float conversion.
		 * @param halfHeight Capture half-height after source aspect-ratio adjustment.
		 * @param sourcePixelFactorX Horizontal pixel-to-normalized projection factor.
		 * @param sourcePixelFactorY Vertical pixel-to-normalized projection factor.
		 * @param projectionCorner Projected output-image edge being mapped.
		 * @param worldCorner Receives the calculated world-space XYZ corner.
		 * @return True when intermediate and final coordinates are finite.
		 */
		static bool TryTransformCorner(
			const SpritePlacement& placement,
			const std::array<double, 4>& rotation,
			const std::array<double, 3>& captureCenter,
			double halfWidth,
			double halfHeight,
			double sourcePixelFactorX,
			double sourcePixelFactorY,
			const std::array<double, 2>& projectionCorner,
			std::array<double, 3>& worldCorner) noexcept
		{
			const double x = rotation[0];
			const double y = rotation[1];
			const double z = rotation[2];
			const double w = rotation[3];
			const std::array<double, 3> scaledCapturePoint{{
				(captureCenter[0] + halfWidth *
					(projectionCorner[0] * sourcePixelFactorX - 1.0)) * placement.SignedScale[0],
				(captureCenter[1] + halfHeight *
					(projectionCorner[1] * sourcePixelFactorY - 1.0)) * placement.SignedScale[1],
				captureCenter[2] * placement.SignedScale[2]}};
			for (double coordinate : scaledCapturePoint)
			{
				if (!std::isfinite(coordinate))
				{
					return false;
				}
			}

			const double tx = 2.0 * (y * scaledCapturePoint[2] - z * scaledCapturePoint[1]);
			const double ty = 2.0 * (z * scaledCapturePoint[0] - x * scaledCapturePoint[2]);
			const double tz = 2.0 * (x * scaledCapturePoint[1] - y * scaledCapturePoint[0]);
			worldCorner = {{
				placement.Position[0] + scaledCapturePoint[0] + w * tx + y * tz - z * ty,
				placement.Position[1] + scaledCapturePoint[1] + w * ty + z * tx - x * tz,
				placement.Position[2] + scaledCapturePoint[2] + w * tz + x * ty - y * tx}};
			for (double coordinate : worldCorner)
			{
				if (!std::isfinite(coordinate))
				{
					return false;
				}
			}
			return true;
		}
	};
}
