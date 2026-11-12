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
		Cache::SpriteSourceManager& atlas,
		bool atlasBindingsChanged)
	{
		const auto previous = presentationSnapshot_.load(std::memory_order_acquire);
		const MiniMap::Services::Texture atlasTexture = atlas.PublishedTexture();
		const MiniMap::Services::TextureSize atlasSize = atlas.PublishedAtlasSize();
		const std::shared_ptr<const void> atlasLease = atlas.PublishedTextureLease();
		std::shared_ptr<PresentationSnapshot> next;
		if (!previous || previous->AtlasTexture.id != atlasTexture.id ||
			previous->AtlasSize.width != atlasSize.width || previous->AtlasSize.height != atlasSize.height ||
			previous->AtlasLease != atlasLease)
		{
			next = std::make_shared<PresentationSnapshot>();
			next->AtlasTexture = atlasTexture;
			next->AtlasSize = atlasSize;
			next->AtlasLease = atlasLease;
		}
		std::size_t sectionIndex = 0;
		EnumerateDrawableSections([&](std::int64_t gridX, std::int64_t gridY,
			const Visuals::PreparedBuildingGeometry& geometry, bool)
		{
			BuildingMapSection* section = service_ != nullptr
				? service_->FindSectionAt(gridX, gridY)
				: nullptr;
			if (section == nullptr)
			{
				return;
			}
			const auto presentation = GetOrBuildSectionPresentation(
				*section, geometry, atlas, atlasBindingsChanged);
			AppendSnapshotSection(next, previous, presentation, sectionIndex,
				atlasTexture, atlasSize, atlasLease);
			++sectionIndex;
		});
		if (!next && previous && sectionIndex != previous->Sections.size())
		{
			next = std::make_shared<PresentationSnapshot>();
			next->AtlasTexture = atlasTexture;
			next->AtlasSize = atlasSize;
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

	/** Reuses Section geometry unless source content, readiness or its atlas binding changed. */
	std::shared_ptr<const BuildingMapSectionPresentation>
	BuildingMapGpuViewport::GetOrBuildSectionPresentation(
		BuildingMapSection& section,
		const Visuals::PreparedBuildingGeometry& geometry,
		Cache::SpriteSourceManager& atlas,
		bool atlasBindingsChanged)
	{
		auto existing = section.gpuPresentation_;
		if (existing == nullptr)
		{
			auto replacement = std::make_shared<BuildingMapSectionPresentation>();
			replacement->PreparedGeometry = BuildSectionGeometry(geometry);
			BuildSpriteBindings(geometry.OrdinarySprites(), atlas, replacement->OrdinaryBindings);
			BuildSpriteBindings(geometry.OverlayLastSprites(), atlas, replacement->OverlayBindings);
			section.gpuPresentationAtlasBindingSignature_ = CalculateAtlasBindingSignature(geometry, atlas);
			section.gpuPresentationAtlasBindingSignatureValid_ = true;
			section.gpuPresentation_ = replacement;
			return replacement;
		}
		if (atlasBindingsChanged || !section.gpuPresentationAtlasBindingSignatureValid_)
		{
			const std::uint64_t bindingSignature = CalculateAtlasBindingSignature(geometry, atlas);
			if (bindingSignature != section.gpuPresentationAtlasBindingSignature_)
			{
				auto replacement = std::make_shared<BuildingMapSectionPresentation>();
				replacement->PreparedGeometry = existing->PreparedGeometry;
				BuildSpriteBindings(geometry.OrdinarySprites(), atlas, replacement->OrdinaryBindings);
				BuildSpriteBindings(geometry.OverlayLastSprites(), atlas, replacement->OverlayBindings);
				section.gpuPresentationAtlasBindingSignature_ = bindingSignature;
				section.gpuPresentation_ = replacement;
				existing = std::move(replacement);
			}
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
		const std::shared_ptr<const void>& atlasLease) const
	{
		if (!next && (sectionIndex >= previous->Sections.size() || previous->Sections[sectionIndex] != section))
		{
			next = std::make_shared<PresentationSnapshot>();
			next->AtlasTexture = atlasTexture;
			next->AtlasSize = atlasSize;
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

	/** Copies already prepared drawable values and leaves unsupported textured materials omitted. */
	std::shared_ptr<const BuildingMapSectionPresentation::Geometry>
	BuildingMapGpuViewport::BuildSectionGeometry(
		const Visuals::PreparedBuildingGeometry& geometry) const
	{
		auto result = std::make_shared<BuildingMapSectionPresentation::Geometry>();
		for (const auto& occurrence : geometry.OrdinarySprites())
		{
			AppendSpriteOccurrence(occurrence, result->OrdinaryTriangles);
		}
		for (const auto& surface : geometry.SplineSurfaces())
		{
			if (!surface.IsTextured)
			{
				for (const SolidTriangle& triangle : surface.SolidTriangles)
				{
					BuildingMapSectionPresentation::SolidTriangle prepared;
					prepared.World = triangle.Vertices;
					prepared.Rgb = surface.SolidRgb;
					prepared.Opacity = surface.Opacity;
					result->SolidSurfaces.push_back(prepared);
				}
			}
		}
		for (const auto& occurrence : geometry.OverlayLastSprites())
		{
			AppendSpriteOccurrence(occurrence, result->OverlayTriangles);
		}
		return result;
	}

	/** Captures only current content rectangles; an unavailable source keeps an empty binding. */
	void BuildingMapGpuViewport::BuildSpriteBindings(
		const std::vector<Visuals::PreparedBuildingGeometry::SpriteOccurrence>& occurrences,
		Cache::SpriteSourceManager& atlas,
		std::vector<BuildingMapSectionPresentation::SpriteBinding>& output) const
	{
		output.clear();
		output.reserve(occurrences.size());
		std::size_t firstTriangle = 0;
		for (const auto& occurrence : occurrences)
		{
			BuildingMapSectionPresentation::SpriteBinding binding;
			binding.FirstTriangle = firstTriangle;
			binding.TriangleCount = occurrence.SectionTriangleCount;
			firstTriangle += occurrence.SectionTriangleCount;
			Cache::SpriteSourceManager::AtlasSpriteEntry entry;
			if (occurrence.SectionTriangleCount != 0 &&
				atlas.TryGetPublishedSprite(occurrence.SourceKey, entry) && entry.DrawingEligible)
			{
				binding.ContentRectangle = {entry.Content.X, entry.Content.Y,
					entry.Content.Width, entry.Content.Height};
			}
			output.push_back(binding);
		}
	}

	/** Hashes canonical keys and exact published content rectangles without allocating per sprite. */
	std::uint64_t BuildingMapGpuViewport::CalculateAtlasBindingSignature(
		const Visuals::PreparedBuildingGeometry& geometry,
		Cache::SpriteSourceManager& atlas) const noexcept
	{
		std::uint64_t signature = 1469598103934665603ull;
		const MiniMap::Services::TextureSize size = atlas.PublishedAtlasSize();
		signature = (signature ^ size.width) * 1099511628211ull;
		signature = (signature ^ size.height) * 1099511628211ull;
		const auto append = [&](const auto& occurrences)
		{
			for (const auto& occurrence : occurrences)
			{
				signature = (signature ^ occurrence.SourceKey.Bytes().size()) * 1099511628211ull;
				for (const unsigned char character : occurrence.SourceKey.Bytes())
				{
					signature = (signature ^ character) * 1099511628211ull;
				}
				Cache::SpriteSourceManager::AtlasSpriteEntry entry;
				const bool available = atlas.TryGetPublishedSprite(occurrence.SourceKey, entry) && entry.DrawingEligible;
				signature = (signature ^ std::uint64_t(available)) * 1099511628211ull;
				if (available)
				{
					const std::array<std::uint32_t, 4> bounds{
						entry.Content.X, entry.Content.Y, entry.Content.Width, entry.Content.Height};
					for (std::uint32_t value : bounds)
					{
						signature = (signature ^ value) * 1099511628211ull;
					}
				}
			}
		};
		append(geometry.OrdinarySprites());
		append(geometry.OverlayLastSprites());
		return signature;
	}

	/** Resolves source-image UVs into the published content rectangle without using gutter pixels. */
	void BuildingMapGpuViewport::AppendSpriteOccurrence(
		const Visuals::PreparedBuildingGeometry::SpriteOccurrence& occurrence,
		std::vector<BuildingMapSectionPresentation::SpriteTriangle>& output) const
	{
		for (std::size_t triangleIndex = 0; triangleIndex < occurrence.SectionTriangleCount; ++triangleIndex)
		{
			const auto& source = occurrence.SectionTriangles[triangleIndex];
			BuildingMapSectionPresentation::SpriteTriangle triangle;
			for (std::size_t vertexIndex = 0; vertexIndex < 3; ++vertexIndex)
			{
				triangle.World[vertexIndex] = source.Vertices[vertexIndex].Position;
				const double u = source.Vertices[vertexIndex].TextureCoordinates[0];
				const double v = source.Vertices[vertexIndex].TextureCoordinates[1];
				triangle.ProcessedUv[vertexIndex] = {static_cast<float>(u), static_cast<float>(v)};
			}
			output.push_back(triangle);
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
