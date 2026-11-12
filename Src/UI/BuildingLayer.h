// Adapts the game-independent GPU Building Map viewport to the MiniMap UI.
#pragma once

#include "../Services/Map/Buildings/BuildingMapGpuViewport.h"

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
#if defined(MODLOADER_CLIENT_BUILD) && !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		/** Lightweight counters for the active GPU UI adapter. */
		struct DiagnosticsSnapshot
		{
			std::uint64_t OpenAttempts = 0;
			std::uint64_t OpenSuccesses = 0;
			std::uint64_t UpdateAttempts = 0;
			std::uint64_t UpdateSuccesses = 0;
			std::uint64_t RenderCallbacks = 0;
			std::uint64_t HiddenDraws = 0;
			std::uint64_t InvalidTransforms = 0;
			std::uint64_t ShutdownAttempts = 0;
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

		/** Updates viewport demand and advances the service scheduling path once. */
		bool Update(double x1, double y1, double x2, double y2);

		/** Applies the existing game-specific rail collection policy. */
		void SetRenderDroneRails(bool enabled);

		/** Draws current immutable GPU presentation through the MiniMap transform. */
		void Render(MiniMap::Services::ICanvas& canvas, const MiniMapMap::Transform& transform, bool showLayer);

		/** Closes the GPU viewport before collector shutdown and is safe to repeat. */
		bool Shutdown();

	private:
		/** Copies the viewport owner under the short ownership mutex. */
		std::shared_ptr<Services::Map::Buildings::BuildingMapGpuViewport> AcquireViewport() const;
		/** Converts the existing MiniMap world projection and inset viewport bounds. */
		static bool TryBuildCanvasTransform(
			const MiniMapMap::Transform& mapTransform,
			Services::Map::Buildings::BuildingMapGpuViewport::CanvasTransform& output) noexcept;

		Services::Map::Buildings::BuildingMapService* service_ = nullptr;
		std::unique_ptr<MiniMap::BuildingAcquisition::BuildingCollectorV2> buildingCollector_;
		std::shared_ptr<Services::Map::Buildings::BuildingMapGpuViewport> gpuViewport_;
		mutable std::mutex viewportMutex_;
#if defined(MODLOADER_CLIENT_BUILD) && !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		mutable std::mutex diagnosticsMutex_;
		DiagnosticsSnapshot diagnostics_{};
#endif
		bool renderDroneRailsEnabled_ = true;
		bool shutdownRequested_ = false;
	};
}
