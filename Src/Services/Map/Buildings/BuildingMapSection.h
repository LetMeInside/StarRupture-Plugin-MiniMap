// Owns one sparse Building Map cell's prepared numerical scenes, RGBA8 image,
// contexts and immutable Section presentation shared with render snapshots.
#pragma once

#include "Cache/OwnedSpriteKey.h"
#include "Visuals/PreparedBuildingGeometry.h"
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
#include "BuildingMapPerformanceDiagnostics.h"
#endif

#include <cstddef>
#include <cstdint>
#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Services::Map::Buildings::Scheduling
{
	class SectionSchedulingCoordinator;
	class SectionScheduler;
}

namespace Services::Map::Buildings::Visuals
{
	class BuildingVisualsContext;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
	struct SectionFailureDetails;
#endif
}

namespace Services::Map::Buildings
{
	class BuildingMapGpuViewport;
	/** Compact immutable GPU transfer geometry shared by snapshots while this Section lives. */
	struct BuildingMapSectionPresentation final
	{
		/** One prepared source-image sprite triangle, independent of atlas publication. */
		using SpriteTriangle = Visuals::SectionDrawSpriteTriangle;
		/** Maps one occurrence's contiguous triangle range into its current atlas content. */
		struct SpriteBinding
		{
			std::size_t FirstTriangle = 0;
			std::size_t TriangleCount = 0;
			Cache::OwnedSpriteKey SourceKey;
			std::array<std::uint32_t, 4> ContentRectangle{};
		};
		/** One spline triangle with positions, UV correspondence and packed material values. */
		using SplineTriangle = Visuals::SectionDrawSplineTriangle;
		/** Stores the world XY envelope of a range or group, including sticky invalidity. */
		struct SplineWorldBounds
		{
			std::array<double, 2> MinimumWorldXY{
				std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
			std::array<double, 2> MaximumWorldXY{
				-std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
			bool HasFiniteWorldXY = true;

			/** Expands the staging bounds for one copied triangle and disables culling on nonfinite XY. */
			void IncludeTriangleInBounds(const SplineTriangle& triangle) noexcept;
			/** Expands the parent envelope with the child's bounds and propagates invalidity. */
			void IncludeGroupBounds(const SplineWorldBounds& groupBounds) noexcept;
		};

		/** Describes a contiguous interval of a binding range's triangles in the Section draw vector. */
		struct SplineTriangleGroup
		{
			static constexpr std::size_t Capacity = 64;
			std::size_t FirstTriangle = 0;
			std::size_t TriangleCount = 0;
			SplineWorldBounds Bounds;
		};

		/** Owns a binding interval's culling bounds and its ordered child groups. */
		struct SplineBindingRange
		{
			std::size_t FirstTriangle = 0;
			std::size_t TriangleCount = 0;
			Visuals::SplineTextureBindingSlot BindingSlot = Visuals::SplineTextureBindingSlot::Invalid;
#if defined(MINIMAP_EXPERIMENTAL)
			SplineWorldBounds Bounds;
			std::vector<SplineTriangleGroup> Groups;

			/** Extends the last group or opens one for this newly copied triangle and Section offset. */
			void IncludeCopiedTriangle(std::size_t triangleIndex, const SplineTriangle& triangle);
#endif
		};

		/** Shares unchanged world geometry when only atlas content bindings change. */
		struct Geometry final
		{
			std::vector<SpriteTriangle> OrdinaryTriangles;
			std::vector<SplineTriangle> SplineTriangles;
			std::vector<SplineBindingRange> SplineBindingRanges;
			std::vector<SpriteTriangle> OverlayTriangles;

			/** Opens a staging range at the next triangle offset unless the current slot can continue. */
			void OpenSplineBindingRange(Visuals::SplineTextureBindingSlot bindingSlot);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			std::uint64_t SourceGeneration = 0;
			std::size_t OrdinaryOccurrenceCount = 0;
			std::size_t OverlayOccurrenceCount = 0;
			std::size_t SurfaceCount = 0;
			std::shared_ptr<BuildingMapPerformanceDiagnostics::PresentationTiming> Timing;
#endif
		};

		std::shared_ptr<const Geometry> PreparedGeometry;
		std::vector<SpriteBinding> OrdinaryBindings;
		std::vector<SpriteBinding> OverlayBindings;
	};

	/**
	 * Identifies one fixed service-owned map cell and retains its private numerical
	 * rendering context for the Section lifetime. X columns and Y rows increase
	 * rightward and downward; the service remains the Section's lifetime owner.
	 */
	class BuildingMapSection
	{
	public:
		/** Releases Section-owned rendering state when this service-owned Section is destroyed. */
		~BuildingMapSection();

		/** Returns the integer world-grid coordinate on the X axis. */
		std::int64_t GetGridX() const;

		/** Returns the integer world-grid coordinate on the Y axis. */
		std::int64_t GetGridY() const;

		/**
		 * Describes the actual dimensions and borrowed immutable bytes of a stored
		 * completed straight-alpha sRGB RGBA8 Section image. The Section owns the storage; this view
		 * does not extend its lifetime and is usable only under the borrowing
		 * restrictions documented by TryBorrowCompletedImage.
		 */
		struct CompletedImageView
		{
			std::uint32_t PixelWidth = 0;
			std::uint32_t PixelHeight = 0;
			std::span<const std::uint8_t> Rgba8Bytes;
		};

		/** Borrows the selected presentation scene for one serialized synchronous read interval.
		 * A completed scene wins during replacement; otherwise the available preparing
		 * contributions are exposed with IsComplete false until atomic adoption. The scene
		 * and its category vectors remain valid until this callback returns because service,
		 * scheduler and viewport mutations share one calling thread; the borrower must not
		 * reenter those mutation paths or retain the view.
		 */
		struct ScenePresentationView
		{
			const Visuals::PreparedBuildingGeometry* Scene = nullptr;
			bool IsComplete = false;
		};

		/** Borrows the completed scene, or first-appearance contributions while preparing.
		 * The view ends at the next scene mutation or Section destruction and must not be retained.
		 */
		bool TryBorrowPresentationScene(ScenePresentationView& view) const noexcept;

		/**
		 * Borrows immutable metadata and RGBA8 bytes from the completed image.
		 * The borrowed span is valid only while this Section remains alive, its
		 * completed image remains unchanged, and the service's serialized
		 * calling-thread contract is respected. Consume the bytes synchronously;
		 * do not retain the span across image replacement or removal, residency
		 * eviction, service destruction, or any other image-ownership mutation.
		 *
		 * @param image Receives actual pixel dimensions and a const borrowed byte span.
		 *               It is reset to an empty view when no completed image exists.
		 * @return True only when a completed image is stored.
		 */
		bool TryBorrowCompletedImage(CompletedImageView& image) const;

		/** Reports whether a fully completed scene matches the supplied source hash. */
		bool HasCompletedSceneForContentHash(std::uint64_t sourceContentHash) const noexcept;

	private:
		friend class BuildingMapService;
		friend class Scheduling::SectionSchedulingCoordinator;
		friend class Scheduling::SectionScheduler;
		friend class BuildingMapGpuViewport;

		/** Creates a section for the service-owned grid coordinate. */
		BuildingMapSection(std::int64_t gridX, std::int64_t gridY);

		/**
		 * Owns one finalized row-major straight-alpha sRGB RGBA8 image for this
		 * Section. Image presence represents availability; bytes are transferred
		 * by move without inspecting or copying their contents.
		 */
		struct CompletedImage
		{
			std::uint32_t PixelWidth = 0;
			std::uint32_t PixelHeight = 0;
			std::vector<std::uint8_t> Rgba8Bytes;
		};

		/** Reports whether this Section currently owns a completed image. */
		bool HasCompletedImage() const;

		/** Releases only the completed image, preserving this Section and its rendering context. */
		void InvalidateCompletedImage();

		/**
		 * Takes ownership of a successfully finalized RGBA8 image, replacing the
		 * previous completed image without inspecting or copying its contents.
		 *
		 * @param pixelWidth Image width in pixels.
		 * @param pixelHeight Image height in pixels.
		 * @param rgba8Bytes Finalized row-major straight-alpha sRGB RGBA8 bytes to move.
		 */
		void AdoptCompletedImage(
			std::uint32_t pixelWidth,
			std::uint32_t pixelHeight,
			std::vector<std::uint8_t>&& rgba8Bytes);
		/** Starts or resumes an owned replacement scene while preserving CompletedScene. */
		Visuals::PreparedBuildingGeometry* BeginPreparingScene(
			std::uint64_t sourceContentHash,
			std::size_t contributionCount);
		/** Atomically promotes the matching complete PreparingScene, including an empty scene. */
		bool CompletePreparingScene(std::uint64_t sourceContentHash) noexcept;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		/** Emits a single failure record for this Section and the selected source hash, when known. */
		void ReportFailure(
			std::optional<std::uint64_t> sourceContentHash,
			int status,
			const Visuals::SectionFailureDetails& failure) const noexcept;
		/** Reports a withdrawal only when this Section owns previously published nonempty GPU geometry.
		 * @param transition Source or lifecycle transition responsible for the withdrawal.
		 * @param previousSourceHash Source hash before the transition, when known.
		 * @param nextSourceHash Source hash after the transition, absent on source withdrawal.
		 * @param demanded Current aggregate Section demand, when known by the caller.
		 */
		void ReportGpuPresentationWithdrawal(
			const char* transition,
			std::optional<std::uint64_t> previousSourceHash,
			std::optional<std::uint64_t> nextSourceHash,
			std::optional<bool> demanded = std::nullopt) const noexcept;
#endif
		/** Removes only an obsolete replacement, leaving CompletedScene and its image intact. */
		void InvalidatePreparingScene(std::uint64_t currentSourceContentHash) noexcept;
		/** Discards incomplete preparation while preserving CompletedScene and its image.
		 * @param transition Identifies the source or lifecycle transition that caused the discard.
		 * @param demanded Records current viewport demand when the caller can establish it.
		 */
		void DiscardPreparingScene(
			const char* transition = "preparing-scene-discard",
			std::optional<bool> demanded = std::nullopt) noexcept;
		/** Invalidates cached transfer data when selected drawable geometry changes. */
		void MarkGpuPresentationChanged() noexcept;
		/** Consumes the Section-local change signal after its scheduler reports the coordinate. */
		bool ConsumeGpuPresentationChanged() noexcept;
		/** Consumes an immediate geometry-eligibility transition for snapshot membership. */
		bool ConsumeGpuGeometrySnapshotChanged() noexcept;

		/**
		 * Returns this Section's private rendering context, allocating it on the
		 * first explicit request. The returned reference is borrowed and remains
		 * valid only while this Section and its owned context remain alive.
		 *
		 * @return The context exclusively owned by this Section.
		 */
		Visuals::BuildingVisualsContext& GetOrCreateVisualsContext();
		/** Returns scratch exclusively reserved for the independent Section geometry operation. */
		Visuals::BuildingVisualsContext& GetOrCreateGeometryContext();
		/** Releases unfinished scheduler state while preserving completed scene, context and image.
		 * @param transition Identifies the reset cause for transition diagnostics.
		 * @param demanded Records current viewport demand when the caller can establish it.
		 */
		void ResetRenderingState(
			const char* transition = "section-reset",
			std::optional<bool> demanded = std::nullopt) noexcept;

		std::int64_t gridX_;
		std::int64_t gridY_;
		std::unique_ptr<Visuals::BuildingVisualsContext> visualsContext_;
		std::unique_ptr<Visuals::BuildingVisualsContext> geometryContext_;
		/** Opaque scheduler state owned only for this Section's unfinished render generation. */
		std::shared_ptr<void> renderingState_;
		std::optional<CompletedImage> completedImage_;
		/** Immutable presentation scene survives replacement preparation and image changes. */
		std::unique_ptr<const Visuals::PreparedBuildingGeometry> completedScene_;
		/** Mutable Section-owned contributions for the currently selected replacement. */
		std::unique_ptr<Visuals::PreparedBuildingGeometry> preparingScene_;
		std::shared_ptr<const BuildingMapSectionPresentation> gpuPresentation_;
		std::uint64_t gpuPresentationAtlasRevision_ = 0;
		bool gpuPresentationAtlasRevisionValid_ = false;
		bool gpuPresentationChanged_ = false;
		/** Owns one private resumable draw-buffer batch until atomic publication or source invalidation. */
		struct GpuGeometryAssembly
		{
			/** Defines the required ordinary, spline and overlay pass sequence plus atlas binding stages. */
			enum class Phase : std::uint8_t
			{
				OrdinarySprites,
				SplineTriangles,
				OverlaySprites,
				OrdinaryBindings,
				OverlayBindings
			};
			std::unique_ptr<BuildingMapSectionPresentation> Presentation;
			std::shared_ptr<BuildingMapSectionPresentation::Geometry> Geometry;
			const Visuals::PreparedBuildingGeometry* SourceScene = nullptr;
			std::uint64_t SourceContentHash = 0;
			std::size_t PainterOrderIndex = 0;
			std::size_t TriangleIndex = 0;
			std::size_t OrdinaryOccurrenceCount = 0;
			std::size_t OverlayOccurrenceCount = 0;
			std::size_t SurfaceIndex = 0;
			std::size_t SurfaceCount = 0;
			std::size_t SurfaceTriangleIndex = 0;
			std::size_t BindingIndex = 0;
			std::uint64_t BindingAtlasRevision = 0;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			std::uint64_t DiagnosticLastAdvanceTick = 0;
#endif
			Phase CurrentPhase = Phase::OrdinarySprites;
			bool BindingAtlasRevisionValid = false;
		};
		std::unique_ptr<GpuGeometryAssembly> gpuGeometryAssembly_;
		bool gpuGeometryChangedDuringAssembly_ = false;
		bool gpuGeometrySnapshotChanged_ = false;
		std::uint64_t gpuGeometryChangeSequence_ = 0;
	};
}
