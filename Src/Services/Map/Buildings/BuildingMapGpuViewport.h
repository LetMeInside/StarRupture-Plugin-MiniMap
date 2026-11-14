// Defines the GPU viewport's immutable presentation boundary and portable canvas drawing path.
#pragma once

#include "BuildingMapViewport.h"
#include "BuildingMapSection.h"
#include "Visuals/PreparedBuildingGeometry.h"

#include <cstdint>
#include <atomic>
#include <functional>
#include <array>
#include <chrono>
#include <memory>
#include <vector>

#include "Services/Viewport.h"

namespace Services::Map::Buildings
{
	namespace Cache { class SpriteSourceManager; }
	/** Publishes demanded Section presentation snapshots on the game thread and draws them on the render thread. */
	class BuildingMapGpuViewport final : public BuildingMapViewport
	{
	public:
		/** Copies the caller's current map projection and screen clip for one draw callback. */
		struct CanvasTransform
		{
			std::array<double, 2> WorldOrigin{};
			MiniMap::Services::Vec2 ScreenOrigin{};
			double PixelsPerWorldUnitX = 1.0;
			double PixelsPerWorldUnitY = 1.0;
			double Cosine = 1.0;
			double Sine = 0.0;
			MiniMap::Services::Vec2 ClipMinimum{};
			MiniMap::Services::Vec2 ClipMaximum{};
		};

		/** Receives one demanded Section's borrowed drawable geometry in deterministic grid order.
		 * The geometry reference is valid only during this synchronous callback. The callback must
		 * not retain its address or spans, reenter scheduling, mutate service/session/viewport
		 * state, or destroy the service or viewport. IsComplete is false for first appearance.
		 */
		using DrawableSectionHandler = std::function<void(
			std::int64_t,
			std::int64_t,
			const Visuals::PreparedBuildingGeometry&,
			bool)>;

		/** Allows owned GPU viewports to be destroyed safely through their base pointer. */
		~BuildingMapGpuViewport() override;
		/** Returns read-only access to the service's published shared sprite atlas. */
		const Cache::SpriteSourceManager* GetPublishedSpriteAtlas() const noexcept;
		/** Visits existing demanded Sections without copying their geometry.
		 * Sprite vectors are stored in append order; use SpritePainterOrder() and direct ordinal
		 * lookup to traverse ordinary sprites, then spline surfaces, then overlay-last sprites.
		 */
		void EnumerateDrawableSections(const DrawableSectionHandler& handler) const;
		/**
		 * Draws only the latest immutable game-thread presentation snapshot on the render thread.
		 * The canvas, transform and viewport object must remain valid for this synchronous call;
		 * the method does not retain the canvas or access the service, Sections, atlas or scheduler.
		 * @param canvas Borrowed portable canvas for the current render callback.
		 * @param layer Existing canvas layer to receive drawing commands.
		 * @param transform Current map-to-canvas projection and viewport clip rectangle.
		 */
		void Draw(MiniMap::Services::ICanvas& canvas,
			MiniMap::Services::DrawLayer layer,
			const CanvasTransform& transform) const;
		/** Drops this viewport's snapshot and then closes the shared registration. */
		void Close() override;

	private:
		friend class BuildingMapService;
		/** Atomically published render-thread value paired with the atlas owner lease. */
		struct PresentationSnapshot
		{
			MiniMap::Services::Texture AtlasTexture{};
			MiniMap::Services::TextureSize AtlasSize{};
			std::uint64_t AtlasDrawingRevision = 0;
			std::shared_ptr<const void> AtlasLease;
			std::vector<std::shared_ptr<const BuildingMapSectionPresentation>> Sections;
		};

		/** Creates the specialization using the existing service-owned base registration inputs. */
		BuildingMapGpuViewport(
			BuildingMapService& service,
			IBuildingCollector& buildingCollector,
			std::uint64_t viewportHandle,
			double xMapUnits,
			double yMapUnits,
			double widthMapUnits,
			double heightMapUnits);
		/** Rebuilds only on a queued Section, range or atlas publication transition. */
		void RefreshPresentationSnapshot(Cache::SpriteSourceManager& atlas);
		/** Appends the currently selected Section's immutable presentation if one has been published. */
		void AppendCurrentSection(
			std::shared_ptr<PresentationSnapshot>& next,
			const std::shared_ptr<const PresentationSnapshot>& previous,
			std::int64_t gridX,
			std::int64_t gridY,
			std::size_t& sectionIndex,
			Cache::SpriteSourceManager& atlas,
			MiniMap::Services::Texture atlasTexture,
			MiniMap::Services::TextureSize atlasSize,
			std::uint64_t atlasDrawingRevision,
			const std::shared_ptr<const void>& atlasLease);
		/** Borrows the last immutable Section geometry and rebinds only its owned keys. */
		std::shared_ptr<const BuildingMapSectionPresentation> GetOrBuildSectionPresentation(
			BuildingMapSection& section,
			Cache::SpriteSourceManager& atlas);
		/** Appends one Section while lazily creating a replacement only after observable change. */
		void AppendSnapshotSection(
			std::shared_ptr<PresentationSnapshot>& next,
			const std::shared_ptr<const PresentationSnapshot>& previous,
			const std::shared_ptr<const BuildingMapSectionPresentation>& section,
			std::size_t sectionIndex,
			MiniMap::Services::Texture atlasTexture,
			MiniMap::Services::TextureSize atlasSize,
			std::uint64_t atlasDrawingRevision,
			const std::shared_ptr<const void>& atlasLease) const;
		/** Resolves new atlas rectangles from keys owned by the exact published geometry. */
		void RebindSpriteBindings(
			const std::vector<BuildingMapSectionPresentation::SpriteBinding>& source,
			Cache::SpriteSourceManager& atlas,
			std::vector<BuildingMapSectionPresentation::SpriteBinding>& output) const;
		/** Advances one admitted Section draw-layout assembly until its shared deadline. */
		static bool AdvanceSectionGeometryPublication(
			BuildingMapSection& section,
			const Visuals::PreparedBuildingGeometry& geometry,
			Cache::SpriteSourceManager& atlas,
			std::chrono::steady_clock::time_point deadline);
		/** Starts a fresh private assembly for the selected Section source generation. */
		static void BeginSectionGeometryAssembly(
			BuildingMapSection& section,
			const Visuals::PreparedBuildingGeometry& geometry);
		/** Appends one painter-ordered sprite pass using retained per-contribution draw values. */
		static bool AdvanceSpritePassAssembly(
			BuildingMapSection& section,
			const Visuals::PreparedBuildingGeometry& geometry,
			bool overlayLast,
			std::chrono::steady_clock::time_point deadline);
		/** Appends retained solid spline values in existing surface order. */
		static bool AdvanceSolidSurfaceAssembly(
			BuildingMapSection& section,
			const Visuals::PreparedBuildingGeometry& geometry,
			std::chrono::steady_clock::time_point deadline);
		/** Applies atlas rectangles to staged occurrence bindings without rebuilding geometry. */
		static bool AdvanceBindingAssembly(
			BuildingMapSection& section,
			Cache::SpriteSourceManager& atlas,
			std::chrono::steady_clock::time_point deadline);
		/** Transfers a complete staged value into the Section's immutable presentation owner. */
		static void PublishSectionGeometryAssembly(BuildingMapSection& section);
		/** Applies the current published atlas rectangle to one staged binding. */
		static void UpdateSpriteBindingRectangle(
			BuildingMapSectionPresentation::SpriteBinding& binding,
			Cache::SpriteSourceManager& atlas);
		/** Converts one world-space point using the caller's existing map projection. */
		static MiniMap::Services::Vec2 TransformWorldPoint(
			const std::array<double, 3>& world,
			const CanvasTransform& transform) noexcept;
		/** Draws one sprite category using per-occurrence atlas bindings and stable triangle ranges. */
		void DrawSpriteCategory(
			MiniMap::Services::ICanvas& canvas,
			MiniMap::Services::DrawLayer layer,
			const CanvasTransform& transform,
			MiniMap::Services::Texture texture,
			MiniMap::Services::TextureSize atlasSize,
			const std::vector<std::shared_ptr<const BuildingMapSectionPresentation>>& sections,
			bool overlayLast) const;
		/** Maps processed-image UVs through one bound content rectangle before submission. */
		void DrawSpriteTriangle(
			MiniMap::Services::ICanvas& canvas,
			MiniMap::Services::DrawLayer layer,
			const CanvasTransform& transform,
			MiniMap::Services::Texture texture,
			MiniMap::Services::TextureSize atlasSize,
			const BuildingMapSectionPresentation::SpriteBinding& binding,
			const BuildingMapSectionPresentation::SpriteTriangle& triangle) const;
		/** Draws prepared solid spline surfaces in Section and surface order. */
		void DrawSolidSurfaces(
			MiniMap::Services::ICanvas& canvas,
			MiniMap::Services::DrawLayer layer,
			const CanvasTransform& transform,
			const std::vector<std::shared_ptr<const BuildingMapSectionPresentation>>& sections) const;
		/** Clears renderer snapshots before service invalidation or viewport closure. */
		void ClearPresentationSnapshot() noexcept;

		std::atomic<std::shared_ptr<const PresentationSnapshot>> presentationSnapshot_;
	};
}
