// Defines the Building Map library's game-independent synchronous image and solid/textured geometry destination contract.
#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace Services::Map::Buildings
{
	/** Reports the outcome of one synchronous destination submission. */
	enum class ImageSubmission : std::uint8_t
	{
		Submitted,
		Unavailable,
		InvalidInput,
		Culled,
		Rejected
	};

	/** Borrows immutable RGBA8 source pixels and the half-open pixel region used for sampling.
	 * Pixel storage remains caller-owned and the view is valid only during synchronous consumption.
	 * Producers must keep the underlying payload alive during submission; consumers must not retain
	 * the view after the submission call returns.
	 */
	struct Rgba8ImageSource
	{
		std::span<const std::uint8_t> Pixels;
		std::uint32_t Width = 0;
		std::uint32_t Height = 0;
		std::array<double, 4> PixelRegion{};
	};

	/** Stores four global world-space XY corners in source-corner correspondence order. */
	struct ImageWorldQuad
	{
		std::array<std::array<double, 2>, 4> Corners{};
	};

	/** Supplies immutable image pixels, the sampled region, and global world placement. */
	struct ImageSubmissionCommand
	{
		Rgba8ImageSource Source;
		ImageWorldQuad Geometry;
	};

	/** Stores one global world-space XYZ triangle; destination implementations use XY for 2D output. */
	struct SolidTriangle
	{
		std::array<std::array<double, 3>, 3> Vertices{};
	};

	/** Borrows a contiguous triangle span and its sRGB 0xRRGGBB color plus straight 8-bit opacity. */
	struct SolidTriangleBatch
	{
		std::span<const SolidTriangle> Triangles;
		std::uint32_t Rgb = 0;
		std::uint8_t Opacity = 255;
	};

	/** Selects how normalized texture coordinates address values outside the source region. */
	enum class TextureAddressMode : std::uint8_t
	{
		Clamp,
		Repeat
	};

	/** Stores one global world-space XYZ vertex with its interpolated texture coordinate. */
	struct TexturedTriangleVertex
	{
		/** Global world-space location; two-dimensional destinations rasterize XY. */
		std::array<double, 3> Position{};
		/** Per-vertex source coordinates interpolated barycentrically across the triangle. */
		std::array<double, 2> TextureCoordinates{};
	};

	/** Stores one textured world-space triangle in deterministic vertex order. */
	struct TexturedTriangle
	{
		std::array<TexturedTriangleVertex, 3> Vertices{};
	};

	/** Borrows textured triangles and immutable RGBA8 pixels synchronously. */
	struct TexturedTriangleBatch
	{
		/** Borrowed triangle order consumed synchronously. */
		std::span<const TexturedTriangle> Triangles;
		/** Borrowed immutable RGBA8 pixels, dimensions, and selected half-open region.
		 * Pixel storage remains caller-owned and valid only during synchronous submission.
		 */
		Rgba8ImageSource Source;
		/** Multiplies canonical surface UVs before addressing. */
		std::array<double, 2> UvScale{1.0, 1.0};
		/** Adds to canonical surface UVs before addressing. */
		std::array<double, 2> UvOffset{};
		/** U axis sampling behavior outside the source region. */
		TextureAddressMode AddressU = TextureAddressMode::Repeat;
		/** V axis sampling behavior outside the source region. */
		TextureAddressMode AddressV = TextureAddressMode::Clamp;
		/** Linear-light modulation source supplied as sRGB 0xRRGGBB. */
		std::uint32_t ModulationRgb = 0xFFFFFF;
		/** Straight 8-bit opacity multiplied by sampled texture alpha. */
		std::uint8_t Opacity = 255;
	};

	/** Submits immutable image sources and borrowed solid or textured geometry synchronously. */
	class IImageDestination
	{
	public:
		/** Supports polymorphic destruction through the public destination boundary. */
		virtual ~IImageDestination() = default;

		/** Submits immutable RGBA8 pixels and their global world-space quad synchronously.
		 * Implementations may report unavailable resources, invalid input, clipping, or rejection.
		 * The command and pixel view are borrowed for this call. The producer must keep the payload alive
		 * until the call returns, and the consumer must not retain the view afterward.
		 */
		virtual ImageSubmission SubmitImage(const ImageSubmissionCommand& command) = 0;

		/** Submits a contiguous span of global world-space XYZ triangles synchronously.
		 * RGB is sRGB in 0xRRGGBB form and opacity is straight 8-bit alpha.
		 * The span is borrowed and must not be retained.
		 */
		virtual ImageSubmission SubmitSolidTriangles(const SolidTriangleBatch& batch) = 0;

		/** Submits textured world-space triangles synchronously with per-vertex UVs and borrowed immutable pixels.
		 * UV scale and offset are applied before U/V addressing; RGB modulation is sRGB 0xRRGGBB and opacity is straight alpha.
		 * Triangle and pixel spans are borrowed; producers keep their storage alive through the call and consumers do not retain them.
		 */
		virtual ImageSubmission SubmitTexturedTriangles(const TexturedTriangleBatch& batch) = 0;
	};
}
