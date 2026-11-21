// Adapts the game-independent GPU Building Map viewport to the MiniMap UI.
#pragma once

#include "../Services/Map/Buildings/BuildingMapGpuViewport.h"

#if defined(_DEBUG)
#include <array>
#include <cstddef>
#endif
#include <cstdint>
#include <memory>
#include <mutex>

namespace Services::Map::Buildings
{
	class BuildingMapService;
}

namespace MiniMap::BuildingAcquisition
{
	class BuildingCollectorV2;
}

namespace MiniMap::Services
{
	class ICanvas;
}

namespace MiniMapMap
{
	struct Transform;
}

namespace MiniMapUI
{
	/** Owns the MiniMap collector and GPU viewport; canvas calls borrow a shared viewport locally. */
	class BuildingLayer final
	{
	public:
#if defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		/** Lightweight counters for the active GPU UI adapter. */
		struct DiagnosticsSnapshot
		{
			std::uint64_t OpenAttempts = 0;
			std::uint64_t OpenSuccesses = 0;
			std::uint64_t UpdateAttempts = 0;
			std::uint64_t UpdateReturnsTrue = 0;
			std::uint64_t RenderCallbacks = 0;
			std::uint64_t HiddenDraws = 0;
			std::uint64_t InvalidTransforms = 0;
			std::uint64_t ShutdownAttempts = 0;
			std::uint64_t AcquisitionHash = 0;
			std::uint64_t AcquisitionHashChanges = 0;
			bool HasAcquisitionHash = false;
			Services::Map::Buildings::BuildingMapPerformanceDiagnostics::RenderWork Draw;
		};

		/** Copies and clears counters observed since the previous call. */
		bool TakeDiagnosticsSnapshot(DiagnosticsSnapshot& output) noexcept;
#endif

		/** Closes an attached viewport before releasing collector resources. */
		~BuildingLayer();

		/** Prevents copying the collector and viewport ownership. */
		BuildingLayer(const BuildingLayer&) = delete;
		/** Prevents copying the collector and viewport ownership. */
		BuildingLayer& operator=(const BuildingLayer&) = delete;
		/** Keeps callback ownership stable for the layer's lifetime. */
		BuildingLayer(BuildingLayer&&) = delete;
		/** Keeps callback ownership stable for the layer's lifetime. */
		BuildingLayer& operator=(BuildingLayer&&) = delete;

		/** Borrows the gameplay service and creates inert collector state. */
		explicit BuildingLayer(Services::Map::Buildings::BuildingMapService& service);

		/** Initializes collector resources and opens the sole GPU viewport. */
		bool OpenViewport(double xMapUnits, double yMapUnits, double widthMapUnits, double heightMapUnits);

		/** Updates viewport demand and advances scheduling once.
		 * @param xMapUnits Upper-left map X coordinate.
		 * @param yMapUnits Upper-left map Y coordinate.
		 * @param widthMapUnits Positive rectangle width.
		 * @param heightMapUnits Positive rectangle height.
		 * @return True when the viewport update and scheduling operation ran.
		 */
		bool Update(double xMapUnits, double yMapUnits, double widthMapUnits, double heightMapUnits);

		/** Applies the existing game-specific rail collection policy. */
		void SetRenderDroneRails(bool enabled);

		/** Draws current immutable GPU presentation through the MiniMap transform. */
		void Render(MiniMap::Services::ICanvas& canvas, const MiniMapMap::Transform& transform, bool showLayer
#if defined(_DEBUG)
			, bool showSectionGrid
#endif
		);

		/** Closes the GPU viewport before collector shutdown and is safe to repeat. */
		bool Shutdown();

	private:
#if defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		/** Records one callback's visibility outcome and optional draw data under the existing diagnostics lock. */
		void RecordRenderDiagnostics(bool hidden, bool invalidTransform,
			const Services::Map::Buildings::BuildingMapPerformanceDiagnostics::RenderWork* draw) noexcept;
#endif
		/** Copies the viewport owner under the short ownership mutex. */
		std::shared_ptr<Services::Map::Buildings::BuildingMapGpuViewport> AcquireViewport() const;
		/** Converts the existing MiniMap world projection and inset viewport bounds. */
		static bool TryBuildCanvasTransform(
			const MiniMapMap::Transform& mapTransform,
			Services::Map::Buildings::BuildingMapGpuViewport::CanvasTransform& output) noexcept;
#if defined(_DEBUG)
		/** Draws demanded and undemanded Section cells from the actual map grid and viewport bounds. */
		static void DrawSectionGrid(
			MiniMap::Services::ICanvas& canvas, const MiniMapMap::Transform& mapTransform, double sectionSize);
		/** Resolves the signed grid range intersecting the rotated MiniMap bounds. */
		static bool TryGetVisibleSectionGridRange(
			const MiniMapMap::Transform& mapTransform, double sectionSize,
			std::int64_t& firstGridX, std::int64_t& firstGridY,
			std::int64_t& endGridX, std::int64_t& endGridY) noexcept;
		/** Inverts the four visible screen corners through the existing map transform. */
		static bool TryGetVisibleWorldBounds(
			const MiniMapMap::Transform& mapTransform,
			double& minimumX, double& minimumY, double& maximumX, double& maximumY) noexcept;
		/** Draws the top/left cell edges and upright coordinate label. */
		static void DrawSectionGridCell(
			MiniMap::Services::ICanvas& canvas, const MiniMapMap::Transform& mapTransform,
			std::int64_t gridX, std::int64_t gridY, double sectionSize);
		/** Finds an upright label anchor inside the visible portion of a Section cell. */
		static bool TryGetClippedSectionLabelPosition(
			const MiniMapMap::Transform& mapTransform, std::int64_t gridX,
			std::int64_t gridY, double sectionSize, MiniMap::Services::Vec2& output);
		/** Projects a Section's four map corners for viewport clipping. */
		static bool TryProjectSectionCell(
			const MiniMapMap::Transform& mapTransform, std::int64_t gridX,
			std::int64_t gridY, double sectionSize, std::array<MiniMap::Services::Vec2, 8>& output);
		/** Clips one convex Section label polygon against one MiniMap viewport edge. */
		static std::size_t ClipSectionLabelPolygon(
			const std::array<MiniMap::Services::Vec2, 8>& input, std::size_t inputCount,
			std::array<MiniMap::Services::Vec2, 8>& output, int edge,
			MiniMap::Services::Vec2 clipMinimum, MiniMap::Services::Vec2 clipMaximum);
		/** Draws one clipped screen-space section boundary as a one-pixel quad. */
		static void DrawSectionGridLine(
			MiniMap::Services::ICanvas& canvas, const MiniMapMap::Transform& mapTransform,
			double x1, double y1, double x2, double y2);
#endif

		Services::Map::Buildings::BuildingMapService* service_ = nullptr;
		std::unique_ptr<MiniMap::BuildingAcquisition::BuildingCollectorV2> buildingCollector_;
		std::shared_ptr<Services::Map::Buildings::BuildingMapGpuViewport> gpuViewport_;
		mutable std::mutex viewportMutex_;
#if defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		mutable std::mutex diagnosticsMutex_;
		DiagnosticsSnapshot diagnostics_{};
#endif
		bool renderDroneRailsEnabled_ = true;
		bool shutdownRequested_ = false;
#if defined(MODLOADER_CLIENT_BUILD) && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		std::uint64_t lastAcquisitionHash_ = 0;
		bool hasLastAcquisitionHash_ = false;
#endif
	};
}
