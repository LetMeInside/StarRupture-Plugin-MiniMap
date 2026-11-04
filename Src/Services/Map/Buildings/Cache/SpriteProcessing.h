// Combines neutral HDR capture samples and independently produces cropped RGBA8 sprites.
#pragma once

#include "ProcessedSpritePayload.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace Services::Map::Buildings::Cache
{
	/** Describes CPU-readable final display color and inverse-opacity capture samples.
	 * Both spans contain row-major RGBA16F half words for the authored 256 by 256 capture.
	 * The caller retains both spans through Process; no input storage is retained afterward.
	 */
	struct SpriteCapturePixels
	{
		std::span<const std::uint16_t> FinalDisplayColorRgba16F;
		std::span<const std::uint16_t> HdrInverseOpacityRgba16F;
	};

	/** Supplies neutral capture metadata needed to preserve the sprite's world projection. */
	struct SpriteCaptureMetadata
	{
		Visuals::SpriteCaptureBounds Bounds{};
		double OrthographicExtent = 0.0;
		double PixelsPerWorldUnit = 0.0;
	};

	/** Reports independent CPU processing outcome and, when ready, one owned immutable payload. */
	struct SpriteProcessingResult
	{
		/** Describes why independent CPU processing did or did not produce a payload. */
		enum class Status : std::uint8_t
		{
			Processed,
			EmptyCapture,
			InvalidInput,
			ProcessingFailure
		};
		Status Result = Status::InvalidInput;
		std::shared_ptr<const ProcessedSpritePayload> Payload;
	};

	/** Converts authored capture samples to straight display RGBA8 and applies compatible crop/filtering. */
	class SpriteProcessing final
	{
	public:
		/** Produces a processed sprite while retaining original 256-square capture coordinates.
		 * @param pixels Neutral final-color and inverse-opacity half-float capture spans.
		 * @param metadata Capture bounds and orthographic density inputs.
		 * @return Processed payload, empty capture, invalid input or processing failure.
		 */
		static SpriteProcessingResult Process(
			const SpriteCapturePixels& pixels,
			const SpriteCaptureMetadata& metadata) noexcept
		{
			try
			{
				if (!IsValidInput(pixels, metadata))
				{
					return {SpriteProcessingResult::Status::InvalidInput, {}};
				}

				std::vector<std::uint8_t> displayRgba;
				if (!CombineCapture(pixels, displayRgba))
				{
					return {SpriteProcessingResult::Status::InvalidInput, {}};
				}

				return CropAndFilter(std::move(displayRgba), metadata);
			}
			catch (...)
			{
				return {SpriteProcessingResult::Status::ProcessingFailure, {}};
			}
		}

	private:
		/** Rejects mismatched sample sizes, non-finite bounds, and invalid density parameters. */
		static bool IsValidInput(
			const SpriteCapturePixels& pixels,
			const SpriteCaptureMetadata& metadata) noexcept
		{
			constexpr std::size_t capturePixels = 256u * 256u;
			if (pixels.FinalDisplayColorRgba16F.size() != capturePixels * 4 ||
				pixels.HdrInverseOpacityRgba16F.size() != capturePixels * 4 ||
				!std::isfinite(metadata.OrthographicExtent) || metadata.OrthographicExtent <= 0 ||
				!std::isfinite(metadata.PixelsPerWorldUnit) || metadata.PixelsPerWorldUnit <= 0)
			{
				return false;
			}
			for (std::size_t axis = 0; axis < 3; ++axis)
			{
				if (!std::isfinite(metadata.Bounds.Minimum[axis]) || !std::isfinite(metadata.Bounds.Maximum[axis]) ||
					metadata.Bounds.Maximum[axis] < metadata.Bounds.Minimum[axis])
				{
					return false;
				}
			}
			return metadata.Bounds.Maximum[0] > metadata.Bounds.Minimum[0] &&
				metadata.Bounds.Maximum[1] > metadata.Bounds.Minimum[1];
		}

		/** Converts half samples to final display RGB and combines independently derived coverage. */
		static bool CombineCapture(
			const SpriteCapturePixels& pixels,
			std::vector<std::uint8_t>& rgba)
		{
			rgba.resize(256u * 256u * 4);
			for (std::size_t index = 0; index < 256u * 256u; ++index)
			{
				if (!ConvertPixel(pixels, index, rgba.data() + index * 4))
				{
					return false;
				}
			}
			return true;
		}

		/** Converts one half-float pixel using the established final-color/coverage semantics. */
		static bool ConvertPixel(
			const SpriteCapturePixels& pixels,
			std::size_t index,
			std::uint8_t* output) noexcept
		{
			for (std::size_t channel = 0; channel < 3; ++channel)
			{
				const float color = HalfToFloat(pixels.FinalDisplayColorRgba16F[index * 4 + channel]);
				if (!std::isfinite(color))
				{
					return false;
				}
			}
			const float inverseOpacity = HalfToFloat(pixels.HdrInverseOpacityRgba16F[index * 4 + 3]);
			if (!std::isfinite(inverseOpacity))
			{
				return false;
			}
			output[3] = ToColorByte(1.0f - std::clamp(inverseOpacity, 0.0f, 1.0f));
			if (!output[3])
			{
				output[0] = 0;
				output[1] = 0;
				output[2] = 0;
				return true;
			}
			for (std::size_t channel = 0; channel < 3; ++channel)
			{
				const float color = HalfToFloat(pixels.FinalDisplayColorRgba16F[index * 4 + channel]);
			output[channel] = ToColorByte(color);
			}
			return true;
		}

		/** Converts one IEEE binary16 value to float without engine or graphics dependencies. */
		static float HalfToFloat(std::uint16_t value) noexcept
		{
			const bool negative = (value & 0x8000) != 0;
			const unsigned exponent = (value >> 10) & 31;
			const unsigned fraction = value & 1023;
			float converted = exponent == 31 ? (fraction ? NAN : INFINITY) :
				(exponent == 0 ? std::ldexp(float(fraction), -24) :
				std::ldexp(float(1024 + fraction), int(exponent) - 25));
			return negative ? -converted : converted;
		}

		/** Clamps and rounds one display-space channel to an 8-bit value. */
		static std::uint8_t ToColorByte(float value) noexcept
		{
			return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
		}

		/** Finds occupied alpha, adds its original-pixel guard, and chooses a no-upsample ratio. */
		static SpriteProcessingResult CropAndFilter(
			std::vector<std::uint8_t>&& rgba,
			const SpriteCaptureMetadata& metadata)
		{
			std::array<std::size_t, 4> crop{};
			if (!FindOccupiedBounds(rgba, crop))
			{
				return {SpriteProcessingResult::Status::EmptyCapture, {}};
			}
			AddTransparentGuard(crop);
			const double ratio = CalculateDensityRatio(metadata);
			if (!std::isfinite(ratio) || ratio <= 0)
			{
				return {SpriteProcessingResult::Status::InvalidInput, {}};
			}
			const double step = 1.0 / ratio;
			if (ratio == 1.0 && crop[0] == 0 && crop[1] == 0 && crop[2] == 256 && crop[3] == 256)
			{
				return AdoptOutput(std::move(rgba), 256, 256, metadata.Bounds, crop, step);
			}
			return Resample(rgba, metadata.Bounds, crop, ratio, step);
		}

		/** Finds half-open bounds of all pixels with nonzero alpha. */
		static bool FindOccupiedBounds(const std::vector<std::uint8_t>& rgba, std::array<std::size_t, 4>& crop)
		{
			crop = {256, 256, 0, 0};
			for (std::size_t y = 0; y < 256; ++y)
			{
				for (std::size_t x = 0; x < 256; ++x)
				{
					if (rgba[(y * 256 + x) * 4 + 3])
					{
						crop[0] = (std::min)(crop[0], x);
						crop[1] = (std::min)(crop[1], y);
						crop[2] = (std::max)(crop[2], x + 1);
						crop[3] = (std::max)(crop[3], y + 1);
					}
				}
			}
			return crop[2] > crop[0];
		}

		/** Expands occupied bounds by one available original capture pixel on each side. */
		static void AddTransparentGuard(std::array<std::size_t, 4>& crop) noexcept
		{
			if (crop[0]) --crop[0];
			if (crop[1]) --crop[1];
			if (crop[2] < 256) ++crop[2];
			if (crop[3] < 256) ++crop[3];
		}

		/** Applies the existing orthographic-footprint density limit without upsampling. */
		static double CalculateDensityRatio(const SpriteCaptureMetadata& metadata) noexcept
		{
			const double requested = (metadata.OrthographicExtent / 256.0) * metadata.PixelsPerWorldUnit;
			return std::isfinite(requested) && requested > 0 ? (std::min)(1.0, requested) : requested;
		}

		/** Allocates ceil-rounded output, filters in linear light, and adopts it with full-capture mapping. */
		static SpriteProcessingResult Resample(
			const std::vector<std::uint8_t>& rgba,
			const Visuals::SpriteCaptureBounds& bounds,
			const std::array<std::size_t, 4>& crop,
			double ratio,
			double step)
		{
			const std::uint32_t width = static_cast<std::uint32_t>(std::ceil((crop[2] - crop[0]) * ratio));
			const std::uint32_t height = static_cast<std::uint32_t>(std::ceil((crop[3] - crop[1]) * ratio));
			if (!width || !height || width > 256 || height > 256)
			{
				return {SpriteProcessingResult::Status::InvalidInput, {}};
			}

			std::vector<std::uint8_t> output(std::size_t(width) * height * 4);
			FilterPixels(rgba, crop, width, height, step, output);
			if (!HasCoverage(output))
			{
				return {SpriteProcessingResult::Status::EmptyCapture, {}};
			}
			return AdoptOutput(std::move(output), width, height, bounds, crop, step);
		}

		/** Applies area-weighted filtering with premultiplied coverage and sRGB linearization. */
		static void FilterPixels(
			const std::vector<std::uint8_t>& rgba,
			const std::array<std::size_t, 4>& crop,
			std::uint32_t width,
			std::uint32_t height,
			double step,
			std::vector<std::uint8_t>& output)
		{
			const auto linear = MakeLinearTable();
			for (std::uint32_t y = 0; y < height; ++y)
			{
				for (std::uint32_t x = 0; x < width; ++x)
				{
					FilterPixel(rgba, crop, x, y, step, linear, output.data() + (std::size_t(y) * width + x) * 4);
				}
			}
		}

		/** Builds the established sRGB byte-to-linear-light lookup table. */
		static std::array<double, 256> MakeLinearTable() noexcept
		{
			std::array<double, 256> table{};
			for (std::size_t value = 0; value < table.size(); ++value)
			{
				const double encoded = double(value) / 255.0;
				table[value] = encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
			}
			return table;
		}

		/** Integrates source-pixel overlap for one output pixel and encodes straight RGBA8. */
		static void FilterPixel(
			const std::vector<std::uint8_t>& rgba,
			const std::array<std::size_t, 4>& crop,
			std::uint32_t x,
			std::uint32_t y,
			double step,
			const std::array<double, 256>& linear,
			std::uint8_t* output)
		{
			const double left = crop[0] + x * step;
			const double top = crop[1] + y * step;
			const double right = (std::min)(double(crop[2]), left + step);
			const double bottom = (std::min)(double(crop[3]), top + step);
			std::array<double, 4> accumulated{};
			AccumulatePixel(rgba, left, top, right, bottom, linear, accumulated);
			EncodePixel(accumulated, step * step, output);
		}

		/** Accumulates area times alpha and linear-light color for one source footprint. */
		static void AccumulatePixel(
			const std::vector<std::uint8_t>& rgba,
			double left,
			double top,
			double right,
			double bottom,
			const std::array<double, 256>& linear,
			std::array<double, 4>& accumulated)
		{
			for (std::size_t sourceY = std::size_t(std::floor(top)); sourceY < std::size_t(std::ceil(bottom)); ++sourceY)
			{
				for (std::size_t sourceX = std::size_t(std::floor(left)); sourceX < std::size_t(std::ceil(right)); ++sourceX)
				{
					AccumulateSourceSample(rgba, sourceX, sourceY, left, top, right, bottom, linear, accumulated);
				}
			}
		}

		/** Adds one clipped source texel's coverage-weighted color contribution. */
		static void AccumulateSourceSample(
			const std::vector<std::uint8_t>& rgba,
			std::size_t x,
			std::size_t y,
			double left,
			double top,
			double right,
			double bottom,
			const std::array<double, 256>& linear,
			std::array<double, 4>& accumulated)
		{
			const double overlap = ((std::min)(right, double(x + 1)) - (std::max)(left, double(x))) *
				((std::min)(bottom, double(y + 1)) - (std::max)(top, double(y)));
			const std::size_t offset = (y * 256 + x) * 4;
			const double covered = overlap * double(rgba[offset + 3]) / 255.0;
			accumulated[3] += covered;
			for (std::size_t channel = 0; channel < 3; ++channel)
			{
				accumulated[channel] += covered * linear[rgba[offset + channel]];
			}
		}

		/** Encodes alpha against the full texel area and unpremultiplies linear color. */
		static void EncodePixel(const std::array<double, 4>& accumulated, double area, std::uint8_t* output) noexcept
		{
			output[3] = static_cast<std::uint8_t>(std::lround(255.0 * std::clamp(accumulated[3] / area, 0.0, 1.0)));
			if (!output[3] || accumulated[3] <= 0)
			{
				return;
			}
			for (std::size_t channel = 0; channel < 3; ++channel)
			{
				const double value = std::clamp(accumulated[channel] / accumulated[3], 0.0, 1.0);
				const double encoded = value <= 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
				output[channel] = static_cast<std::uint8_t>(std::lround(255.0 * encoded));
			}
		}

		/** Detects a nonempty final alpha channel before exposing the payload. */
		static bool HasCoverage(const std::vector<std::uint8_t>& rgba) noexcept
		{
			for (std::size_t offset = 3; offset < rgba.size(); offset += 4)
			{
				if (rgba[offset]) return true;
			}
			return false;
		}

		/** Adopts processed pixels and records the original-coordinate projection rectangle. */
		static SpriteProcessingResult AdoptOutput(
			std::vector<std::uint8_t>&& output,
			std::uint32_t width,
			std::uint32_t height,
			const Visuals::SpriteCaptureBounds& bounds,
			const std::array<std::size_t, 4>& crop,
			double step)
		{
			const Visuals::SpriteProjectionRectangle projection{
				double(crop[0]), double(crop[1]), double(crop[0]) + width * step, double(crop[1]) + height * step};
			std::shared_ptr<const ProcessedSpritePayload> payload;
			if (!ProcessedSpritePayload::TryAdopt(std::move(output), width, height, {256, 256}, bounds, projection, payload))
			{
				return {SpriteProcessingResult::Status::InvalidInput, {}};
			}
			return {SpriteProcessingResult::Status::Processed, std::move(payload)};
		}
	};
}
