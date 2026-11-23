#if defined(MODLOADER_CLIENT_BUILD)

// Connects the MiniMap's game-specific collector to the shared GPU Building Map presentation.
#include "BuildingLayer.h"

#include "../Map/Buildings/BuildingCollectorV2.h"
#include "../Map/MapTransform.h"
#include "../Services/Map/Buildings/BuildingMapService.h"
#include "../Native/MassLifecycle.h"
#include "Services/Viewport.h"

#if defined(_DEBUG)
#include <algorithm>
#endif
#include <cmath>
#include <memory>
#include <utility>
#if defined(_DEBUG)
#include <array>
#include <cstdio>
#include <limits>
#endif

namespace MiniMapUI
{
	BuildingLayer::BuildingLayer(Services::Map::Buildings::BuildingMapService& service)
		: service_(&service)
		, buildingCollector_(std::make_unique<MiniMap::BuildingAcquisition::BuildingCollectorV2>(
			service.GetSectionSizeMapUnits()))
	{
	}

	BuildingLayer::~BuildingLayer()
	{
		Shutdown();
	}

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
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

	/** Merges completed callback-local measurements into the layer's existing report interval. */
	void BuildingLayer::RecordRenderDiagnostics(bool hidden, bool invalidTransform,
		const Services::Map::Buildings::BuildingMapPerformanceDiagnostics::RenderWork* draw) noexcept
	{
		std::lock_guard<std::mutex> lock(diagnosticsMutex_);
		++diagnostics_.RenderCallbacks;
		diagnostics_.HiddenDraws += hidden ? 1 : 0;
		diagnostics_.InvalidTransforms += invalidTransform ? 1 : 0;
		if (draw != nullptr)
		{
			diagnostics_.Draw.Merge(*draw);
		}
	}
#endif

	bool BuildingLayer::OpenViewport(
		double xMapUnits,
		double yMapUnits,
		double widthMapUnits,
		double heightMapUnits)
	{
	#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
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
	#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		{
			std::lock_guard<std::mutex> lock(diagnosticsMutex_);
			++diagnostics_.OpenSuccesses;
		}
	#endif
		return true;
	}

	bool BuildingLayer::Update(
		double xMapUnits,
		double yMapUnits,
		double widthMapUnits,
		double heightMapUnits)
	{
	#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
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
		if (!viewport->Update(xMapUnits, yMapUnits, widthMapUnits, heightMapUnits, false))
		{
			return false;
		}
	#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
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
		bool showLayer
#if defined(_DEBUG)
		, bool showSectionGrid
#endif
	)
	{
		if (!showLayer)
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			RecordRenderDiagnostics(true, false, nullptr);
#endif
#if defined(_DEBUG)
			if (showSectionGrid && service_ != nullptr)
			{
				DrawSectionGrid(canvas, mapTransform, service_->GetSectionSizeMapUnits());
			}
#endif
			return;
		}
		Services::Map::Buildings::BuildingMapGpuViewport::CanvasTransform transform{};
		if (!TryBuildCanvasTransform(mapTransform, transform))
		{
		#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			RecordRenderDiagnostics(false, true, nullptr);
			#endif
			return;
		}
		const auto viewport = AcquireViewport();
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		Services::Map::Buildings::BuildingMapPerformanceDiagnostics::RenderWork draw;
#endif
		if (viewport != nullptr)
		{
			viewport->Draw(canvas, MiniMap::Services::DrawLayer::Window, transform
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
				, &draw
#endif
				);
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		RecordRenderDiagnostics(false, false, &draw);
#endif
#if defined(_DEBUG)
		if (showSectionGrid && service_ != nullptr)
		{
			DrawSectionGrid(canvas, mapTransform, service_->GetSectionSizeMapUnits());
		}
#endif
	}

#if defined(_DEBUG)
	void BuildingLayer::DrawSectionGrid(
		MiniMap::Services::ICanvas& canvas,
		const MiniMapMap::Transform& mapTransform,
		double sectionSize)
	{
		std::int64_t firstGridX = 0;
		std::int64_t firstGridY = 0;
		std::int64_t endGridX = 0;
		std::int64_t endGridY = 0;
		if (!TryGetVisibleSectionGridRange(mapTransform, sectionSize,
			firstGridX, firstGridY, endGridX, endGridY))
		{
			return;
		}
		const MiniMap::Services::Vec2 clipMinimum{
			mapTransform.CenterX - mapTransform.Width * 0.5f,
			mapTransform.CenterY - mapTransform.Height * 0.5f};
		const MiniMap::Services::Vec2 clipMaximum{
			mapTransform.CenterX + mapTransform.Width * 0.5f,
			mapTransform.CenterY + mapTransform.Height * 0.5f};
		canvas.PushClip(MiniMap::Services::DrawLayer::Window, clipMinimum, clipMaximum, true);
		for (std::int64_t gridY = firstGridY; gridY < endGridY; ++gridY)
		{
			for (std::int64_t gridX = firstGridX; gridX < endGridX; ++gridX)
			{
				DrawSectionGridCell(canvas, mapTransform, gridX, gridY, sectionSize);
			}
		}
		canvas.PopClip(MiniMap::Services::DrawLayer::Window);
	}

	bool BuildingLayer::TryGetVisibleSectionGridRange(
		const MiniMapMap::Transform& mapTransform,
		double sectionSize,
		std::int64_t& firstGridX,
		std::int64_t& firstGridY,
		std::int64_t& endGridX,
		std::int64_t& endGridY) noexcept
	{
		double minimumX = 0.0;
		double minimumY = 0.0;
		double maximumX = 0.0;
		double maximumY = 0.0;
		if (!std::isfinite(sectionSize) || sectionSize <= 0.0 ||
			!TryGetVisibleWorldBounds(mapTransform, minimumX, minimumY, maximumX, maximumY))
		{
			return false;
		}
		const long double firstX = std::floor(static_cast<long double>(minimumX) / sectionSize);
		const long double firstY = std::floor(static_cast<long double>(minimumY) / sectionSize);
		const long double endX = std::ceil(static_cast<long double>(maximumX) / sectionSize);
		const long double endY = std::ceil(static_cast<long double>(maximumY) / sectionSize);
		const long double minimumGrid = (std::numeric_limits<std::int64_t>::min)();
		const long double maximumGrid = (std::numeric_limits<std::int64_t>::max)();
		if (firstX < minimumGrid || firstY < minimumGrid || endX > maximumGrid || endY > maximumGrid)
		{
			return false;
		}
		firstGridX = static_cast<std::int64_t>(firstX);
		firstGridY = static_cast<std::int64_t>(firstY);
		endGridX = static_cast<std::int64_t>(endX);
		endGridY = static_cast<std::int64_t>(endY);
		return firstGridX < endGridX && firstGridY < endGridY;
	}

	bool BuildingLayer::TryGetVisibleWorldBounds(
		const MiniMapMap::Transform& mapTransform,
		double& minimumX,
		double& minimumY,
		double& maximumX,
		double& maximumY) noexcept
	{
		const double scaleY = mapTransform.PixelsPerWorldUnitY > 0.0
			? mapTransform.PixelsPerWorldUnitY : mapTransform.PixelsPerWorldUnit;
		if (!mapTransform.Valid || !std::isfinite(mapTransform.PixelsPerWorldUnit) ||
			mapTransform.PixelsPerWorldUnit <= 0.0 || !std::isfinite(scaleY) || scaleY <= 0.0)
		{
			return false;
		}
		minimumX = (std::numeric_limits<double>::max)();
		minimumY = minimumX;
		maximumX = -(std::numeric_limits<double>::max)();
		maximumY = maximumX;
		for (int corner = 0; corner < 4; ++corner)
		{
			const double screenX = mapTransform.CenterX + ((corner & 1) ? 0.5 : -0.5) * mapTransform.Width;
			const double screenY = mapTransform.CenterY + ((corner & 2) ? 0.5 : -0.5) * mapTransform.Height;
			const double rotatedX = (screenX - mapTransform.CenterX) / mapTransform.PixelsPerWorldUnit;
			const double rotatedY = (screenY - mapTransform.CenterY) / scaleY;
			const double worldX = mapTransform.TrackingWorldX +
				mapTransform.CosMapRotation * rotatedX + mapTransform.SinMapRotation * rotatedY;
			const double worldY = mapTransform.TrackingWorldY -
				mapTransform.SinMapRotation * rotatedX + mapTransform.CosMapRotation * rotatedY;
			if (!std::isfinite(worldX) || !std::isfinite(worldY))
			{
				return false;
			}
			minimumX = (std::min)(minimumX, worldX);
			minimumY = (std::min)(minimumY, worldY);
			maximumX = (std::max)(maximumX, worldX);
			maximumY = (std::max)(maximumY, worldY);
		}
		return true;
	}

	void BuildingLayer::DrawSectionGridCell(
		MiniMap::Services::ICanvas& canvas,
		const MiniMapMap::Transform& mapTransform,
		std::int64_t gridX,
		std::int64_t gridY,
		double sectionSize)
	{
		constexpr MiniMap::Services::Color gridColor = 0x80BEBEBEu;
		const double left = static_cast<double>(gridX) * sectionSize;
		const double top = static_cast<double>(gridY) * sectionSize;
		DrawSectionGridLine(canvas, mapTransform, left, top, left + sectionSize, top);
		DrawSectionGridLine(canvas, mapTransform, left, top, left, top + sectionSize);
		MiniMap::Services::Vec2 labelPosition{};
		if (!TryGetClippedSectionLabelPosition(mapTransform, gridX, gridY, sectionSize, labelPosition))
		{
			return;
		}
		char label[64]{};
		std::snprintf(label, sizeof(label), "(%lld, %lld)",
			static_cast<long long>(gridX), static_cast<long long>(gridY));
		const MiniMap::Services::Vec2 clipMaximum{
			mapTransform.CenterX + mapTransform.Width * 0.5f,
			mapTransform.CenterY + mapTransform.Height * 0.5f};
		const MiniMap::Services::Vec2 textSize = canvas.MeasureText(label, false, -1.0f);
		labelPosition.x = (std::min)(labelPosition.x, clipMaximum.x - textSize.x - 2.0f);
		labelPosition.y = (std::min)(labelPosition.y, clipMaximum.y - textSize.y - 2.0f);
		canvas.Text(MiniMap::Services::DrawLayer::Window, labelPosition, gridColor, label);
	}

	bool BuildingLayer::TryGetClippedSectionLabelPosition(
		const MiniMapMap::Transform& mapTransform,
		std::int64_t gridX,
		std::int64_t gridY,
		double sectionSize,
		MiniMap::Services::Vec2& output)
	{
		std::array<MiniMap::Services::Vec2, 8> polygon{};
		if (!TryProjectSectionCell(mapTransform, gridX, gridY, sectionSize, polygon))
			return false;
		const MiniMap::Services::Vec2 originalTopLeft = polygon[0];
		const MiniMap::Services::Vec2 minimum{
			mapTransform.CenterX - mapTransform.Width * 0.5f,
			mapTransform.CenterY - mapTransform.Height * 0.5f};
		const MiniMap::Services::Vec2 maximum{
			mapTransform.CenterX + mapTransform.Width * 0.5f,
			mapTransform.CenterY + mapTransform.Height * 0.5f};
		std::array<MiniMap::Services::Vec2, 8> clipped{};
		std::size_t count = 4;
		for (int edge = 0; edge < 4; ++edge)
		{
			count = ClipSectionLabelPolygon(polygon, count, clipped, edge, minimum, maximum);
			polygon.swap(clipped);
		}
		if (count == 0)
		{
			return false;
		}
		std::size_t nearest = 0;
		float nearestDistance = std::numeric_limits<float>::max();
		for (std::size_t index = 0; index < count; ++index)
		{
			const float dx = polygon[index].x - originalTopLeft.x;
			const float dy = polygon[index].y - originalTopLeft.y;
			const float distance = std::hypot(dx, dy);
			if (distance < nearestDistance)
			{
				nearest = index;
				nearestDistance = distance;
			}
		}
		output = {polygon[nearest].x + 3.0f, polygon[nearest].y + 3.0f};
		return true;
	}

	bool BuildingLayer::TryProjectSectionCell(
		const MiniMapMap::Transform& mapTransform,
		std::int64_t gridX,
		std::int64_t gridY,
		double sectionSize,
		std::array<MiniMap::Services::Vec2, 8>& output)
	{
		const double left = static_cast<double>(gridX) * sectionSize;
		const double top = static_cast<double>(gridY) * sectionSize;
		const std::array<std::array<double, 2>, 4> worldCorners{{
			{left, top}, {left + sectionSize, top},
			{left + sectionSize, top + sectionSize}, {left, top + sectionSize}}};
		for (std::size_t index = 0; index < worldCorners.size(); ++index)
		{
			MiniMapMap::ScreenPoint point{};
			if (!mapTransform.WorldToScreen(worldCorners[index][0], worldCorners[index][1], point))
				return false;
			output[index] = {point.X, point.Y};
		}
		return true;
	}

	std::size_t BuildingLayer::ClipSectionLabelPolygon(
		const std::array<MiniMap::Services::Vec2, 8>& input,
		std::size_t inputCount,
		std::array<MiniMap::Services::Vec2, 8>& output,
		int edge,
		MiniMap::Services::Vec2 clipMinimum,
		MiniMap::Services::Vec2 clipMaximum)
	{
		if (inputCount == 0)
		{
			return 0;
		}
		const bool vertical = edge < 2;
		const float boundary = edge == 0 ? clipMinimum.x : edge == 1 ? clipMaximum.x :
			edge == 2 ? clipMinimum.y : clipMaximum.y;
		const auto inside = [vertical, edge, boundary](MiniMap::Services::Vec2 point)
		{
			const float coordinate = vertical ? point.x : point.y;
			return edge == 0 || edge == 2 ? coordinate >= boundary : coordinate <= boundary;
		};
		const auto intersection = [vertical, boundary](MiniMap::Services::Vec2 first, MiniMap::Services::Vec2 second)
		{
			const float firstCoordinate = vertical ? first.x : first.y;
			const float secondCoordinate = vertical ? second.x : second.y;
			const float amount = (boundary - firstCoordinate) / (secondCoordinate - firstCoordinate);
			return vertical ? MiniMap::Services::Vec2{boundary, first.y + amount * (second.y - first.y)}
				: MiniMap::Services::Vec2{first.x + amount * (second.x - first.x), boundary};
		};
		std::size_t outputCount = 0;
		MiniMap::Services::Vec2 previous = input[inputCount - 1];
		bool previousInside = inside(previous);
		for (std::size_t index = 0; index < inputCount; ++index)
		{
			const MiniMap::Services::Vec2 current = input[index];
			const bool currentInside = inside(current);
			if (currentInside != previousInside)
			{
				output[outputCount++] = intersection(previous, current);
			}
			if (currentInside)
			{
				output[outputCount++] = current;
			}
			previous = current;
			previousInside = currentInside;
		}
		return outputCount;
	}

	void BuildingLayer::DrawSectionGridLine(
		MiniMap::Services::ICanvas& canvas,
		const MiniMapMap::Transform& mapTransform,
		double x1,
		double y1,
		double x2,
		double y2)
	{
		MiniMapMap::ScreenPoint first{};
		MiniMapMap::ScreenPoint second{};
		if (!mapTransform.WorldToScreen(x1, y1, first) || !mapTransform.WorldToScreen(x2, y2, second))
		{
			return;
		}
		const float dx = second.X - first.X;
		const float dy = second.Y - first.Y;
		const float length = std::hypot(dx, dy);
		if (!std::isfinite(length) || length <= 0.0f)
		{
			return;
		}
		constexpr float halfWidth = 0.5f;
		constexpr MiniMap::Services::Color gridColor = 0x80BEBEBEu;
		const float offsetX = -dy / length * halfWidth;
		const float offsetY = dx / length * halfWidth;
		const MiniMap::Services::Vec2 a{first.X + offsetX, first.Y + offsetY};
		const MiniMap::Services::Vec2 b{second.X + offsetX, second.Y + offsetY};
		const MiniMap::Services::Vec2 c{second.X - offsetX, second.Y - offsetY};
		const MiniMap::Services::Vec2 d{first.X - offsetX, first.Y - offsetY};
		canvas.FilledTriangle(MiniMap::Services::DrawLayer::Window, {a, b, c}, gridColor);
		canvas.FilledTriangle(MiniMap::Services::DrawLayer::Window, {a, c, d}, gridColor);
	}
#endif

	bool BuildingLayer::Shutdown()
	{
	#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
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
