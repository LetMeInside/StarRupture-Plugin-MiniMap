#if defined(MODLOADER_CLIENT_BUILD)

// Implements game-thread presentation publication and render-thread canvas submission.
#include "BuildingMapGpuViewport.h"
#include "BuildingMapService.h"
#include "Cache/SpriteSourceManager.h"

#include <atomic>
#include <algorithm>
#include <cmath>

namespace Services::Map::Buildings
{
	/** Clears the renderer-owned transfer snapshot before base destruction closes registration. */
	BuildingMapGpuViewport::~BuildingMapGpuViewport()
	{
		ClearPresentationSnapshot();
	}

	/** Borrows the service-owned manager without transferring texture or registry ownership. */
	const Cache::SpriteSourceManager* BuildingMapGpuViewport::GetPublishedSpriteAtlas() const noexcept
	{
		return service_ != nullptr ? service_->PublishedSpriteAtlas() : nullptr;
	}

	/** Delegates deterministic borrowed reads to the service that owns every Section. */
	void BuildingMapGpuViewport::EnumerateDrawableSections(const DrawableSectionHandler& handler) const
	{
		if (service_ != nullptr && handler)
		{
			service_->EnumerateDrawableSections(*this, handler);
		}
	}

	/** Drops the immutable snapshot before closing the game-thread service registration. */
	void BuildingMapGpuViewport::Close()
	{
		ClearPresentationSnapshot();
		BuildingMapViewport::Close();
	}

	/** Reuses unchanged Section transfer data and publishes one complete immutable render value. */
	void BuildingMapGpuViewport::RefreshPresentationSnapshot(
		Cache::SpriteSourceManager& atlas)
	{
		const auto previous = presentationSnapshot_.load(std::memory_order_acquire);
		const MiniMap::Services::Texture atlasTexture = atlas.PublishedTexture();
		const MiniMap::Services::TextureSize atlasSize = atlas.PublishedAtlasSize();
		const std::uint64_t atlasDrawingRevision = atlas.PublishedDrawingRevision();
		const std::shared_ptr<const void> atlasLease = atlas.PublishedTextureLease();
		std::shared_ptr<PresentationSnapshot> next;
		if (!previous || previous->AtlasTexture.id != atlasTexture.id ||
			previous->AtlasSize.width != atlasSize.width || previous->AtlasSize.height != atlasSize.height ||
			previous->AtlasDrawingRevision != atlasDrawingRevision ||
			previous->AtlasLease != atlasLease)
		{
			next = std::make_shared<PresentationSnapshot>();
			next->AtlasTexture = atlasTexture;
			next->AtlasSize = atlasSize;
			next->AtlasDrawingRevision = atlasDrawingRevision;
			next->AtlasLease = atlasLease;
		}
		std::size_t sectionIndex = 0;
		EnumerateDrawableSections([&](std::int64_t gridX, std::int64_t gridY,
			const Visuals::PreparedBuildingGeometry&, bool)
		{
			AppendCurrentSection(next, previous, gridX, gridY, sectionIndex,
				atlas, atlasTexture, atlasSize, atlasDrawingRevision, atlasLease);
		});
		if (!next && previous && sectionIndex != previous->Sections.size())
		{
			next = std::make_shared<PresentationSnapshot>();
			next->AtlasTexture = atlasTexture;
			next->AtlasSize = atlasSize;
			next->AtlasDrawingRevision = atlasDrawingRevision;
			next->AtlasLease = atlasLease;
			next->Sections = previous->Sections;
			next->Sections.resize(sectionIndex);
		}
		if (next)
		{
			presentationSnapshot_.store(
				std::shared_ptr<const PresentationSnapshot>(std::move(next)), std::memory_order_release);
		}
	}

	/** Resolves one enumerated Section and appends its already published presentation. */
	void BuildingMapGpuViewport::AppendCurrentSection(
		std::shared_ptr<PresentationSnapshot>& next,
		const std::shared_ptr<const PresentationSnapshot>& previous,
		std::int64_t gridX,
		std::int64_t gridY,
		std::size_t& sectionIndex,
		Cache::SpriteSourceManager& atlas,
		MiniMap::Services::Texture atlasTexture,
		MiniMap::Services::TextureSize atlasSize,
		std::uint64_t atlasDrawingRevision,
		const std::shared_ptr<const void>& atlasLease)
	{
		BuildingMapSection* section = service_ != nullptr
			? service_->FindSectionAt(gridX, gridY)
			: nullptr;
		if (section == nullptr)
		{
			return;
		}
		const auto presentation = GetOrBuildSectionPresentation(*section, atlas);
		if (presentation == nullptr)
		{
			return;
		}
		AppendSnapshotSection(next, previous, presentation, sectionIndex,
			atlasTexture, atlasSize, atlasDrawingRevision, atlasLease);
		++sectionIndex;
	}

	/** Reuses immutable Section geometry and rebuilds only bindings for a new atlas revision. */
	std::shared_ptr<const BuildingMapSectionPresentation>
	BuildingMapGpuViewport::GetOrBuildSectionPresentation(
		BuildingMapSection& section,
		Cache::SpriteSourceManager& atlas)
	{
		const std::uint64_t atlasDrawingRevision = atlas.PublishedDrawingRevision();
		auto existing = section.gpuPresentation_;
		if (existing == nullptr)
		{
			return nullptr;
		}
		if (!section.gpuPresentationAtlasRevisionValid_ ||
			atlasDrawingRevision != section.gpuPresentationAtlasRevision_)
		{
			auto replacement = std::make_shared<BuildingMapSectionPresentation>();
			replacement->PreparedGeometry = existing->PreparedGeometry;
			RebindSpriteBindings(existing->OrdinaryBindings, atlas, replacement->OrdinaryBindings);
			RebindSpriteBindings(existing->OverlayBindings, atlas, replacement->OverlayBindings);
			section.gpuPresentationAtlasRevision_ = atlasDrawingRevision;
			section.gpuPresentationAtlasRevisionValid_ = true;
			section.gpuPresentation_ = replacement;
			existing = std::move(replacement);
		}
		return existing;
	}

	/** Retains the old snapshot prefix until one Section or atlas value requires replacement. */
	void BuildingMapGpuViewport::AppendSnapshotSection(
		std::shared_ptr<PresentationSnapshot>& next,
		const std::shared_ptr<const PresentationSnapshot>& previous,
		const std::shared_ptr<const BuildingMapSectionPresentation>& section,
		std::size_t sectionIndex,
		MiniMap::Services::Texture atlasTexture,
		MiniMap::Services::TextureSize atlasSize,
		std::uint64_t atlasDrawingRevision,
		const std::shared_ptr<const void>& atlasLease) const
	{
		if (!next && (sectionIndex >= previous->Sections.size() || previous->Sections[sectionIndex] != section))
		{
			next = std::make_shared<PresentationSnapshot>();
			next->AtlasTexture = atlasTexture;
			next->AtlasSize = atlasSize;
			next->AtlasDrawingRevision = atlasDrawingRevision;
			next->AtlasLease = atlasLease;
			const std::size_t prefixCount = (std::min)(sectionIndex, previous->Sections.size());
			next->Sections.insert(next->Sections.end(), previous->Sections.begin(),
				previous->Sections.begin() + static_cast<std::ptrdiff_t>(prefixCount));
		}
		if (next)
		{
			next->Sections.push_back(section);
		}
	}

	/** Rebinds keys retained with the published geometry and keeps each triangle range unchanged. */
	void BuildingMapGpuViewport::RebindSpriteBindings(
		const std::vector<BuildingMapSectionPresentation::SpriteBinding>& source,
		Cache::SpriteSourceManager& atlas,
		std::vector<BuildingMapSectionPresentation::SpriteBinding>& output) const
	{
		output.clear();
		output.reserve(source.size());
		for (const auto& sourceBinding : source)
		{
			BuildingMapSectionPresentation::SpriteBinding binding = sourceBinding;
			binding.ContentRectangle = {};
			Cache::SpriteSourceManager::AtlasSpriteEntry entry;
			if (binding.TriangleCount != 0 &&
				atlas.TryGetPublishedSprite(binding.SourceKey, entry) && entry.DrawingEligible)
			{
				binding.ContentRectangle = {entry.Content.X, entry.Content.Y,
					entry.Content.Width, entry.Content.Height};
			}
			output.push_back(binding);
		}
	}

	/** Resumes one Section's ordered draw layout and transfers it only when every pass is sealed. */
	bool BuildingMapGpuViewport::AdvanceSectionGeometryPublication(
		BuildingMapSection& section,
		const Visuals::PreparedBuildingGeometry& geometry,
		Cache::SpriteSourceManager& atlas,
		std::chrono::steady_clock::time_point deadline)
	{
		auto& assembly = section.gpuGeometryAssembly_;
		if (assembly == nullptr || assembly->SourceScene != &geometry ||
			assembly->SourceContentHash != geometry.SourceContentHash())
		{
			BeginSectionGeometryAssembly(section, geometry);
		}
		while (std::chrono::steady_clock::now() < deadline)
		{
			if (assembly->CurrentPhase == BuildingMapSection::GpuGeometryAssembly::Phase::OrdinarySprites)
			{
				if (!AdvanceSpritePassAssembly(section, geometry, false, deadline))
				{
					return false;
				}
				assembly->CurrentPhase = BuildingMapSection::GpuGeometryAssembly::Phase::SolidSurfaces;
			}
			else if (assembly->CurrentPhase == BuildingMapSection::GpuGeometryAssembly::Phase::SolidSurfaces)
			{
				if (!AdvanceSolidSurfaceAssembly(section, geometry, deadline))
				{
					return false;
				}
				assembly->CurrentPhase = BuildingMapSection::GpuGeometryAssembly::Phase::OverlaySprites;
				assembly->PainterOrderIndex = 0;
			}
			else if (assembly->CurrentPhase == BuildingMapSection::GpuGeometryAssembly::Phase::OverlaySprites)
			{
				if (!AdvanceSpritePassAssembly(section, geometry, true, deadline))
				{
					return false;
				}
				assembly->CurrentPhase = BuildingMapSection::GpuGeometryAssembly::Phase::OrdinaryBindings;
				assembly->BindingIndex = 0;
			}
			else if (AdvanceBindingAssembly(section, atlas, deadline))
			{
				PublishSectionGeometryAssembly(section);
				return true;
			}
			else
			{
				return false;
			}
		}
		return false;
	}

	/** Captures an append-only input boundary and allocates this generation's private staging owners. */
	void BuildingMapGpuViewport::BeginSectionGeometryAssembly(
		BuildingMapSection& section,
		const Visuals::PreparedBuildingGeometry& geometry)
	{
		auto& assembly = section.gpuGeometryAssembly_;
		assembly = std::make_unique<BuildingMapSection::GpuGeometryAssembly>();
		assembly->Presentation = std::make_unique<BuildingMapSectionPresentation>();
		assembly->Geometry = std::make_shared<BuildingMapSectionPresentation::Geometry>();
		assembly->SourceScene = &geometry;
		assembly->SourceContentHash = geometry.SourceContentHash();
		assembly->OrdinaryOccurrenceCount = geometry.OrdinarySprites().size();
		assembly->OverlayOccurrenceCount = geometry.OverlayLastSprites().size();
		assembly->SurfaceCount = geometry.SplineSurfaces().size();
		section.gpuGeometryChangedDuringAssembly_ = false;
	}

	/** Publishes sealed vectors by ownership transfer and preserves old snapshot owners. */
	void BuildingMapGpuViewport::PublishSectionGeometryAssembly(BuildingMapSection& section)
	{
		auto& assembly = section.gpuGeometryAssembly_;
		assembly->Presentation->PreparedGeometry = std::move(assembly->Geometry);
		if (assembly->Presentation->PreparedGeometry->OrdinaryTriangles.empty() &&
			assembly->Presentation->PreparedGeometry->SolidSurfaces.empty() &&
			assembly->Presentation->PreparedGeometry->OverlayTriangles.empty())
		{
			section.gpuPresentation_.reset();
			section.gpuPresentationAtlasRevisionValid_ = false;
		}
		else
		{
			section.gpuPresentation_ = std::shared_ptr<const BuildingMapSectionPresentation>(
				std::move(assembly->Presentation));
			section.gpuPresentationAtlasRevision_ = assembly->BindingAtlasRevision;
			section.gpuPresentationAtlasRevisionValid_ = true;
		}
		section.gpuGeometryAssembly_.reset();
	}

	/** Appends one painter-ordered sprite pass without converting retained triangles again. */
	bool BuildingMapGpuViewport::AdvanceSpritePassAssembly(
		BuildingMapSection& section,
		const Visuals::PreparedBuildingGeometry& geometry,
		bool overlayLast,
		std::chrono::steady_clock::time_point deadline)
	{
		auto& assembly = *section.gpuGeometryAssembly_;
		const auto& painterOrder = geometry.SpritePainterOrder();
		auto& triangles = overlayLast ? assembly.Geometry->OverlayTriangles : assembly.Geometry->OrdinaryTriangles;
		auto& bindings = overlayLast
			? assembly.Presentation->OverlayBindings : assembly.Presentation->OrdinaryBindings;
		while (assembly.PainterOrderIndex < geometry.SpritePainterCount() &&
			std::chrono::steady_clock::now() < deadline)
		{
			bool occurrenceIsOverlay = false;
			std::size_t vectorIndex = 0;
			const auto* occurrence = geometry.SpriteOccurrenceAtOrdinal(
				painterOrder[assembly.PainterOrderIndex], occurrenceIsOverlay, &vectorIndex);
			const std::size_t occurrenceLimit = overlayLast
				? assembly.OverlayOccurrenceCount : assembly.OrdinaryOccurrenceCount;
			if (occurrence == nullptr || occurrenceIsOverlay != overlayLast || vectorIndex >= occurrenceLimit)
			{
				++assembly.PainterOrderIndex;
				assembly.TriangleIndex = 0;
				continue;
			}
			while (assembly.TriangleIndex < occurrence->SectionTriangleCount &&
				std::chrono::steady_clock::now() < deadline)
			{
				triangles.push_back(occurrence->DrawTriangles[assembly.TriangleIndex]);
				++assembly.TriangleIndex;
			}
			if (assembly.TriangleIndex != occurrence->SectionTriangleCount)
			{
				return false;
			}
			BuildingMapSectionPresentation::SpriteBinding binding;
			binding.FirstTriangle = triangles.size() - occurrence->SectionTriangleCount;
			binding.TriangleCount = occurrence->SectionTriangleCount;
			binding.SourceKey = occurrence->SourceKey;
			bindings.push_back(std::move(binding));
			++assembly.PainterOrderIndex;
			assembly.TriangleIndex = 0;
		}
		return assembly.PainterOrderIndex == geometry.SpritePainterCount();
	}

	/** Appends converted solid spline values in their established surface and triangle order. */
	bool BuildingMapGpuViewport::AdvanceSolidSurfaceAssembly(
		BuildingMapSection& section,
		const Visuals::PreparedBuildingGeometry& geometry,
		std::chrono::steady_clock::time_point deadline)
	{
		auto& assembly = *section.gpuGeometryAssembly_;
		const auto& surfaces = geometry.SplineSurfaces();
		while (assembly.SurfaceIndex < assembly.SurfaceCount && std::chrono::steady_clock::now() < deadline)
		{
			const auto& surface = surfaces[assembly.SurfaceIndex];
			const auto& triangles = surface.DrawSolidTriangles;
			while (assembly.SurfaceTriangleIndex < triangles.size() &&
				std::chrono::steady_clock::now() < deadline)
			{
				assembly.Geometry->SolidSurfaces.push_back(triangles[assembly.SurfaceTriangleIndex]);
				++assembly.SurfaceTriangleIndex;
			}
			if (assembly.SurfaceTriangleIndex != triangles.size())
			{
				return false;
			}
			++assembly.SurfaceIndex;
			assembly.SurfaceTriangleIndex = 0;
		}
		return assembly.SurfaceIndex == assembly.SurfaceCount;
	}

	/** Resumes binding lookup by source key and restarts only this stage on atlas revision changes. */
	bool BuildingMapGpuViewport::AdvanceBindingAssembly(
		BuildingMapSection& section,
		Cache::SpriteSourceManager& atlas,
		std::chrono::steady_clock::time_point deadline)
	{
		auto& assembly = *section.gpuGeometryAssembly_;
		const std::uint64_t drawingRevision = atlas.PublishedDrawingRevision();
		if (!assembly.BindingAtlasRevisionValid || assembly.BindingAtlasRevision != drawingRevision)
		{
			assembly.BindingAtlasRevision = drawingRevision;
			assembly.BindingAtlasRevisionValid = true;
			assembly.CurrentPhase = BuildingMapSection::GpuGeometryAssembly::Phase::OrdinaryBindings;
			assembly.BindingIndex = 0;
		}
		while (std::chrono::steady_clock::now() < deadline)
		{
			const bool overlayLast = assembly.CurrentPhase ==
				BuildingMapSection::GpuGeometryAssembly::Phase::OverlayBindings;
			auto& bindings = overlayLast
				? assembly.Presentation->OverlayBindings : assembly.Presentation->OrdinaryBindings;
			if (assembly.BindingIndex == bindings.size())
			{
				if (overlayLast)
				{
					return atlas.PublishedDrawingRevision() == assembly.BindingAtlasRevision;
				}
				assembly.CurrentPhase = BuildingMapSection::GpuGeometryAssembly::Phase::OverlayBindings;
				assembly.BindingIndex = 0;
				continue;
			}
			UpdateSpriteBindingRectangle(bindings[assembly.BindingIndex], atlas);
			++assembly.BindingIndex;
		}
		return false;
	}

	/** Refreshes one staged binding's rectangle while retaining its source key and triangle range. */
	void BuildingMapGpuViewport::UpdateSpriteBindingRectangle(
		BuildingMapSectionPresentation::SpriteBinding& binding,
		Cache::SpriteSourceManager& atlas)
	{
		binding.ContentRectangle = {};
		Cache::SpriteSourceManager::AtlasSpriteEntry entry;
		if (binding.TriangleCount != 0 &&
			atlas.TryGetPublishedSprite(binding.SourceKey, entry) && entry.DrawingEligible)
		{
			binding.ContentRectangle = {entry.Content.X, entry.Content.Y,
				entry.Content.Width, entry.Content.Height};
		}
	}

	/** Draws all categories from immutable snapshot data without touching service or atlas state. */
	void BuildingMapGpuViewport::Draw(
		MiniMap::Services::ICanvas& canvas,
		MiniMap::Services::DrawLayer layer,
		const CanvasTransform& transform) const
	{
		const auto snapshot = presentationSnapshot_.load(std::memory_order_acquire);
		if (!snapshot)
		{
			return;
		}
		canvas.PushClip(layer, transform.ClipMinimum, transform.ClipMaximum, true);
		DrawSpriteCategory(canvas, layer, transform, snapshot->AtlasTexture,
			snapshot->AtlasSize, snapshot->Sections, false);
		DrawSolidSurfaces(canvas, layer, transform, snapshot->Sections);
		DrawSpriteCategory(canvas, layer, transform, snapshot->AtlasTexture,
			snapshot->AtlasSize, snapshot->Sections, true);
		canvas.PopClip(layer);
	}

	/** Applies the map's rotation, upper-left origin and independent screen-axis scales. */
	MiniMap::Services::Vec2 BuildingMapGpuViewport::TransformWorldPoint(
		const std::array<double, 3>& world,
		const CanvasTransform& transform) noexcept
	{
		const double dx = world[0] - transform.WorldOrigin[0];
		const double dy = world[1] - transform.WorldOrigin[1];
		const double rotatedX = transform.Cosine * dx - transform.Sine * dy;
		const double rotatedY = transform.Sine * dx + transform.Cosine * dy;
		return {static_cast<float>(transform.ScreenOrigin.x + rotatedX * transform.PixelsPerWorldUnitX),
			static_cast<float>(transform.ScreenOrigin.y + rotatedY * transform.PixelsPerWorldUnitY)};
	}

	/** Draws one sprite category while retaining deterministic Section and triangle order. */
	void BuildingMapGpuViewport::DrawSpriteCategory(
		MiniMap::Services::ICanvas& canvas,
		MiniMap::Services::DrawLayer layer,
		const CanvasTransform& transform,
		MiniMap::Services::Texture texture,
		MiniMap::Services::TextureSize atlasSize,
		const std::vector<std::shared_ptr<const BuildingMapSectionPresentation>>& sections,
		bool overlayLast) const
	{
		if (texture.id == 0)
		{
			return;
		}
		for (const auto& section : sections)
		{
			const auto& geometry = *section->PreparedGeometry;
			const auto& bindings = overlayLast ? section->OverlayBindings : section->OrdinaryBindings;
			const auto& triangles = overlayLast ? geometry.OverlayTriangles : geometry.OrdinaryTriangles;
			for (const auto& binding : bindings)
			{
				if (binding.ContentRectangle[2] == 0 || binding.ContentRectangle[3] == 0)
				{
					continue;
				}
				for (std::size_t triangleIndex = 0; triangleIndex < binding.TriangleCount; ++triangleIndex)
				{
					const auto& triangle = triangles[binding.FirstTriangle + triangleIndex];
					DrawSpriteTriangle(canvas, layer, transform, texture, atlasSize,
						binding, triangle);
				}
			}
		}
	}

	/** Transforms prepared positions and processed UVs into the snapshot's matching atlas. */
	void BuildingMapGpuViewport::DrawSpriteTriangle(
		MiniMap::Services::ICanvas& canvas,
		MiniMap::Services::DrawLayer layer,
		const CanvasTransform& transform,
		MiniMap::Services::Texture texture,
		MiniMap::Services::TextureSize atlasSize,
		const BuildingMapSectionPresentation::SpriteBinding& binding,
		const BuildingMapSectionPresentation::SpriteTriangle& triangle) const
	{
		if (atlasSize.width == 0 || atlasSize.height == 0)
		{
			return;
		}
		std::array<MiniMap::Services::Vec2, 3> positions{};
		std::array<MiniMap::Services::Vec2, 3> uvs{};
		for (std::size_t index = 0; index < positions.size(); ++index)
		{
			positions[index] = TransformWorldPoint(triangle.World[index], transform);
			uvs[index] = {
				static_cast<float>((binding.ContentRectangle[0] +
					triangle.ProcessedUv[index][0] * binding.ContentRectangle[2]) / atlasSize.width),
				static_cast<float>((binding.ContentRectangle[1] +
					triangle.ProcessedUv[index][1] * binding.ContentRectangle[3]) / atlasSize.height)};
		}
		canvas.TexturedTriangle(layer, texture, positions, uvs, 0xFFFFFFFFu);
	}

	/** Draws solid prepared spline triangles with authored RGB and opacity. */
	void BuildingMapGpuViewport::DrawSolidSurfaces(
		MiniMap::Services::ICanvas& canvas,
		MiniMap::Services::DrawLayer layer,
		const CanvasTransform& transform,
		const std::vector<std::shared_ptr<const BuildingMapSectionPresentation>>& sections) const
	{
		if (!std::isfinite(transform.ClipMinimum.x) || !std::isfinite(transform.ClipMinimum.y) ||
			!std::isfinite(transform.ClipMaximum.x) || !std::isfinite(transform.ClipMaximum.y) ||
			transform.ClipMinimum.x >= transform.ClipMaximum.x ||
			transform.ClipMinimum.y >= transform.ClipMaximum.y)
		{
			return;
		}
		for (const auto& section : sections)
		{
			for (const BuildingMapSectionPresentation::SolidTriangle& triangle : section->PreparedGeometry->SolidSurfaces)
			{
				std::array<MiniMap::Services::Vec2, 3> positions{};
				bool finite = true;
				for (std::size_t index = 0; index < positions.size(); ++index)
				{
					positions[index] = TransformWorldPoint(triangle.World[index], transform);
					finite = finite && std::isfinite(positions[index].x) && std::isfinite(positions[index].y);
				}
				if (!finite)
				{
					continue;
				}
				const double area =
					(static_cast<double>(positions[1].x) - positions[0].x) *
					(static_cast<double>(positions[2].y) - positions[0].y) -
					(static_cast<double>(positions[1].y) - positions[0].y) *
					(static_cast<double>(positions[2].x) - positions[0].x);
				if (!std::isfinite(area) || area == 0.0)
				{
					continue;
				}
				const float minimumX = (std::min)({positions[0].x, positions[1].x, positions[2].x});
				const float maximumX = (std::max)({positions[0].x, positions[1].x, positions[2].x});
				const float minimumY = (std::min)({positions[0].y, positions[1].y, positions[2].y});
				const float maximumY = (std::max)({positions[0].y, positions[1].y, positions[2].y});
				if (maximumX <= transform.ClipMinimum.x || minimumX >= transform.ClipMaximum.x ||
					maximumY <= transform.ClipMinimum.y || minimumY >= transform.ClipMaximum.y)
				{
					continue;
				}
				if (area < 0.0)
				{
					std::swap(positions[1], positions[2]);
				}
				const std::uint32_t color = ((std::uint32_t(triangle.Opacity) << 24) |
					((triangle.Rgb & 0xFFu) << 16) | (triangle.Rgb & 0x00FF00u) |
					((triangle.Rgb >> 16) & 0xFFu));
				canvas.FilledTriangle(layer, positions, color);
			}
		}
	}

	/** Atomically removes the published render-thread owner without waiting for an active draw. */
	void BuildingMapGpuViewport::ClearPresentationSnapshot() noexcept
	{
		presentationSnapshot_.store({}, std::memory_order_release);
	}

	/** Delegates all registration state and behavior to the existing base viewport. */
	BuildingMapGpuViewport::BuildingMapGpuViewport(
		BuildingMapService& service,
		IBuildingCollector& buildingCollector,
		std::uint64_t viewportHandle,
		double xMapUnits,
		double yMapUnits,
		double widthMapUnits,
		double heightMapUnits)
		: BuildingMapViewport(
			service,
			buildingCollector,
			viewportHandle,
			xMapUnits,
			yMapUnits,
			widthMapUnits,
			heightMapUnits,
			false)
	{
	}
}

#endif // MODLOADER_CLIENT_BUILD
