// Defines an owned, renderer-independent snapshot of prepared Building Map geometry.
#pragma once

#include "SpriteGeometry.h"
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
			/** Index in this occurrence's painter-sorted pass, unset until that correspondence is finalized. */
			std::optional<std::size_t> SceneIndex;
			std::array<double, 3> PlacementPosition{};
			Cache::OwnedSpriteKey SourceKey;
			/** Original projected quad retained for RGBA8 raster input and full occluder evidence. */
			SpriteWorldQuad SourceWorldQuad;
			/** Fixed maximum: clipping a convex quad by a rectangle yields at most eight vertices/six triangles. */
			std::array<TexturedTriangle, 6> SectionTriangles{};
			std::size_t SectionTriangleCount = 0;
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
		};

		/** Reports whether a full scene preparation reached its complete-result boundary. */
		bool IsComplete() const noexcept;
		/** Returns the existing publication content hash associated by BuildingMapSection. */
		std::uint64_t SourceContentHash() const noexcept;
		/** Borrows the ordinary sprite occurrences in stable source order. */
		const std::vector<SpriteOccurrence>& OrdinarySprites() const noexcept;
		/** Borrows the clipped spline surfaces in established contributor order. */
		const std::vector<SplineSurface>& SplineSurfaces() const noexcept;
		/** Borrows terminal no-drawable spline ordinals accepted as scene omissions. */
		const std::vector<std::uint64_t>& TerminalSplineOmissionOrdinals() const noexcept;
		/** Borrows overlay-last sprite occurrences in stable source order. */
		const std::vector<SpriteOccurrence>& OverlayLastSprites() const noexcept;
		/** Borrows per-publication contribution states without implying whole-scene completion. */
		const std::vector<ContributionStatus>& ContributionStates() const noexcept;

	private:
		std::vector<SpriteOccurrence> ordinarySprites_;
		std::vector<SplineSurface> splineSurfaces_;
		std::vector<std::uint64_t> terminalSplineOmissionOrdinals_;
		std::vector<SpriteOccurrence> overlayLastSprites_;
		std::vector<ContributionStatus> contributionStates_;
		std::size_t unfinishedContributionCount_ = 0;
		std::size_t failedContributionCount_ = 0;
		std::size_t terminalOmissionCount_ = 0;
		std::uint64_t sourceContentHash_ = 0;
		bool complete_ = false;

		/** Initializes exact publication-local occurrence cardinality for a fresh replacement scene. */
		void InitializeContributionStates(std::size_t contributionCount);
		/** Applies one legal occurrence-state transition without extending publication cardinality. */
		bool TrySetContributionStatus(std::uint64_t ordinal, ContributionStatus status) noexcept;
		/** Reports whether every occurrence is terminal and none failed. */
		bool CanComplete() const noexcept;
	};
}
