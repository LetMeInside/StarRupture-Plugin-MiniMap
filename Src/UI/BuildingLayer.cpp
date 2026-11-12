#if defined(MODLOADER_CLIENT_BUILD)

// Connects the MiniMap's game-specific collector to the shared GPU Building Map presentation.
#include "BuildingLayer.h"

#include "../Map/Buildings/BuildingCollectorV2.h"
#include "../Map/MapTransform.h"
#include "../Services/Map/Buildings/BuildingMapService.h"
#include "../Native/MassLifecycle.h"
#include "Services/Viewport.h"

#include <cmath>
#include <memory>
#include <utility>

namespace MiniMapUI
{
	BuildingLayer::BuildingLayer(Services::Map::Buildings::BuildingMapService& service)
		: service_(&service)
		, buildingCollector_(std::make_unique<MiniMap::BuildingAcquisition::BuildingCollectorV2>())
	{
	}

	BuildingLayer::~BuildingLayer()
	{
		Shutdown();
	}

#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
	bool BuildingLayer::TakeDiagnosticsSnapshot(DiagnosticsSnapshot& output) noexcept
	{
		std::lock_guard<std::mutex> lock(diagnosticsMutex_);
		if (diagnostics_.OpenAttempts == 0 && diagnostics_.UpdateAttempts == 0 &&
			diagnostics_.RenderCallbacks == 0 && diagnostics_.ShutdownAttempts == 0)
		{
			return false;
		}
		output = diagnostics_;
		diagnostics_ = {};
		diagnostics_.AcquisitionHash = lastAcquisitionHash_;
		diagnostics_.HasAcquisitionHash = hasLastAcquisitionHash_;
		return true;
	}
#endif

	bool BuildingLayer::OpenViewport(
		double xMapUnits,
		double yMapUnits,
		double widthMapUnits,
		double heightMapUnits)
	{
	#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		{
			std::lock_guard<std::mutex> lock(diagnosticsMutex_);
			++diagnostics_.OpenAttempts;
		}
	#endif
		if (shutdownRequested_ || service_ == nullptr || buildingCollector_ == nullptr || gpuViewport_ != nullptr ||
			service_->GetMapWidthMapUnits() == 0.0 || service_->GetMapHeightMapUnits() == 0.0 ||
			!buildingCollector_->InitializeResources())
		{
			return false;
		}
		auto viewport = service_->OpenGpuViewport(
			buildingCollector_.get(), xMapUnits, yMapUnits, widthMapUnits, heightMapUnits);
		if (viewport == nullptr)
		{
			return false;
		}
		{
			std::lock_guard<std::mutex> lock(viewportMutex_);
			gpuViewport_ = std::shared_ptr<Services::Map::Buildings::BuildingMapGpuViewport>(std::move(viewport));
		}
	#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		{
			std::lock_guard<std::mutex> lock(diagnosticsMutex_);
			++diagnostics_.OpenSuccesses;
		}
	#endif
		return true;
	}

	bool BuildingLayer::Update(double x1, double y1, double x2, double y2)
	{
	#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		{
			std::lock_guard<std::mutex> lock(diagnosticsMutex_);
			++diagnostics_.UpdateAttempts;
		}
	#endif
		const auto viewport = AcquireViewport();
		if (shutdownRequested_ || viewport == nullptr)
		{
			return false;
		}
	#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		MiniMapMassLifecycle::MassLifecycle::BeginCollectorUpdate();
	#endif
		if (!viewport->Update(x1, y1, x2, y2))
		{
			return false;
		}
	#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		{
			std::lock_guard<std::mutex> lock(diagnosticsMutex_);
			++diagnostics_.UpdateReturnsTrue;
			const std::uint64_t acquisitionHash = buildingCollector_->AcquisitionHash();
			if (hasLastAcquisitionHash_ && acquisitionHash != lastAcquisitionHash_)
			{
				++diagnostics_.AcquisitionHashChanges;
			}
			lastAcquisitionHash_ = acquisitionHash;
			hasLastAcquisitionHash_ = true;
			diagnostics_.AcquisitionHash = acquisitionHash;
			diagnostics_.HasAcquisitionHash = true;
		}
	#endif
		return true;
	}

	void BuildingLayer::SetRenderDroneRails(bool enabled)
	{
		if (renderDroneRailsEnabled_ == enabled)
		{
			return;
		}
		renderDroneRailsEnabled_ = enabled;
		if (buildingCollector_ != nullptr)
		{
			buildingCollector_->SetRenderDroneRails(enabled);
		}
		if (const auto viewport = AcquireViewport())
		{
			viewport->RequestSectionHashRecheck();
		}
	}

	void BuildingLayer::Render(
		MiniMap::Services::ICanvas& canvas,
		const MiniMapMap::Transform& mapTransform,
		bool showLayer)
	{
	#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		{
			std::lock_guard<std::mutex> lock(diagnosticsMutex_);
			++diagnostics_.RenderCallbacks;
			if (!showLayer)
			{
				++diagnostics_.HiddenDraws;
			}
		}
	#endif
		if (!showLayer)
		{
			return;
		}
		Services::Map::Buildings::BuildingMapGpuViewport::CanvasTransform transform{};
		if (!TryBuildCanvasTransform(mapTransform, transform))
		{
		#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
			std::lock_guard<std::mutex> lock(diagnosticsMutex_);
			++diagnostics_.InvalidTransforms;
			#endif
			return;
		}
		const auto viewport = AcquireViewport();
		if (viewport != nullptr)
		{
			viewport->Draw(canvas, MiniMap::Services::DrawLayer::Window, transform);
		}
	}

	bool BuildingLayer::Shutdown()
	{
	#if !MINIMAP_USE_LEGACY_BUILDINGS && MINIMAP_DEVELOPMENT_DIAGNOSTICS
		{
			std::lock_guard<std::mutex> lock(diagnosticsMutex_);
			++diagnostics_.ShutdownAttempts;
		}
	#endif
		shutdownRequested_ = true;
		std::shared_ptr<Services::Map::Buildings::BuildingMapGpuViewport> viewport;
		{
			std::lock_guard<std::mutex> lock(viewportMutex_);
			viewport = std::move(gpuViewport_);
		}
		if (viewport != nullptr)
		{
			viewport->Close();
		}
		service_ = nullptr;
		if (buildingCollector_ != nullptr)
		{
			buildingCollector_->ShutdownResources();
			buildingCollector_.reset();
		}
		return true;
	}

	std::shared_ptr<Services::Map::Buildings::BuildingMapGpuViewport> BuildingLayer::AcquireViewport() const
	{
		std::lock_guard<std::mutex> lock(viewportMutex_);
		return gpuViewport_;
	}

	bool BuildingLayer::TryBuildCanvasTransform(
		const MiniMapMap::Transform& mapTransform,
		Services::Map::Buildings::BuildingMapGpuViewport::CanvasTransform& output) noexcept
	{
		output = {};
		const double scaleY = mapTransform.PixelsPerWorldUnitY > 0.0
			? mapTransform.PixelsPerWorldUnitY
			: mapTransform.PixelsPerWorldUnit;
		if (!mapTransform.Valid || !std::isfinite(mapTransform.TrackingWorldX) ||
			!std::isfinite(mapTransform.TrackingWorldY) || !std::isfinite(mapTransform.CenterX) ||
			!std::isfinite(mapTransform.CenterY) || !std::isfinite(mapTransform.Width) ||
			!std::isfinite(mapTransform.Height) || mapTransform.Width <= 0.0f ||
			mapTransform.Height <= 0.0f || !std::isfinite(mapTransform.PixelsPerWorldUnit) ||
			mapTransform.PixelsPerWorldUnit <= 0.0 || !std::isfinite(scaleY) || scaleY <= 0.0 ||
			!std::isfinite(mapTransform.CosMapRotation) || !std::isfinite(mapTransform.SinMapRotation))
		{
			return false;
		}
		output.WorldOrigin = {mapTransform.TrackingWorldX, mapTransform.TrackingWorldY};
		output.ScreenOrigin = {mapTransform.CenterX, mapTransform.CenterY};
		output.PixelsPerWorldUnitX = mapTransform.PixelsPerWorldUnit;
		output.PixelsPerWorldUnitY = scaleY;
		output.Cosine = mapTransform.CosMapRotation;
		output.Sine = mapTransform.SinMapRotation;
		output.ClipMinimum = {
			mapTransform.CenterX - mapTransform.Width * 0.5f,
			mapTransform.CenterY - mapTransform.Height * 0.5f};
		output.ClipMaximum = {
			mapTransform.CenterX + mapTransform.Width * 0.5f,
			mapTransform.CenterY + mapTransform.Height * 0.5f};
		return true;
	}
}
#endif
