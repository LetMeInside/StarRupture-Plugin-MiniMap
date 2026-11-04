// Defines compact, game-independent inputs consumed synchronously by building rendering.
// Source spans are borrowed for one call; shared sprite pixels retain their existing resource owner.
#pragma once

#include "SpriteGeometry.h"
#include "SplineGeometry.h"
#include "SplineSurfaceGeometry.h"
#include "../IImageDestination.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Services::Map::Buildings::Visuals
{
	/** Selects one generic solid-color or textured surface appearance. */
	enum class SplineSurfaceAppearanceMode : std::uint8_t
	{
		SolidColor,
		Textured
	};

	/** Describes one neutral solid or textured appearance for a spline surface. */
	struct SplineSurfaceAppearance
	{
		/** Defaults to solid color so texture resources are optional. */
		SplineSurfaceAppearanceMode Mode = SplineSurfaceAppearanceMode::SolidColor;
		/** sRGB 0xRRGGBB value used by the solid path. */
		std::uint32_t SolidRgb = 0xFFFFFF;
		/** Shared immutable RGBA8 source used only by the textured path. */
		Rgba8ImageSource Texture;
		/** Maps canonical distance/lateral UVs into the texture address space. */
		std::array<double, 2> UvScale{1.0, 1.0};
		std::array<double, 2> UvOffset{};
		TextureAddressMode AddressU = TextureAddressMode::Repeat;
		TextureAddressMode AddressV = TextureAddressMode::Clamp;
		/** Optional sRGB 0xRRGGBB modulation applied to a texture. */
		std::uint32_t ModulationRgb = 0xFFFFFF;
		/** Straight opacity shared by solid and textured paths. */
		double Opacity = 1.0;
	};

	/** Holds one immutable RGBA8 sprite source and the neutral values needed to project it into world space. */
	struct BuildingSpriteRenderInput
	{
		/** Shared immutable RGBA8 allocation; copying this handle does not copy pixel bytes. */
		std::shared_ptr<const std::vector<std::uint8_t>> Pixels;
		/** Dimensions of the shared pixel allocation. */
		std::uint32_t PixelWidth = 0;
		std::uint32_t PixelHeight = 0;
		/** Half-open region in Pixels addressed by this sprite. */
		std::array<double, 4> PixelRegion{};
		SpritePlacement Placement{};
		SpriteSourceSize ProjectionSourceSize{};
		SpriteCaptureBounds CaptureBounds{};
		SpriteProjectionRectangle Projection{};
	};

	/** Holds one drawn surface's numerical dimensions and neutral presentation values. */
	struct BuildingSplineSurfaceRenderInput
	{
		SplineSurfaceBounds Bounds{};
		SplineSurfaceCrossSection CrossSection{};
		SplineSurfaceAppearance Appearance{};
	};

	/** Holds one spline contributor's borrowed curve and surface spans for a synchronous call. */
	struct BuildingSplineRenderInput
	{
		std::span<const SplineCurveSegment> Segments;
		SplineSamplingPolicy SamplingPolicy{};
		SplineSurfaceTransform RootTransform{};
		SplineSurfaceTransform ComponentTransform{};
		std::span<const BuildingSplineSurfaceRenderInput> Surfaces;
	};

	/** Borrows caller-owned source spans in deterministic pass order for one synchronous rendering call. */
	struct BuildingRenderSceneInput
	{
		std::span<const BuildingSpriteRenderInput> OrdinarySprites;
		std::span<const BuildingSplineRenderInput> Splines;
		std::span<const BuildingSpriteRenderInput> OverlayLastSprites;
	};
}
