// Defines compact, game-independent inputs consumed synchronously by building rendering.
// Pixel and geometry spans are borrowed; callers retain storage through consumption and receivers do not retain spans.
#pragma once

#include "SpriteGeometry.h"
#include "SplineGeometry.h"
#include "SplineSurfaceGeometry.h"
#include "../IImageDestination.h"

#include <cstdint>
#include <optional>
#include <span>

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
		/** Borrowed immutable RGBA8 source consumed synchronously by the textured path. */
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
		/** Borrowed immutable RGBA8 pixels; producer keeps the payload alive during submission and
		 * consumers must not retain this view after synchronous consumption returns.
		 */
		std::span<const std::uint8_t> Pixels;
		/** Dimensions of the borrowed processed-pixel view. */
		std::uint32_t PixelWidth = 0;
		std::uint32_t PixelHeight = 0;
		/** Half-open region in Pixels addressed by this sprite. */
		std::array<double, 4> PixelRegion{};
		SpritePlacement Placement{};
		SpriteSourceSize ProjectionSourceSize{};
		SpriteCaptureBounds CaptureBounds{};
		SpriteProjectionRectangle Projection{};
	};

	/** Holds one drawn surface's numerical dimensions, presentation values and optional component transform. */
	struct BuildingSplineSurfaceRenderInput
	{
		SplineSurfaceBounds Bounds{};
		SplineSurfaceCrossSection CrossSection{};
		SplineSurfaceAppearance Appearance{};
		/** Optional transform replacing the contributor-wide component transform for this surface. */
		std::optional<SplineSurfaceTransform> ComponentTransformOverride;
	};

	/** Selects whether a spline draws every surface or one first-drawable ordered alternative. */
	enum class BuildingSplineSurfaceSelectionMode : std::uint8_t
	{
		DrawAllSurfaces,
		FirstNumericallyDrawableAlternative
	};

	/** Holds one spline contributor's borrowed curve and surface spans for a synchronous call.
	 * Sampling and root transforms are shared; alternatives use each surface's optional component override.
	 */
	struct BuildingSplineRenderInput
	{
		std::span<const SplineCurveSegment> Segments;
		SplineSamplingPolicy SamplingPolicy{};
		SplineSurfaceTransform RootTransform{};
		SplineSurfaceTransform ComponentTransform{};
		std::span<const BuildingSplineSurfaceRenderInput> Surfaces;
		/** Defaults to generic multi-surface drawing; alternatives retain the supplied span order. */
		BuildingSplineSurfaceSelectionMode SurfaceSelectionMode =
			BuildingSplineSurfaceSelectionMode::DrawAllSurfaces;
	};

	/** Borrows caller-owned source spans in deterministic pass order for one synchronous rendering call. */
	struct BuildingRenderSceneInput
	{
		std::span<const BuildingSpriteRenderInput> OrdinarySprites;
		std::span<const BuildingSplineRenderInput> Splines;
		std::span<const BuildingSpriteRenderInput> OverlayLastSprites;
	};
}
