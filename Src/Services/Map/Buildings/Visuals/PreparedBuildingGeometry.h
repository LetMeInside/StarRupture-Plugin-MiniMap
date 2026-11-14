// Defines an owned, renderer-independent snapshot of prepared Building Map geometry.
#pragma once

#include "SpriteGeometry.h"
#include "BuildingVisualsResult.h"
#include "../IImageDestination.h"
#include "../Cache/OwnedSpriteKey.h"

#include <cstddef>
#include <cstdint>
#include <array>
#include <optional>
#include <vector>

namespace Services::Map::Buildings::Scheduling
{
	class SectionScheduler;
}

namespace Services::Map::Buildings
{
	class BuildingMapSection;
}

namespace Services::Map::Buildings::Visuals
{
	class BuildingVisuals;
	class BuildingGeometryPreparation;
	/** Holds the final atlas-independent values consumed by the GPU sprite draw path. */
	struct SectionDrawSpriteTriangle
	{
		std::array<std::array<double, 3>, 3> World{};
		std::array<std::array<float, 2>, 3> ProcessedUv{};
	};
	/** Holds the final position and material values consumed by the GPU solid draw path. */
	struct SectionDrawSolidTriangle
	{
		std::array<std::array<double, 3>, 3> World{};
		std::uint32_t Rgb = 0xFFFFFF;
		std::uint8_t Opacity = 255;
	};

	/** Owns incrementally prepared world-space contributions for one Section publication.
	 * BuildingMapSection owns this value while it is preparing and after completion. The
	 * contribution vectors own geometry and keys; they never borrow numerical scratch.
	 */
	class PreparedBuildingGeometry final
	{
		friend class BuildingVisuals;
		friend class BuildingGeometryPreparation;
		friend class ::Services::Map::Buildings::Scheduling::SectionScheduler;
		friend class ::Services::Map::Buildings::BuildingMapSection;

	public:
		/** Owns one Section-clipped sprite occurrence and its original image/occlusion source quad. */
		struct SpriteOccurrence
		{
			std::uint64_t SectionLocalOrdinal = 0;
			/** Index in its final painter-ordered descriptor pass, unset until descriptor assembly. */
			std::optional<std::size_t> SceneIndex;
			std::array<double, 3> PlacementPosition{};
			Cache::OwnedSpriteKey SourceKey;
			/** Original projected quad retained for RGBA8 raster input and full occluder evidence. */
			SpriteWorldQuad SourceWorldQuad;
			/** Fixed maximum: clipping a convex quad by a rectangle yields at most eight vertices/six triangles. */
			std::array<TexturedTriangle, 6> SectionTriangles{};
			std::size_t SectionTriangleCount = 0;
			std::array<SectionDrawSpriteTriangle, 6> DrawTriangles{};
			double OccluderTopHeight = 0.0;
			bool IsOccluder = false;
		};
		/** Distinguishes unresolved inputs from independently drawable and terminal contributors. */
		enum class ContributionStatus : std::uint8_t
		{
			NotProcessed,
			Unresolved,
			Prepared,
			TerminalOmission,
			GeometricOmission,
			Failed
		};

		/** Owns one clipped spline surface's world triangles and numeric appearance metadata. */
		struct SplineSurface
		{
			std::uint64_t SectionLocalOrdinal = 0;
			std::size_t SourceDescriptorIndex = 0;
			std::size_t SurfaceIndex = 0;
			bool IsTextured = false;
			bool SelectedAlternative = false;
			std::uint32_t SolidRgb = 0xFFFFFF;
			std::uint32_t ModulationRgb = 0xFFFFFF;
			std::array<double, 2> UvScale{1.0, 1.0};
			std::array<double, 2> UvOffset{};
			TextureAddressMode AddressU = TextureAddressMode::Repeat;
			TextureAddressMode AddressV = TextureAddressMode::Clamp;
			std::uint8_t Opacity = 255;
			std::vector<SolidTriangle> SolidTriangles;
			std::vector<TexturedTriangle> TexturedTriangles;
			std::vector<SectionDrawSolidTriangle> DrawSolidTriangles;
		};

		/** Reports whether a full scene preparation reached its complete-result boundary. */
		bool IsComplete() const noexcept;
		/** Returns the existing publication content hash associated by BuildingMapSection. */
		std::uint64_t SourceContentHash() const noexcept;
		/** Borrows this generation's original sprite-first ordinal order; valid only while this object lives. */
		const std::vector<std::size_t>& SpritePainterOrder() const noexcept;
		/** Returns the valid-placement sprite prefix length within SpritePainterOrder(). */
		std::size_t ValidSpritePainterCount() const noexcept;
		/** Returns the complete sprite prefix length within SpritePainterOrder(). */
		std::size_t SpritePainterCount() const noexcept;
		/** Borrows ordinary occurrences in append order; use SpritePainterOrder() for drawing order. */
		const std::vector<SpriteOccurrence>& OrdinarySprites() const noexcept;
		/** Resolves one ordinal directly without exposing mutable Section-owned geometry. */
		const SpriteOccurrence* SpriteOccurrenceAtOrdinal(
			std::uint64_t ordinal,
			bool& isOverlayLast,
			std::size_t* vectorIndex = nullptr) const noexcept;
		/** Borrows the clipped spline surfaces in established contributor order. */
		const std::vector<SplineSurface>& SplineSurfaces() const noexcept;
		/** Borrows terminal no-drawable spline ordinals accepted as scene omissions. */
		const std::vector<std::uint64_t>& TerminalSplineOmissionOrdinals() const noexcept;
		/** Reports the representative reason retained for this generation's terminal spline omissions. */
		BuildingSplineOmissionReason TerminalSplineOmissionReason() const noexcept;
		/** Borrows overlay occurrences in append order; use SpritePainterOrder() for drawing order. */
		const std::vector<SpriteOccurrence>& OverlayLastSprites() const noexcept;
		/** Borrows per-publication contribution states without implying whole-scene completion. */
		const std::vector<ContributionStatus>& ContributionStates() const noexcept;

	private:
		/** Directly resolves one publication ordinal into its append-only sprite pass vector. */
		struct SpriteOccurrenceLocation
		{
			std::size_t VectorIndex = 0;
			bool IsOverlayLast = false;
		};

		std::vector<SpriteOccurrence> ordinarySprites_;
		std::vector<SplineSurface> splineSurfaces_;
		std::vector<std::uint64_t> terminalSplineOmissionOrdinals_;
		BuildingSplineOmissionReason terminalSplineOmissionReason_ =
			BuildingSplineOmissionReason::NoDrawableAlternative;
		std::vector<SpriteOccurrence> overlayLastSprites_;
		std::vector<ContributionStatus> contributionStates_;
		std::vector<std::optional<SpriteOccurrenceLocation>> spriteOccurrenceLocationsByOrdinal_;
		/** The scheduler's existing sprite-first order, including unresolved sprites and the non-sprite suffix. */
		std::vector<std::size_t> spritePainterOrder_;
		/** Boundary after valid placed sprites; unresolved sprites follow through spritePainterCount_. */
		std::size_t validSpritePainterCount_ = 0;
		/** Boundary after all sprite ordinals; non-sprite ordinals follow to the vector end. */
		std::size_t spritePainterCount_ = 0;
		std::size_t unfinishedContributionCount_ = 0;
		std::size_t failedContributionCount_ = 0;
		std::size_t terminalOmissionCount_ = 0;
		std::size_t drawableGpuTriangleCount_ = 0;
		std::uint64_t sourceContentHash_ = 0;
		bool complete_ = false;

		/** Initializes exact publication-local occurrence cardinality for a fresh replacement scene. */
		void InitializeContributionStates(std::size_t contributionCount);
		/** Converts one newly clipped sprite's UV values once while preserving the RGBA8 source values. */
		static void PrepareSpriteDrawLayout(SpriteOccurrence& occurrence) noexcept;
		/** Adds final material values to one newly prepared solid spline surface. */
		static void PrepareSolidSurfaceDrawLayout(SplineSurface& surface);
		/** Resolves one ordinal directly; borrowed pointers expire on vector mutation or object destruction. */
		SpriteOccurrence* SpriteOccurrenceAtOrdinal(std::uint64_t ordinal, bool& isOverlayLast) noexcept;
		/** Applies one legal occurrence-state transition without extending publication cardinality. */
		bool TrySetContributionStatus(std::uint64_t ordinal, ContributionStatus status) noexcept;
		/** Records an appended sprite's stable vector index and pass for its publication ordinal. */
		bool TrySetSpriteOccurrenceLocation(
			std::uint64_t ordinal,
			bool isOverlayLast,
			std::size_t vectorIndex) noexcept;
		/** Reports whether every occurrence is terminal and none failed. */
		bool CanComplete() const noexcept;
	};
}
