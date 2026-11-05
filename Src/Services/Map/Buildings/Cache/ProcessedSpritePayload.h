// Owns one processed sprite image and the original capture geometry needed to project it.
#pragma once

#include "../Visuals/SpriteGeometry.h"

#include <array>
#include <cstdint>
#include <cmath>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace Services::Map::Buildings::Cache
{
	/** Exclusively owns processed RGBA8 pixels and preserves their original capture mapping.
	 * Construction adopts a pixel vector without copying it. Consumers receive const pixel
	 * access; original capture size is distinct from processed image size.
	 */
	class ProcessedSpritePayload final
	{
	public:
		/** Identifies the stored pixel encoding expected by independent consumers. */
		enum class PixelFormat : std::uint8_t
		{
			StraightRgba8SrgbDisplay
		};
		/** Prevents copies from duplicating processed image ownership. */
		ProcessedSpritePayload(const ProcessedSpritePayload&) = delete;
		/** Prevents copies from duplicating processed image ownership. */
		ProcessedSpritePayload& operator=(const ProcessedSpritePayload&) = delete;
		/** Transfers the exclusively owned pixel vector and geometry. */
		ProcessedSpritePayload(ProcessedSpritePayload&&) noexcept = default;
		/** Transfers the exclusively owned pixel vector and geometry. */
		ProcessedSpritePayload& operator=(ProcessedSpritePayload&&) noexcept = default;
		/** Validates and adopts pixels plus source geometry without a second pixel copy.
		 * @param pixels Processed straight RGBA8 bytes; ownership transfers on success.
		 * @param width Processed image width in pixels.
		 * @param height Processed image height in pixels.
		 * @param captureSize Original square capture dimensions.
		 * @param bounds Original capture-space XYZ bounds.
		 * @param projection Processed image edges mapped into original capture pixels.
		 * @param output Receives the exclusively owned immutable payload on success.
		 * @return True when dimensions, byte count, bounds and projection are valid.
		 */
		static bool TryAdopt(
			std::vector<std::uint8_t>&& pixels,
			std::uint32_t width,
			std::uint32_t height,
			Visuals::SpriteSourceSize captureSize,
			Visuals::SpriteCaptureBounds bounds,
			Visuals::SpriteProjectionRectangle projection,
			std::unique_ptr<const ProcessedSpritePayload>& output)
		{
			if (!HasValidSpriteData(width, height, captureSize, bounds, projection, pixels.size()))
			{
				return false;
			}

			output.reset(new ProcessedSpritePayload(
				std::move(pixels), width, height, captureSize, bounds, projection));
			return true;
		}

		/** Returns borrowed immutable processed RGBA8 bytes, valid for this payload's lifetime. */
		const std::vector<std::uint8_t>& Pixels() const noexcept { return m_pixels; }
		/** Returns the processed image width. */
		std::uint32_t Width() const noexcept { return m_width; }
		/** Returns the processed image height. */
		std::uint32_t Height() const noexcept { return m_height; }
		/** Returns the explicit stored source-format metadata. */
		PixelFormat Format() const noexcept { return PixelFormat::StraightRgba8SrgbDisplay; }
		/** Returns the original capture dimensions, not the processed image dimensions. */
		Visuals::SpriteSourceSize CaptureSize() const noexcept { return m_captureSize; }
		/** Returns original capture-space XYZ bounds. */
		const Visuals::SpriteCaptureBounds& Bounds() const noexcept { return m_bounds; }
		/** Returns processed image edges mapped into original capture-pixel coordinates. */
		const Visuals::SpriteProjectionRectangle& Projection() const noexcept { return m_projection; }

	private:
		/** Stores already validated immutable image and capture metadata. */
		ProcessedSpritePayload(
			std::vector<std::uint8_t>&& pixels,
			std::uint32_t width,
			std::uint32_t height,
			Visuals::SpriteSourceSize captureSize,
			Visuals::SpriteCaptureBounds bounds,
			Visuals::SpriteProjectionRectangle projection) noexcept
			: m_pixels(std::move(pixels)), m_width(width), m_height(height),
			  m_captureSize(captureSize), m_bounds(bounds), m_projection(projection)
		{
		}

		/** Checks allocation-safe RGBA8 size and geometry limits used by the sprite cache.
		 * @param width Processed image width.
		 * @param height Processed image height.
		 * @param captureSize Original capture dimensions.
		 * @param bounds Original capture-space bounds.
		 * @param projection Processed image edges mapped into original capture pixels.
		 * @param pixelBytes Number of owned RGBA8 bytes.
		 * @return True when byte count and all geometry are valid.
		 */
		static bool HasValidSpriteData(
			std::uint32_t width,
			std::uint32_t height,
			Visuals::SpriteSourceSize captureSize,
			const Visuals::SpriteCaptureBounds& bounds,
			const Visuals::SpriteProjectionRectangle& projection,
			std::size_t pixelBytes) noexcept
		{
			if (!width || !height || width > 256 || height > 256 ||
				captureSize.Width != 256 || captureSize.Height != 256 ||
				std::size_t(width) * height > 64u * 1024u * 1024u / 4 ||
				pixelBytes != std::size_t(width) * height * 4)
			{
				return false;
			}

			for (std::size_t axis = 0; axis < 3; ++axis)
			{
				if (!std::isfinite(bounds.Minimum[axis]) || !std::isfinite(bounds.Maximum[axis]) ||
					bounds.Maximum[axis] < bounds.Minimum[axis])
				{
					return false;
				}
			}

			return std::isfinite(projection.Left) && std::isfinite(projection.Top) &&
				std::isfinite(projection.Right) && std::isfinite(projection.Bottom) &&
				projection.Left >= 0 && projection.Top >= 0 && projection.Left < 256 && projection.Top < 256 &&
				projection.Right > projection.Left && projection.Bottom > projection.Top;
		}

		std::vector<std::uint8_t> m_pixels;
		std::uint32_t m_width;
		std::uint32_t m_height;
		Visuals::SpriteSourceSize m_captureSize;
		Visuals::SpriteCaptureBounds m_bounds;
		Visuals::SpriteProjectionRectangle m_projection;
	};
}
