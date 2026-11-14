// Owns one sparse Building Map cell's prepared numerical scenes, RGBA8 image,
// contexts and immutable Section presentation shared with render snapshots.
#pragma once

#include "Cache/OwnedSpriteKey.h"
#include "Visuals/PreparedBuildingGeometry.h"

#include <cstddef>
#include <cstdint>
#include <array>
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
		/** One solid spline triangle with the only material fields used by Draw. */
		using SolidTriangle = Visuals::SectionDrawSolidTriangle;

		/** Shares unchanged world geometry when only atlas content bindings change. */
		struct Geometry final
		{
			std::vector<SpriteTriangle> OrdinaryTriangles;
			std::vector<SolidTriangle> SolidSurfaces;
			std::vector<SpriteTriangle> OverlayTriangles;
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

		/** Returns true when the section has a currently certified membership observation. */
		bool HasCurrentMembershipObservation() const;

		/**
		 * Copies the current certified fingerprint to the output when available.
		 * Returns false without changing the output when membership is uncertified.
		 *
		 * @param fingerprint Receives the valid fingerprint, including a possible zero value.
		 * @return True only while a complete observation is currently certified.
		 */
		bool TryGetCurrentMembershipFingerprint(std::uint64_t& fingerprint) const;

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

		/**
		 * Records a complete fingerprint or revokes current certification when incomplete.
		 *
		 * @param isComplete Whether the collector certified complete source coverage.
		 * @param membershipFingerprint Fingerprint to retain when isComplete is true.
		 * @return True for the first complete fingerprint or a change from the previous one.
		 */
		bool RecordMembershipObservation(bool isComplete, std::uint64_t membershipFingerprint);

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
#endif
		/** Removes only an obsolete replacement, leaving CompletedScene and its image intact. */
		void InvalidatePreparingScene(std::uint64_t currentSourceContentHash) noexcept;
		/** Discards an abandoned incomplete replacement while preserving CompletedScene and the image. */
		void DiscardPreparingScene() noexcept;
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
		/** Releases unfinished scheduler state and replacement scene while preserving the completed scene, context and image. */
		void ResetRenderingState() noexcept;

		std::int64_t gridX_;
		std::int64_t gridY_;
		bool hasCurrentMembershipObservation_ = false;
		bool hasPreviousMembershipFingerprint_ = false;
		std::uint64_t membershipFingerprint_ = 0;
		std::unique_ptr<Visuals::BuildingVisualsContext> visualsContext_;
		std::unique_ptr<Visuals::BuildingVisualsContext> geometryContext_;
		/** Opaque scheduler state owned only for this Section's unfinished render generation. */
		std::shared_ptr<void> renderingState_;
		std::optional<CompletedImage> completedImage_;
		/** Immutable presentation scene survives replacement preparation and image changes. */
		std::unique_ptr<const Visuals::PreparedBuildingGeometry> completedScene_;
		/** Mutable Section-owned contributions for the currently selected replacement. */
		std::unique_ptr<Visuals::PreparedBuildingGeometry> preparingScene_;
		std::uint64_t preparingSceneSourceContentHash_ = 0;
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
				SolidSurfaces,
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
			Phase CurrentPhase = Phase::OrdinarySprites;
			bool BindingAtlasRevisionValid = false;
		};
		std::unique_ptr<GpuGeometryAssembly> gpuGeometryAssembly_;
		bool gpuGeometryChangedDuringAssembly_ = false;
		bool gpuGeometrySnapshotChanged_ = false;
	};
}
