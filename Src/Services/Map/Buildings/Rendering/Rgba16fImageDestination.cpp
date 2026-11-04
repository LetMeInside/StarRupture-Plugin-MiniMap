// Implements independent RGBA8 image, solid-triangle, and textured-triangle rasterization for the Building Map library.
#include "Rgba16fImageDestination.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <climits>
#include <limits>

namespace Services::Map::Buildings::Rendering
{
	/** Creates one validated global grid and allocates its reusable four-sample buffer. */
	Rgba16fImageDestination::Rgba16fImageDestination(Rgba16fImageGrid grid) noexcept
		: grid_(grid)
	{
		if (!IsValidGrid(grid_, worldBounds_)) return;
		try
		{
			const std::size_t pixels = std::size_t(grid_.Width) * grid_.Height;
			samples_.resize(pixels * SamplesPerPixel);
			for (auto& sample : samples_) sample = {0.0f, 0.0f, 0.0f, 1.0f};
			initialized_ = true;
		}
		catch (...)
		{
			samples_.clear();
			failed_ = true;
		}
	}

	/** Validates the image command, then rasterizes its two source-ordered quad triangles. */
	ImageSubmission Rgba16fImageDestination::SubmitImage(const ImageSubmissionCommand& command)
	{
		if (!initialized_ || failed_) return ImageSubmission::Unavailable;
		if (!IsValidImage(command)) return ImageSubmission::InvalidInput;

		std::array<RasterVertex, 4> corners{};
		constexpr std::array<std::array<double, 2>, 4> uv{{{{0.0, 0.0}}, {{1.0, 0.0}}, {{1.0, 1.0}}, {{0.0, 1.0}}}};
		for (std::size_t index = 0; index < corners.size(); ++index)
		{
			const auto& position = command.Geometry.Corners[index];
			if (!std::isfinite(position[0]) || !std::isfinite(position[1])) return ImageSubmission::InvalidInput;
			corners[index] = {position[0], position[1], uv[index][0], uv[index][1]};
		}
		if (!IntersectsGrid(command.Geometry)) return ImageSubmission::Culled;

		const std::size_t sampleCount = samples_.size();
		ImageShadeContext first{command, corners, false};
		ImageShadeContext second{command, corners, true};
		const bool firstComplete = RasterTriangle({corners[0], corners[1], corners[2]}, samples_,
			[&](double a, double b, double c, LinearPixel& output)
			{
				return ShadeImage(first, a, b, c, output);
			});
		const bool complete = firstComplete && RasterTriangle({corners[0], corners[2], corners[3]}, samples_,
			[&](double a, double b, double c, LinearPixel& output)
			{
				return ShadeImage(second, a, b, c, output);
			});
		if (!complete || samples_.size() != sampleCount)
		{
			failed_ = true;
			return ImageSubmission::Rejected;
		}
		return ImageSubmission::Submitted;
	}

	/** Submits solid triangles synchronously in span order and poisons finalization after raster failure. */
	ImageSubmission Rgba16fImageDestination::SubmitSolidTriangles(const SolidTriangleBatch& batch)
	{
		if (!initialized_ || failed_) return ImageSubmission::Unavailable;
		if (batch.Triangles.empty()) return ImageSubmission::Culled;

		bool submitted = false;
		for (const auto& triangle : batch.Triangles)
		{
			if (!HasFiniteTriangleXY(triangle)) return ImageSubmission::InvalidInput;
			if (!IntersectsGrid(triangle)) continue;
			const std::size_t sampleCount = samples_.size();
			if (!RasterSolidTriangle(triangle, batch.Rgb, batch.Opacity) || samples_.size() != sampleCount)
			{
				failed_ = true;
				return ImageSubmission::Rejected;
			}
			submitted = true;
		}
		return submitted ? ImageSubmission::Submitted : ImageSubmission::Culled;
	}

	/** Rasterizes textured triangles synchronously and poisons finalization after a raster failure. */
	ImageSubmission Rgba16fImageDestination::SubmitTexturedTriangles(const TexturedTriangleBatch& batch)
	{
		if (!initialized_ || failed_) return ImageSubmission::Unavailable;
		ImageSubmissionCommand sourceValidation;
		sourceValidation.Source = batch.Source;
		if (!IsValidImage(sourceValidation) || !std::isfinite(batch.UvScale[0]) ||
			!std::isfinite(batch.UvScale[1]) || !std::isfinite(batch.UvOffset[0]) ||
			!std::isfinite(batch.UvOffset[1]) ||
			(batch.AddressU != TextureAddressMode::Clamp && batch.AddressU != TextureAddressMode::Repeat) ||
			(batch.AddressV != TextureAddressMode::Clamp && batch.AddressV != TextureAddressMode::Repeat))
		{
			return ImageSubmission::InvalidInput;
		}
		if (batch.Triangles.empty()) return ImageSubmission::Culled;

		bool submitted = false;
		for (const auto& triangle : batch.Triangles)
		{
			if (!HasFiniteTexturedTriangle(triangle)) return ImageSubmission::InvalidInput;
			if (!IntersectsGrid(SolidTriangle{{triangle.Vertices[0].Position, triangle.Vertices[1].Position,
				triangle.Vertices[2].Position}})) continue;
			const std::size_t sampleCount = samples_.size();
			if (!RasterTexturedTriangle(triangle, batch) || samples_.size() != sampleCount)
			{
				failed_ = true;
				return ImageSubmission::Rejected;
			}
			submitted = true;
		}
		return submitted ? ImageSubmission::Submitted : ImageSubmission::Culled;
	}

	/** Clears output on failure and resolves the complete multisample buffer to little-endian RGBA16F. */
	bool Rgba16fImageDestination::Finalize(std::vector<std::uint8_t>& output) const noexcept
	{
		output.clear();
		if (!initialized_ || failed_) return false;
		try
		{
			if (Resolve(output)) return true;
			output.clear();
			return false;
		}
		catch (...)
		{
			output.clear();
			return false;
		}
	}

	/** Validates grid dimensions, allocation arithmetic, scale, and signed-cell world bounds. */
	bool Rgba16fImageDestination::IsValidGrid(
		const Rgba16fImageGrid& grid,
		std::array<long double, 4>& worldBounds) noexcept
	{
		if (!grid.Width || !grid.Height || grid.Width > INT_MAX || grid.Height > INT_MAX ||
			!std::isfinite(grid.WorldOriginX) || !std::isfinite(grid.WorldOriginY) ||
			!std::isfinite(grid.WorldUnitsPerPixel) || grid.WorldUnitsPerPixel <= 0.0)
		{
			return false;
		}

		const std::size_t pixels = std::size_t(grid.Width) * std::size_t(grid.Height);
		if (pixels / grid.Width != grid.Height || pixels > (std::numeric_limits<std::size_t>::max)() / SamplesPerPixel)
		{
			return false;
		}
		return TryGetGridBounds(grid, worldBounds);
	}

	/** Calculates finite half-open world bounds using widened signed-cell arithmetic. */
	bool Rgba16fImageDestination::TryGetGridBounds(
		const Rgba16fImageGrid& grid,
		std::array<long double, 4>& bounds) noexcept
	{
		const long double left = static_cast<long double>(grid.WorldOriginX) +
			static_cast<long double>(grid.CellX) * grid.Width * grid.WorldUnitsPerPixel;
		const long double bottom = static_cast<long double>(grid.WorldOriginY) +
			static_cast<long double>(grid.CellY) * grid.Height * grid.WorldUnitsPerPixel;
		const long double right = left + static_cast<long double>(grid.Width) * grid.WorldUnitsPerPixel;
		const long double top = bottom + static_cast<long double>(grid.Height) * grid.WorldUnitsPerPixel;
		if (!std::isfinite(left) || !std::isfinite(bottom) || !std::isfinite(right) || !std::isfinite(top)) return false;
		bounds = {left, bottom, right, top};
		return true;
	}

	/** Validates image ownership, dimensions, exact RGBA8 byte count, and the half-open pixel region. */
	bool Rgba16fImageDestination::IsValidImage(const ImageSubmissionCommand& command) noexcept
	{
		const auto& source = command.Source;
		if (!source.Pixels || !source.Width || !source.Height) return false;
		const std::size_t pixels = std::size_t(source.Width) * std::size_t(source.Height);
		if (pixels / source.Width != source.Height || pixels > (std::numeric_limits<std::size_t>::max)() / 4 ||
			source.Pixels->size() != pixels * 4)
		{
			return false;
		}
		for (double edge : source.PixelRegion)
		{
			if (!std::isfinite(edge)) return false;
		}
		return source.PixelRegion[0] >= 0.0 && source.PixelRegion[1] >= 0.0 &&
			source.PixelRegion[2] > source.PixelRegion[0] && source.PixelRegion[3] > source.PixelRegion[1] &&
			source.PixelRegion[2] <= source.Width && source.PixelRegion[3] <= source.Height;
	}

	/** Returns true when the world-space image quad overlaps or touches the destination grid. */
	bool Rgba16fImageDestination::IntersectsGrid(const ImageWorldQuad& quad) const noexcept
	{
		double minX = quad.Corners[0][0];
		double maxX = minX;
		double minY = quad.Corners[0][1];
		double maxY = minY;
		for (const auto& corner : quad.Corners)
		{
			minX = (std::min)(minX, corner[0]);
			maxX = (std::max)(maxX, corner[0]);
			minY = (std::min)(minY, corner[1]);
			maxY = (std::max)(maxY, corner[1]);
		}
		return !(static_cast<long double>(maxX) < worldBounds_[0] || static_cast<long double>(minX) > worldBounds_[2] ||
			static_cast<long double>(maxY) < worldBounds_[1] || static_cast<long double>(minY) > worldBounds_[3]);
	}

	/** Returns true when the triangle XY bounds overlap or touch the destination grid. */
	bool Rgba16fImageDestination::IntersectsGrid(const SolidTriangle& triangle) const noexcept
	{
		long double minX = triangle.Vertices[0][0];
		long double maxX = minX;
		long double minY = triangle.Vertices[0][1];
		long double maxY = minY;
		for (const auto& vertex : triangle.Vertices)
		{
			minX = (std::min)(minX, static_cast<long double>(vertex[0]));
			maxX = (std::max)(maxX, static_cast<long double>(vertex[0]));
			minY = (std::min)(minY, static_cast<long double>(vertex[1]));
			maxY = (std::max)(maxY, static_cast<long double>(vertex[1]));
		}
		return !(maxX < worldBounds_[0] || minX > worldBounds_[2] || minY < worldBounds_[1] || minY > worldBounds_[3]);
	}

	/** Rejects non-finite XY while deliberately leaving Z unused by 2D rasterization. */
	bool Rgba16fImageDestination::HasFiniteTriangleXY(const SolidTriangle& triangle) noexcept
	{
		for (const auto& vertex : triangle.Vertices)
		{
			if (!std::isfinite(vertex[0]) || !std::isfinite(vertex[1])) return false;
		}
		return true;
	}

	/** Validates all position and texture coordinates before raster work begins. */
	bool Rgba16fImageDestination::HasFiniteTexturedTriangle(const TexturedTriangle& triangle) noexcept
	{
		for (const auto& vertex : triangle.Vertices)
		{
			for (const double value : vertex.Position)
			{
				if (!std::isfinite(value)) return false;
			}
			for (const double value : vertex.TextureCoordinates)
			{
				if (!std::isfinite(value)) return false;
			}
		}
		return true;
	}

	/** Samples source RGBA8 bilinearly while clamping filter taps to the source region. */
	bool Rgba16fImageDestination::SampleRgba8(
		const Rgba8ImageSource& source,
		double u,
		double v,
		TextureAddressMode addressU,
		TextureAddressMode addressV,
		LinearPixel& output) noexcept
	{
		if (!source.Pixels || !source.Width || !source.Height || !std::isfinite(u) || !std::isfinite(v)) return false;
		const double regionWidth = source.PixelRegion[2] - source.PixelRegion[0];
		const double regionHeight = source.PixelRegion[3] - source.PixelRegion[1];
		if (!std::isfinite(regionWidth) || !std::isfinite(regionHeight) || regionWidth <= 0.0 || regionHeight <= 0.0)
		{
			return false;
		}

		u = addressU == TextureAddressMode::Repeat ? u - std::floor(u) : (std::clamp)(u, 0.0, 1.0);
		v = addressV == TextureAddressMode::Repeat ? v - std::floor(v) : (std::clamp)(v, 0.0, 1.0);
		const double x = source.PixelRegion[0] + u * regionWidth - 0.5;
		const double y = source.PixelRegion[1] + v * regionHeight - 0.5;
		const double floorX = std::floor(x);
		const double floorY = std::floor(y);
		const std::int64_t x0 = static_cast<std::int64_t>(floorX);
		const std::int64_t y0 = static_cast<std::int64_t>(floorY);
		const double tx = x - floorX;
		const double ty = y - floorY;
		const std::int64_t minX = static_cast<std::int64_t>(std::ceil(source.PixelRegion[0] - 0.5));
		const std::int64_t minY = static_cast<std::int64_t>(std::ceil(source.PixelRegion[1] - 0.5));
		const std::int64_t maxX = static_cast<std::int64_t>(std::ceil(source.PixelRegion[2] - 0.5)) - 1;
		const std::int64_t maxY = static_cast<std::int64_t>(std::ceil(source.PixelRegion[3] - 0.5)) - 1;
		return InterpolateSourcePixels(source, x0, y0, minX, minY, maxX, maxY, tx, ty,
			addressU, addressV, output);
	}

	/** Fetches and bilinearly combines the four bounded source texels for one image sample. */
	bool Rgba16fImageDestination::InterpolateSourcePixels(
		const Rgba8ImageSource& source,
		std::int64_t x0,
		std::int64_t y0,
		std::int64_t minX,
		std::int64_t minY,
		std::int64_t maxX,
		std::int64_t maxY,
		double tx,
		double ty,
		TextureAddressMode addressU,
		TextureAddressMode addressV,
		LinearPixel& output) noexcept
	{
		LinearPixel p00;
		LinearPixel p10;
		LinearPixel p01;
		LinearPixel p11;
		if (!FetchPixel(source, x0, y0, minX, minY, maxX, maxY, addressU, addressV, p00) ||
			!FetchPixel(source, x0 + 1, y0, minX, minY, maxX, maxY, addressU, addressV, p10) ||
			!FetchPixel(source, x0, y0 + 1, minX, minY, maxX, maxY, addressU, addressV, p01) ||
			!FetchPixel(source, x0 + 1, y0 + 1, minX, minY, maxX, maxY, addressU, addressV, p11))
		{
			return false;
		}
		const auto mix = [tx, ty](float a, float b, float c, float d)
		{
			const double upper = double(a) * (1.0 - tx) + double(b) * tx;
			const double lower = double(c) * (1.0 - tx) + double(d) * tx;
			return float(upper * (1.0 - ty) + lower * ty);
		};
		output = {mix(p00.R, p10.R, p01.R, p11.R), mix(p00.G, p10.G, p01.G, p11.G),
			mix(p00.B, p10.B, p01.B, p11.B), mix(p00.A, p10.A, p01.A, p11.A)};
		return true;
	}

	/** Reads a clamped texel and converts sRGB color while preserving straight alpha. */
	bool Rgba16fImageDestination::FetchPixel(
		const Rgba8ImageSource& source,
		std::int64_t pixelX,
		std::int64_t pixelY,
		std::int64_t minX,
		std::int64_t minY,
		std::int64_t maxX,
		std::int64_t maxY,
		TextureAddressMode addressU,
		TextureAddressMode addressV,
		LinearPixel& output) noexcept
	{
		const std::int64_t boundedMinimumX = (std::max)(std::int64_t(0), minX);
		const std::int64_t boundedMinimumY = (std::max)(std::int64_t(0), minY);
		const std::int64_t boundedMaximumX = (std::min)(std::int64_t(source.Width) - 1, maxX);
		const std::int64_t boundedMaximumY = (std::min)(std::int64_t(source.Height) - 1, maxY);
		if (boundedMinimumX > boundedMaximumX || boundedMinimumY > boundedMaximumY) return false;
		pixelX = AddressPixel(pixelX, boundedMinimumX, boundedMaximumX, addressU);
		pixelY = AddressPixel(pixelY, boundedMinimumY, boundedMaximumY, addressV);
		if (pixelX < 0 || pixelY < 0) return false;
		const std::size_t offset = (std::size_t(pixelY) * source.Width + std::size_t(pixelX)) * 4;
		if (offset > source.Pixels->size() || source.Pixels->size() - offset < 4) return false;
		const auto* bytes = source.Pixels->data() + offset;
		output = {float(SrgbToLinear(bytes[0])), float(SrgbToLinear(bytes[1])),
			float(SrgbToLinear(bytes[2])), float(bytes[3]) / 255.0f};
		return true;
	}

	/** Applies clamp or positive modulo within the validated source-region pixel range. */
	std::int64_t Rgba16fImageDestination::AddressPixel(
		std::int64_t pixel,
		std::int64_t minimum,
		std::int64_t maximum,
		TextureAddressMode addressMode) noexcept
	{
		if (addressMode == TextureAddressMode::Clamp) return (std::clamp)(pixel, minimum, maximum);
		const std::int64_t count = maximum - minimum + 1;
		const std::int64_t offset = (pixel - minimum) % count;
		return minimum + (offset < 0 ? offset + count : offset);
	}

	/** Returns one precomputed linear-light value for an 8-bit sRGB channel. */
	double Rgba16fImageDestination::SrgbToLinear(std::uint8_t channel) noexcept
	{
		static const std::array<double, 256> lookup = []
		{
			std::array<double, 256> values{};
			for (std::size_t index = 0; index < values.size(); ++index)
			{
				const double value = double(index) / 255.0;
				values[index] = value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
			}
			return values;
		}();
		return lookup[channel];
	}

	/** Interpolates normalized image UVs from barycentric weights and samples the immutable pixel source. */
	bool Rgba16fImageDestination::ShadeImage(
		const ImageShadeContext& context,
		double firstWeight,
		double secondWeight,
		double thirdWeight,
		LinearPixel& output) noexcept
	{
		const auto& corners = context.Corners;
		const std::size_t first = 0;
		const std::size_t second = context.OtherTriangle ? 2 : 1;
		const std::size_t third = context.OtherTriangle ? 3 : 2;
		const double u = corners[first].U * firstWeight + corners[second].U * secondWeight + corners[third].U * thirdWeight;
		const double v = corners[first].V * firstWeight + corners[second].V * secondWeight + corners[third].V * thirdWeight;
		return SampleRgba8(context.Command.Source, u, v, TextureAddressMode::Clamp,
			TextureAddressMode::Clamp, output);
	}

	/** Samples barycentrically interpolated UVs and applies linear-light RGB modulation and opacity. */
	bool Rgba16fImageDestination::ShadeTexture(
		const TextureShadeContext& context,
		const std::array<RasterVertex, 3>& vertices,
		double firstWeight,
		double secondWeight,
		double thirdWeight,
		LinearPixel& output) noexcept
	{
		const auto& batch = context.Batch;
		const double u = vertices[0].U * firstWeight + vertices[1].U * secondWeight + vertices[2].U * thirdWeight;
		const double v = vertices[0].V * firstWeight + vertices[1].V * secondWeight + vertices[2].V * thirdWeight;
		if (!SampleRgba8(batch.Source, u, v, batch.AddressU, batch.AddressV, output)) return false;
		output.R *= float(SrgbToLinear(std::uint8_t((batch.ModulationRgb >> 16) & 0xFF)));
		output.G *= float(SrgbToLinear(std::uint8_t((batch.ModulationRgb >> 8) & 0xFF)));
		output.B *= float(SrgbToLinear(std::uint8_t(batch.ModulationRgb & 0xFF)));
		output.A *= float(batch.Opacity) / 255.0f;
		return true;
	}

	/** Rasterizes one triangle with shared-edge top-left ownership and the established four sample positions. */
	template <typename ShadeFunction>
	bool Rgba16fImageDestination::RasterTriangle(
		std::array<RasterVertex, 3> vertices,
		SampleBuffer& samples,
		ShadeFunction&& shade) const noexcept
	{
		for (const auto& vertex : vertices)
		{
			if (!std::isfinite(vertex.X) || !std::isfinite(vertex.Y) ||
				!std::isfinite(vertex.U) || !std::isfinite(vertex.V)) return false;
		}
		double area = Edge(vertices[0], vertices[1], vertices[2].X, vertices[2].Y);
		if (!std::isfinite(area)) return false;
		if (area == 0.0) return true;
		const bool swapped = area < 0.0;
		if (swapped)
		{
			std::swap(vertices[1], vertices[2]);
			area = -area;
		}
		std::array<int, 4> bounds{};
		bool hasPixels = false;
		if (!TryGetRasterBounds(vertices, bounds, hasPixels)) return false;
		if (!hasPixels) return true;
		const std::array<bool, 3> topLeftEdges{{
			IsTopLeftEdge(vertices[1], vertices[2]),
			IsTopLeftEdge(vertices[2], vertices[0]),
			IsTopLeftEdge(vertices[0], vertices[1])}};
		return RasterizeSamples(vertices, bounds, area, swapped, topLeftEdges, samples, shade);
	}

	/** Calculates clamped integer pixel bounds while preserving the reference's one-pixel conservative expansion. */
	bool Rgba16fImageDestination::TryGetRasterBounds(
		const std::array<RasterVertex, 3>& vertices,
		std::array<int, 4>& bounds,
		bool& hasPixels) const noexcept
	{
		const long double sideX = grid_.Width;
		const long double sideY = grid_.Height;
		long double minX = std::numeric_limits<long double>::infinity();
		long double minY = minX;
		long double maxX = -minX;
		long double maxY = -minX;
		for (const auto& vertex : vertices)
		{
			const long double pixelX = (static_cast<long double>(vertex.X) - grid_.WorldOriginX) / grid_.WorldUnitsPerPixel -
				static_cast<long double>(grid_.CellX) * sideX;
			const long double pixelY = (static_cast<long double>(vertex.Y) - grid_.WorldOriginY) / grid_.WorldUnitsPerPixel -
				static_cast<long double>(grid_.CellY) * sideY;
			minX = (std::min)(minX, pixelX);
			maxX = (std::max)(maxX, pixelX);
			minY = (std::min)(minY, pixelY);
			maxY = (std::max)(maxY, pixelY);
		}
		if (!std::isfinite(minX) || !std::isfinite(maxX) || !std::isfinite(minY) || !std::isfinite(maxY)) return false;
		if (maxX < 0.0L || maxY < 0.0L || minX > sideX || minY > sideY)
		{
			hasPixels = false;
			return true;
		}
		bounds = {LowPixel(minX, grid_.Width), LowPixel(minY, grid_.Height),
			HighPixel(maxX, grid_.Width), HighPixel(maxY, grid_.Height)};
		hasPixels = bounds[0] <= bounds[2] && bounds[1] <= bounds[3];
		return true;
	}

	/** Computes the reference conservative lower raster bound. */
	int Rgba16fImageDestination::LowPixel(long double value, std::uint32_t extent) noexcept
	{
		const long double clipped = (std::min)(value, static_cast<long double>(extent));
		return int((std::max)(0.0L, std::floor(clipped) - 1.0L));
	}

	/** Computes the reference conservative upper raster bound. */
	int Rgba16fImageDestination::HighPixel(long double value, std::uint32_t extent) noexcept
	{
		const long double clipped = (std::max)(value, 0.0L);
		return int((std::min)(static_cast<long double>(extent - 1), std::ceil(clipped) + 1.0L));
	}

	/** Visits four fixed subpixel samples, applies top-left edge tests, and blends source-over. */
	template <typename ShadeFunction>
	bool Rgba16fImageDestination::RasterizeSamples(
		const std::array<RasterVertex, 3>& vertices,
		const std::array<int, 4>& bounds,
		double area,
		bool swapped,
		const std::array<bool, 3>& topLeftEdges,
		SampleBuffer& samples,
		ShadeFunction&& shade) const noexcept
	{
		constexpr std::array<std::array<double, 2>, 4> offsets{{
			{{0.25, 0.25}}, {{0.75, 0.25}}, {{0.25, 0.75}}, {{0.75, 0.75}}}};
		for (int pixelY = bounds[1]; pixelY <= bounds[3]; ++pixelY)
		{
			for (int pixelX = bounds[0]; pixelX <= bounds[2]; ++pixelX)
			{
				if (!RasterPixelSamples(vertices, pixelX, pixelY, area, swapped, topLeftEdges,
					offsets, samples, shade)) return false;
			}
		}
		return true;
	}

	/** Tests and shades the four samples of one pixel while preserving source-over accumulation. */
	template <typename ShadeFunction>
	bool Rgba16fImageDestination::RasterPixelSamples(
		const std::array<RasterVertex, 3>& vertices,
		int pixelX,
		int pixelY,
		double area,
		bool swapped,
		const std::array<bool, 3>& topLeftEdges,
		const std::array<std::array<double, 2>, 4>& offsets,
		SampleBuffer& samples,
		ShadeFunction&& shade) const noexcept
	{
		const std::size_t pixel = std::size_t(pixelY) * grid_.Width + std::size_t(pixelX);
		for (std::size_t sampleIndex = 0; sampleIndex < offsets.size(); ++sampleIndex)
		{
			const double worldX = GlobalPixel(grid_.CellX, std::size_t(pixelX), offsets[sampleIndex][0],
				grid_.WorldOriginX, grid_.Width, grid_.WorldUnitsPerPixel);
			const double worldY = GlobalPixel(grid_.CellY, std::size_t(pixelY), offsets[sampleIndex][1],
				grid_.WorldOriginY, grid_.Height, grid_.WorldUnitsPerPixel);
			if (!std::isfinite(worldX) || !std::isfinite(worldY)) return false;
			if (!ShadeCoveredSample(vertices, worldX, worldY, area, swapped, topLeftEdges,
				samples[pixel * SamplesPerPixel + sampleIndex], shade)) return false;
		}
		return true;
	}

	/** Applies edge ownership, interpolates covered samples, and blends them into one destination sample. */
	template <typename ShadeFunction>
	bool Rgba16fImageDestination::ShadeCoveredSample(
		const std::array<RasterVertex, 3>& vertices,
		double worldX,
		double worldY,
		double area,
		bool swapped,
		const std::array<bool, 3>& topLeftEdges,
		Sample& destination,
		ShadeFunction&& shade) const noexcept
	{
		const double firstEdge = Edge(vertices[1], vertices[2], worldX, worldY);
		const double secondEdge = Edge(vertices[2], vertices[0], worldX, worldY);
		const double thirdEdge = Edge(vertices[0], vertices[1], worldX, worldY);
		if (IsOutsideTriangle(firstEdge, secondEdge, thirdEdge, topLeftEdges)) return true;
		LinearPixel color;
		const bool shaded = swapped ? shade(firstEdge / area, thirdEdge / area, secondEdge / area, color) :
			shade(firstEdge / area, secondEdge / area, thirdEdge / area, color);
		if (!shaded) return false;
		const float alpha = (std::clamp)(color.A, 0.0f, 1.0f);
		const float transmittance = 1.0f - alpha;
		destination[0] = color.R * alpha + destination[0] * transmittance;
		destination[1] = color.G * alpha + destination[1] * transmittance;
		destination[2] = color.B * alpha + destination[2] * transmittance;
		destination[3] *= transmittance;
		return true;
	}

	/** Applies strict edge and top-left tests to determine whether one sample belongs to a triangle. */
	bool Rgba16fImageDestination::IsOutsideTriangle(
		double firstEdge,
		double secondEdge,
		double thirdEdge,
		const std::array<bool, 3>& topLeftEdges) noexcept
	{
		return firstEdge < 0.0 || (firstEdge == 0.0 && !topLeftEdges[0]) ||
			secondEdge < 0.0 || (secondEdge == 0.0 && !topLeftEdges[1]) ||
			thirdEdge < 0.0 || (thirdEdge == 0.0 && !topLeftEdges[2]);
	}

	/** Converts one world-space solid triangle to zero-UV raster vertices and shades it in linear light. */
	bool Rgba16fImageDestination::RasterSolidTriangle(
		const SolidTriangle& triangle,
		std::uint32_t rgb,
		std::uint8_t opacity) noexcept
	{
		std::array<RasterVertex, 3> vertices{};
		for (std::size_t index = 0; index < vertices.size(); ++index)
		{
			vertices[index] = {triangle.Vertices[index][0], triangle.Vertices[index][1], 0.0, 0.0};
		}
		const LinearPixel color{
			float(SrgbToLinear(std::uint8_t((rgb >> 16) & 0xFF))),
			float(SrgbToLinear(std::uint8_t((rgb >> 8) & 0xFF))),
			float(SrgbToLinear(std::uint8_t(rgb & 0xFF))), float(opacity) / 255.0f};
		return RasterTriangle(vertices, samples_,
			[color](double, double, double, LinearPixel& output)
			{
				output = color;
				return true;
			});
	}

	/** Applies the batch UV transform and uses the shared four-sample rasterizer. */
	bool Rgba16fImageDestination::RasterTexturedTriangle(
		const TexturedTriangle& triangle,
		const TexturedTriangleBatch& batch) noexcept
	{
		std::array<RasterVertex, 3> vertices{};
		for (std::size_t index = 0; index < vertices.size(); ++index)
		{
			const auto& source = triangle.Vertices[index];
			vertices[index] = {source.Position[0], source.Position[1],
				source.TextureCoordinates[0] * batch.UvScale[0] + batch.UvOffset[0],
				source.TextureCoordinates[1] * batch.UvScale[1] + batch.UvOffset[1]};
		}
		const TextureShadeContext context{batch};
		return RasterTriangle(vertices, samples_,
			[&](double first, double second, double third, LinearPixel& output)
			{
				return ShadeTexture(context, vertices, first, second, third, output);
			});
	}

	/** Maps signed section and local pixel indices to global coordinates with widened arithmetic. */
	double Rgba16fImageDestination::GlobalPixel(
		std::int64_t cell,
		std::size_t pixel,
		double sampleOffset,
		double origin,
		std::uint32_t side,
		double worldUnitsPerPixel) noexcept
	{
		const long double index = static_cast<long double>(cell) * side +
			static_cast<long double>(pixel) + sampleOffset;
		return static_cast<double>(static_cast<long double>(origin) + index * worldUnitsPerPixel);
	}

	/** Computes the oriented edge value for one world-space sample. */
	double Rgba16fImageDestination::Edge(
		const RasterVertex& first,
		const RasterVertex& second,
		double x,
		double y) noexcept
	{
		return (second.X - first.X) * (y - first.Y) - (second.Y - first.Y) * (x - first.X);
	}

	/** Selects inclusive top-left edges for deterministic shared-edge coverage. */
	bool Rgba16fImageDestination::IsTopLeftEdge(const RasterVertex& first, const RasterVertex& second) noexcept
	{
		const double dx = second.X - first.X;
		const double dy = second.Y - first.Y;
		return dy < 0.0 || (dy == 0.0 && dx > 0.0);
	}

	/** Converts a finite IEEE-754 single value to binary16 using ties-to-even rounding. */
	std::uint16_t Rgba16fImageDestination::FloatToHalf(float value) noexcept
	{
		const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
		const std::uint32_t sign = (bits >> 16) & 0x8000;
		const std::uint32_t exponent = (bits >> 23) & 0xFF;
		std::uint32_t mantissa = bits & 0x7FFFFF;
		if (exponent == 0xFF) return std::uint16_t(sign | 0x7C00 | (mantissa ? 0x0200 : 0));
		int halfExponent = int(exponent) - 127 + 15;
		if (halfExponent >= 31) return std::uint16_t(sign | 0x7C00);
		if (halfExponent <= 0) return ConvertHalfSubnormal(sign, halfExponent, mantissa);
		return ConvertHalfNormal(sign, halfExponent, mantissa);
	}

	/** Encodes a rounded subnormal half value while preserving the reference conversion. */
	std::uint16_t Rgba16fImageDestination::ConvertHalfSubnormal(
		std::uint32_t sign,
		int exponent,
		std::uint32_t mantissa) noexcept
	{
		if (exponent < -10) return std::uint16_t(sign);
		mantissa |= 0x800000;
		const unsigned shift = unsigned(14 - exponent);
		std::uint32_t rounded = mantissa >> shift;
		const std::uint32_t remainder = mantissa & ((std::uint32_t(1) << shift) - 1);
		const std::uint32_t halfway = std::uint32_t(1) << (shift - 1);
		if (remainder > halfway || (remainder == halfway && (rounded & 1))) ++rounded;
		return std::uint16_t(sign | rounded);
	}

	/** Encodes a rounded normal half value and carries mantissa overflow into its exponent. */
	std::uint16_t Rgba16fImageDestination::ConvertHalfNormal(
		std::uint32_t sign,
		int exponent,
		std::uint32_t mantissa) noexcept
	{
		std::uint32_t rounded = mantissa >> 13;
		const std::uint32_t remainder = mantissa & 0x1FFF;
		if (remainder > 0x1000 || (remainder == 0x1000 && (rounded & 1)))
		{
			if (++rounded == 0x400)
			{
				rounded = 0;
				if (++exponent >= 31) return std::uint16_t(sign | 0x7C00);
			}
		}
		return std::uint16_t(sign | (std::uint32_t(exponent) << 10) | rounded);
	}

	/** Stores one binary16 channel in little-endian output order. */
	void Rgba16fImageDestination::StoreHalf(
		std::vector<std::uint8_t>& output,
		std::size_t pixel,
		std::size_t channel,
		float value)
	{
		const std::uint16_t half = FloatToHalf(value);
		const std::size_t offset = pixel * 8 + channel * 2;
		output[offset] = std::uint8_t(half & 0xFF);
		output[offset + 1] = std::uint8_t(half >> 8);
	}

	/** Resolves four samples per pixel and writes finite normalized little-endian RGBA16F values. */
	bool Rgba16fImageDestination::Resolve(std::vector<std::uint8_t>& output) const
	{
		const std::size_t pixels = std::size_t(grid_.Width) * grid_.Height;
		if (samples_.size() != pixels * SamplesPerPixel || pixels > (std::numeric_limits<std::size_t>::max)() / 8)
		{
			return false;
		}
		output.resize(pixels * 8);
		for (std::size_t pixel = 0; pixel < pixels; ++pixel)
		{
			for (std::size_t channel = 0; channel < 4; ++channel)
			{
				float value = 0.0f;
				for (std::size_t sample = 0; sample < SamplesPerPixel; ++sample)
				{
					value += samples_[pixel * SamplesPerPixel + sample][channel] / float(SamplesPerPixel);
				}
				if (!std::isfinite(value) || value < 0.0f || value > 1.0f) return false;
				StoreHalf(output, pixel, channel, value);
			}
		}
		return true;
	}
}
