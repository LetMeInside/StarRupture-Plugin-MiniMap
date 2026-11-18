#if defined(MODLOADER_CLIENT_BUILD)

// Implements game-thread presentation publication and render-thread canvas submission.
#include "BuildingMapGpuViewport.h"
#include "BuildingMapService.h"
#include "Cache/SpriteSourceManager.h"
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
#include "../../Log.h"
#endif

#include <atomic>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Services::Map::Buildings
{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
	using Performance = BuildingMapPerformanceDiagnostics;
#endif
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
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		Performance::Timer refresh(&Performance::Game().Snapshot);
#endif
		const auto previous = presentationSnapshot_.load(std::memory_order_acquire);
		const MiniMap::Services::Texture atlasTexture = atlas.PublishedTexture();
		const MiniMap::Services::TextureSize atlasSize = atlas.PublishedAtlasSize();
		const std::uint64_t atlasDrawingRevision = atlas.PublishedDrawingRevision();
		const std::shared_ptr<const void> atlasLease = atlas.PublishedTextureLease();
		const MiniMap::Services::Texture splineWhiteTexture = atlas.SolidWhiteTexture();
		const std::shared_ptr<const void> splineWhiteTextureLease = atlas.SolidWhiteTextureLease();
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		if (previous != nullptr && previous->AtlasTexture.id != 0 &&
			(atlasTexture.id == 0 || atlasSize.width == 0 || atlasSize.height == 0) &&
			!previous->Sections.empty())
		{
			if (auto* log = MiniMap::Services::TryLog())
			{
				MiniMap::Services::Logf(*log, MiniMap::Services::Level::Warn,
					"MiniMap: BuildingMapPresentationTrace event=atlas-texture-withdrawal viewport=%llu "
					"previousTexture=%llu nextTexture=%llu nextSize=%ux%u "
					"previousRevision=%llu nextRevision=%llu sections=%llu",
					static_cast<unsigned long long>(viewportHandle_),
					static_cast<unsigned long long>(previous->AtlasTexture.id),
					static_cast<unsigned long long>(atlasTexture.id), atlasSize.width, atlasSize.height,
					static_cast<unsigned long long>(previous->AtlasDrawingRevision),
					static_cast<unsigned long long>(atlasDrawingRevision),
					static_cast<unsigned long long>(previous->Sections.size()));
			}
		}
#endif
		std::shared_ptr<PresentationSnapshot> next;
		if (!previous || previous->AtlasTexture.id != atlasTexture.id ||
			previous->AtlasSize.width != atlasSize.width || previous->AtlasSize.height != atlasSize.height ||
			previous->AtlasDrawingRevision != atlasDrawingRevision ||
			previous->AtlasLease != atlasLease ||
			previous->SplineWhiteTexture.id != splineWhiteTexture.id ||
			previous->SplineWhiteTextureLease != splineWhiteTextureLease)
		{
			next = std::make_shared<PresentationSnapshot>();
			next->AtlasTexture = atlasTexture;
			next->AtlasSize = atlasSize;
			next->AtlasDrawingRevision = atlasDrawingRevision;
			next->AtlasLease = atlasLease;
			next->SplineWhiteTexture = splineWhiteTexture;
			next->SplineWhiteTextureLease = splineWhiteTextureLease;
		}
		std::size_t sectionIndex = 0;
		EnumerateDrawableSections([&](std::int64_t gridX, std::int64_t gridY,
			const Visuals::PreparedBuildingGeometry&, bool)
		{
			AppendCurrentSection(next, previous, gridX, gridY, sectionIndex,
				atlas, atlasTexture, atlasSize, atlasDrawingRevision, atlasLease,
				splineWhiteTexture, splineWhiteTextureLease);
		});
		if (!next && previous && sectionIndex != previous->Sections.size())
		{
			next = std::make_shared<PresentationSnapshot>();
			next->AtlasTexture = atlasTexture;
			next->AtlasSize = atlasSize;
			next->AtlasDrawingRevision = atlasDrawingRevision;
			next->AtlasLease = atlasLease;
			next->SplineWhiteTexture = splineWhiteTexture;
			next->SplineWhiteTextureLease = splineWhiteTextureLease;
			next->Sections = previous->Sections;
			next->Sections.resize(sectionIndex);
		}
		if (next)
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			RecordSnapshotAdoption(*next);
#endif
			presentationSnapshot_.store(
				std::shared_ptr<const PresentationSnapshot>(std::move(next)), std::memory_order_release);
		}
	}

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
	/** Stamps the imminent release store for newly adopted sampled draw buffers. */
	void BuildingMapGpuViewport::RecordSnapshotAdoption(const PresentationSnapshot& snapshot) noexcept
	{
		auto& work = Performance::Game();
		++work.SnapshotReplacements;
		work.SnapshotHandles += snapshot.Sections.size();
		for (const auto& section : snapshot.Sections)
		{
			const auto& timing = section->PreparedGeometry->Timing;
			if (timing != nullptr && timing->Adopted == Performance::Clock::time_point{})
			{
				timing->Adopted = Performance::Clock::now();
			}
		}
	}
#endif

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
		const std::shared_ptr<const void>& atlasLease,
		MiniMap::Services::Texture splineWhiteTexture,
		const std::shared_ptr<const void>& splineWhiteTextureLease)
	{
		BuildingMapSection* section = service_ != nullptr
			? service_->FindSectionAt(gridX, gridY)
			: nullptr;
		if (section == nullptr)
		{
			return;
		}
		const auto presentation = GetOrBuildSectionPresentation(*section, atlas, atlasDrawingRevision);
		if (presentation == nullptr)
		{
			return;
		}
		AppendSnapshotSection(next, previous, presentation, sectionIndex,
			atlasTexture, atlasSize, atlasDrawingRevision, atlasLease,
			splineWhiteTexture, splineWhiteTextureLease);
		++sectionIndex;
	}

	/** Reuses immutable Section geometry and rebuilds only bindings for a new atlas revision. */
	std::shared_ptr<const BuildingMapSectionPresentation>
	BuildingMapGpuViewport::GetOrBuildSectionPresentation(
		BuildingMapSection& section,
		Cache::SpriteSourceManager& atlas,
		std::uint64_t atlasDrawingRevision)
	{
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
			std::size_t invalidatedBindings = 0;
			RebindSpriteBindings(existing->OrdinaryBindings, atlas, replacement->OrdinaryBindings, invalidatedBindings);
			RebindSpriteBindings(existing->OverlayBindings, atlas, replacement->OverlayBindings, invalidatedBindings);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			if (invalidatedBindings != 0)
			{
				const std::uint64_t sourceHash = section.completedScene_ != nullptr
					? section.completedScene_->SourceContentHash()
					: section.preparingScene_ != nullptr ? section.preparingScene_->SourceContentHash() : 0;
				if (auto* log = MiniMap::Services::TryLog())
				{
					MiniMap::Services::Logf(*log, MiniMap::Services::Level::Warn,
						"MiniMap: BuildingMapPresentationTrace event=atlas-binding-suppression grid=(%lld,%lld) "
						"sourceHash=0x%016llx previousRevision=%llu nextRevision=%llu invalidatedBindings=%llu demanded=1",
						static_cast<long long>(section.gridX_), static_cast<long long>(section.gridY_),
						static_cast<unsigned long long>(sourceHash),
						static_cast<unsigned long long>(section.gpuPresentationAtlasRevision_),
						static_cast<unsigned long long>(atlasDrawingRevision),
						static_cast<unsigned long long>(invalidatedBindings));
				}
			}
#endif
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
		const std::shared_ptr<const void>& atlasLease,
		MiniMap::Services::Texture splineWhiteTexture,
		const std::shared_ptr<const void>& splineWhiteTextureLease) const
	{
		if (!next && (sectionIndex >= previous->Sections.size() || previous->Sections[sectionIndex] != section))
		{
			next = std::make_shared<PresentationSnapshot>();
			next->AtlasTexture = atlasTexture;
			next->AtlasSize = atlasSize;
			next->AtlasDrawingRevision = atlasDrawingRevision;
			next->AtlasLease = atlasLease;
			next->SplineWhiteTexture = splineWhiteTexture;
			next->SplineWhiteTextureLease = splineWhiteTextureLease;
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
		std::vector<BuildingMapSectionPresentation::SpriteBinding>& output,
		[[maybe_unused]] std::size_t& invalidatedBindingCount) const
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		auto& work = Performance::Game();
		Performance::Timer rebind(&work.Rebind);
#endif
		output.clear();
		output.reserve(source.size());
		for (const auto& sourceBinding : source)
		{
			BuildingMapSectionPresentation::SpriteBinding binding = sourceBinding;
			UpdateSpriteBindingRectangle(binding, atlas);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			if (sourceBinding.ContentRectangle[2] != 0 && sourceBinding.ContentRectangle[3] != 0 &&
				(binding.ContentRectangle[2] == 0 || binding.ContentRectangle[3] == 0))
			{
				++invalidatedBindingCount;
			}
#endif
			output.push_back(std::move(binding));
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			++work.BindingsRebound;
#endif
		}
	}

	/** Resumes one Section's ordered draw layout and transfers it only when every pass is sealed. */
	bool BuildingMapGpuViewport::AdvanceSectionGeometryPublication(
		BuildingMapSection& section,
		const Visuals::PreparedBuildingGeometry& geometry,
		Cache::SpriteSourceManager& atlas,
		std::chrono::steady_clock::time_point deadline)
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		std::shared_ptr<Performance::PresentationTiming> timing;
		Performance::Timer assemblyTime(&Performance::Game().Assembly);
#endif
		auto& assembly = section.gpuGeometryAssembly_;
		if (assembly == nullptr || assembly->SourceScene != &geometry ||
			assembly->SourceContentHash != geometry.SourceContentHash())
		{
			BeginSectionGeometryAssembly(section, geometry);
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		timing = assembly->Geometry->Timing;
		assemblyTime.TrackNested(timing != nullptr ? &timing->Assembly : nullptr);
#endif
		while (std::chrono::steady_clock::now() < deadline)
		{
			if (assembly->CurrentPhase == BuildingMapSection::GpuGeometryAssembly::Phase::OrdinarySprites)
			{
				if (!AdvanceSpritePassAssembly(section, geometry, false, deadline))
				{
					return false;
				}
				assembly->CurrentPhase = BuildingMapSection::GpuGeometryAssembly::Phase::SplineTriangles;
			}
			else if (assembly->CurrentPhase == BuildingMapSection::GpuGeometryAssembly::Phase::SplineTriangles)
			{
				if (!AdvanceSplineTriangleAssembly(section, geometry, deadline))
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
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		++Performance::Game().AssembliesStarted;
		auto& drawGeometry = *assembly->Geometry;
		drawGeometry.SourceGeneration = geometry.DiagnosticGeneration();
		drawGeometry.OrdinaryOccurrenceCount = assembly->OrdinaryOccurrenceCount;
		drawGeometry.OverlayOccurrenceCount = assembly->OverlayOccurrenceCount;
		drawGeometry.SurfaceCount = assembly->SurfaceCount;
		drawGeometry.Timing = Performance::Admit(section.gridX_, section.gridY_,
			assembly->SourceContentHash, drawGeometry.SourceGeneration);
		if (drawGeometry.Timing != nullptr)
		{
			geometry.BeginDiagnosticSpriteWindow();
		}
#endif
	}

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
	/** Resolves previous frozen bounds for exact copies of previously sealed append-only ranges. */
	const BuildingMapSectionPresentation::Geometry* BuildingMapGpuViewport::PreviousAssemblyGeometry(
		const BuildingMapSection& section) noexcept
	{
		const auto& previous = section.gpuPresentation_;
		if (previous == nullptr ||
			previous->PreparedGeometry->SourceGeneration != section.gpuGeometryAssembly_->Geometry->SourceGeneration)
		{
			return nullptr;
		}
		return previous->PreparedGeometry.get();
	}
#endif

	/** Publishes sealed vectors by ownership transfer and preserves old snapshot owners. */
	void BuildingMapGpuViewport::PublishSectionGeometryAssembly(BuildingMapSection& section)
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		auto& work = Performance::Game();
		Performance::Timer seal(&work.Seal);
		const bool rebuilding = PreviousAssemblyGeometry(section) != nullptr;
#endif
		auto& assembly = section.gpuGeometryAssembly_;
		assembly->Presentation->PreparedGeometry = std::move(assembly->Geometry);
		const auto& geometry = *assembly->Presentation->PreparedGeometry;
		const bool geometryIsEmpty = geometry.OrdinaryTriangles.empty() &&
			geometry.SplineTriangles.empty() && geometry.OverlayTriangles.empty();
		if (geometryIsEmpty)
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			section.ReportGpuPresentationWithdrawal(
				"gpu-assembly-empty-seal", std::nullopt, assembly->SourceContentHash, true);
#endif
			section.gpuPresentation_.reset();
			section.gpuPresentationAtlasRevisionValid_ = false;
		}
		else
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			ReportSectionGeometrySeal(section, geometry, assembly->SourceContentHash);
#endif
			section.gpuPresentation_ = std::shared_ptr<const BuildingMapSectionPresentation>(
				std::move(assembly->Presentation));
			section.gpuPresentationAtlasRevision_ = assembly->BindingAtlasRevision;
			section.gpuPresentationAtlasRevisionValid_ = true;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			if (geometry.Timing != nullptr)
			{
				geometry.Timing->Triangles =
				geometry.OrdinaryTriangles.size() + geometry.SplineTriangles.size() + geometry.OverlayTriangles.size();
				geometry.Timing->Sealed = Performance::Clock::now();
			}
#endif
		}
		section.gpuGeometryAssembly_.reset();
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		++work.AssembliesSealed;
		work.SameGenerationSeals += rebuilding ? 1 : 0;
#endif
	}

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
	/** Reports drawable appearance or a sealed replacement that reduces published triangle counts. */
	void BuildingMapGpuViewport::ReportSectionGeometrySeal(
		const BuildingMapSection& section,
		const BuildingMapSectionPresentation::Geometry& geometry,
		std::uint64_t sourceContentHash) noexcept
	{
		const auto previousGeometry = section.gpuPresentation_ != nullptr
			? section.gpuPresentation_->PreparedGeometry : nullptr;
		const bool firstOrReappearance = previousGeometry == nullptr;
		const bool lostTriangles = previousGeometry != nullptr &&
			(geometry.OrdinaryTriangles.size() < previousGeometry->OrdinaryTriangles.size() ||
				geometry.SplineTriangles.size() < previousGeometry->SplineTriangles.size() ||
				geometry.OverlayTriangles.size() < previousGeometry->OverlayTriangles.size());
		if (!firstOrReappearance && !lostTriangles)
		{
			return;
		}
		if (auto* log = MiniMap::Services::TryLog())
		{
			if (firstOrReappearance)
			{
				MiniMap::Services::Logf(*log, MiniMap::Services::Level::Info,
					"MiniMap: BuildingMapPresentationTrace event=geometry-appearance grid=(%lld,%lld) "
					"sourceHash=0x%016llx appearance=first-or-reappearance demanded=1 "
				"triangles={ordinary:%llu spline:%llu overlay:%llu}",
					static_cast<long long>(section.gridX_), static_cast<long long>(section.gridY_),
					static_cast<unsigned long long>(sourceContentHash),
					static_cast<unsigned long long>(geometry.OrdinaryTriangles.size()),
				static_cast<unsigned long long>(geometry.SplineTriangles.size()),
					static_cast<unsigned long long>(geometry.OverlayTriangles.size()));
			}
			else
			{
				MiniMap::Services::Logf(*log, MiniMap::Services::Level::Warn,
					"MiniMap: BuildingMapPresentationTrace event=geometry-replacement grid=(%lld,%lld) "
					"previousHash=unknown nextHash=0x%016llx "
				"previousTriangles={ordinary:%llu spline:%llu overlay:%llu} "
				"nextTriangles={ordinary:%llu spline:%llu overlay:%llu} demanded=1",
					static_cast<long long>(section.gridX_), static_cast<long long>(section.gridY_),
					static_cast<unsigned long long>(sourceContentHash),
					static_cast<unsigned long long>(previousGeometry->OrdinaryTriangles.size()),
				static_cast<unsigned long long>(previousGeometry->SplineTriangles.size()),
					static_cast<unsigned long long>(previousGeometry->OverlayTriangles.size()),
					static_cast<unsigned long long>(geometry.OrdinaryTriangles.size()),
				static_cast<unsigned long long>(geometry.SplineTriangles.size()),
					static_cast<unsigned long long>(geometry.OverlayTriangles.size()));
			}
		}
	}
#endif

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
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		auto& work = Performance::Game();
		Performance::Timer copying(&work.SpriteCopy);
		const auto* previous = PreviousAssemblyGeometry(section);
		const std::size_t previousLimit = previous == nullptr ? 0 :
			overlayLast ? previous->OverlayOccurrenceCount : previous->OrdinaryOccurrenceCount;
#endif
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
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			const auto firstCopied = assembly.TriangleIndex;
#endif
			while (assembly.TriangleIndex < occurrence->SectionTriangleCount &&
				std::chrono::steady_clock::now() < deadline)
			{
				triangles.push_back(occurrence->DrawTriangles[assembly.TriangleIndex]);
				++assembly.TriangleIndex;
			}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			const auto copied = assembly.TriangleIndex - firstCopied;
			work.SpriteTrianglesCopied += copied;
			work.PriorTrianglesRecopied += vectorIndex < previousLimit ? copied : 0;
#endif
			if (assembly.TriangleIndex != occurrence->SectionTriangleCount)
			{
				return false;
			}
			if (occurrence->SectionTriangleCount != 0)
			{
				BuildingMapSectionPresentation::SpriteBinding binding;
				binding.FirstTriangle = triangles.size() - occurrence->SectionTriangleCount;
				binding.TriangleCount = occurrence->SectionTriangleCount;
				binding.SourceKey = occurrence->SourceKey;
				bindings.push_back(std::move(binding));
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
				++work.AssemblyBindingsBuilt;
#endif
			}
			++assembly.PainterOrderIndex;
			assembly.TriangleIndex = 0;
		}
		return assembly.PainterOrderIndex == geometry.SpritePainterCount();
	}

	/** Extends the copied triangle envelope, retaining any earlier nonfinite XY failure. */
	void BuildingMapSectionPresentation::SplineWorldBounds::IncludeTriangleInBounds(
		const SplineTriangle& triangle) noexcept
	{
		if (!HasFiniteWorldXY)
		{
			return;
		}
		for (const auto& world : triangle.World)
		{
			if (!std::isfinite(world[0]) || !std::isfinite(world[1]))
			{
				HasFiniteWorldXY = false;
				return;
			}
			MinimumWorldXY[0] = (std::min)(MinimumWorldXY[0], world[0]);
			MinimumWorldXY[1] = (std::min)(MinimumWorldXY[1], world[1]);
			MaximumWorldXY[0] = (std::max)(MaximumWorldXY[0], world[0]);
			MaximumWorldXY[1] = (std::max)(MaximumWorldXY[1], world[1]);
		}
	}

	/** Propagates the growing child envelope to its parent without rereading triangle vertices. */
	void BuildingMapSectionPresentation::SplineWorldBounds::IncludeGroupBounds(
		const SplineWorldBounds& groupBounds) noexcept
	{
		HasFiniteWorldXY = HasFiniteWorldXY && groupBounds.HasFiniteWorldXY;
		if (HasFiniteWorldXY)
		{
			MinimumWorldXY[0] = (std::min)(MinimumWorldXY[0], groupBounds.MinimumWorldXY[0]);
			MinimumWorldXY[1] = (std::min)(MinimumWorldXY[1], groupBounds.MinimumWorldXY[1]);
			MaximumWorldXY[0] = (std::max)(MaximumWorldXY[0], groupBounds.MaximumWorldXY[0]);
			MaximumWorldXY[1] = (std::max)(MaximumWorldXY[1], groupBounds.MaximumWorldXY[1]);
		}
	}

	/** Records one copied triangle in this range's tail group and grows the parent bounds. */
	void BuildingMapSectionPresentation::SplineBindingRange::IncludeCopiedTriangle(
		std::size_t triangleIndex, const SplineTriangle& triangle)
	{
		if (Groups.empty() || Groups.back().TriangleCount == SplineTriangleGroup::Capacity)
		{
			Groups.push_back({triangleIndex, 0, {}});
		}
		auto& group = Groups.back();
		group.Bounds.IncludeTriangleInBounds(triangle);
		Bounds.IncludeGroupBounds(group.Bounds);
		++group.TriangleCount;
	}

	/** Opens a binding interval at the staging tail when the prior interval cannot continue. */
	void BuildingMapSectionPresentation::Geometry::OpenSplineBindingRange(
		Visuals::SplineTextureBindingSlot bindingSlot)
	{
		const std::size_t nextTriangleIndex = SplineTriangles.size();
		if (SplineBindingRanges.empty() || SplineBindingRanges.back().BindingSlot != bindingSlot ||
			SplineBindingRanges.back().FirstTriangle + SplineBindingRanges.back().TriangleCount != nextTriangleIndex)
		{
			SplineBindingRanges.push_back({nextTriangleIndex, 0, bindingSlot});
		}
	}

	/** Appends converted spline values and contiguous binding ranges in established surface order. */
	bool BuildingMapGpuViewport::AdvanceSplineTriangleAssembly(
		BuildingMapSection& section,
		const Visuals::PreparedBuildingGeometry& geometry,
		std::chrono::steady_clock::time_point deadline)
	{
		auto& assembly = *section.gpuGeometryAssembly_;
		const auto& surfaces = geometry.SplineSurfaces();
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		auto& work = Performance::Game();
		Performance::Timer copying(&work.SplineCopy);
		const auto* previous = PreviousAssemblyGeometry(section);
		const std::size_t previousLimit = previous == nullptr ? 0 : previous->SurfaceCount;
#endif
		while (assembly.SurfaceIndex < assembly.SurfaceCount && std::chrono::steady_clock::now() < deadline)
		{
			const auto& surface = surfaces[assembly.SurfaceIndex];
			const auto& triangles = surface.DrawSplineTriangles;
			if (assembly.SurfaceTriangleIndex < triangles.size())
			{
				assembly.Geometry->OpenSplineBindingRange(surface.BindingSlot);
			}
			const auto firstCopied = assembly.SurfaceTriangleIndex;
			while (assembly.SurfaceTriangleIndex < triangles.size() &&
				std::chrono::steady_clock::now() < deadline)
			{
				assembly.Geometry->SplineTriangles.push_back(triangles[assembly.SurfaceTriangleIndex]);
				assembly.Geometry->SplineBindingRanges.back().IncludeCopiedTriangle(
					assembly.Geometry->SplineTriangles.size() - 1, assembly.Geometry->SplineTriangles.back());
				++assembly.SurfaceTriangleIndex;
			}
			const std::size_t copied = assembly.SurfaceTriangleIndex - firstCopied;
			if (copied != 0)
			{
				assembly.Geometry->SplineBindingRanges.back().TriangleCount += copied;
			}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			work.SplineTrianglesCopied += copied;
			work.PriorTrianglesRecopied += assembly.SurfaceIndex < previousLimit ? copied : 0;
#endif
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
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		Performance::Timer binding(&Performance::Game().Binding);
#endif
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
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		++Performance::Game().BindingLookups;
#endif
		binding.ContentRectangle = {};
		Cache::SpriteSourceManager::AtlasSpriteEntry entry;
		if (atlas.TryGetPublishedSprite(binding.SourceKey, entry) && entry.DrawingEligible)
		{
			binding.ContentRectangle = {entry.Content.X, entry.Content.Y,
				entry.Content.Width, entry.Content.Height};
		}
	}

	/** Draws all categories from immutable snapshot data without touching service or atlas state. */
	void BuildingMapGpuViewport::Draw(
		MiniMap::Services::ICanvas& canvas,
		MiniMap::Services::DrawLayer layer,
		const CanvasTransform& transform
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		, Performance::RenderWork* measurements
#endif
		) const
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		Performance::RenderScope scope(measurements);
		Performance::Timer draw(measurements != nullptr ? &measurements->Draw : nullptr);
#endif
		const auto snapshot = presentationSnapshot_.load(std::memory_order_acquire);
		if (!snapshot)
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			if (measurements != nullptr)
			{
				++measurements->MissingSnapshot;
			}
#endif
			return;
		}
		CanvasTransform callbackTransform = transform;
		callbackTransform.Projection.A = transform.Cosine * transform.PixelsPerWorldUnitX;
		callbackTransform.Projection.B = -transform.Sine * transform.PixelsPerWorldUnitX;
		callbackTransform.Projection.C = transform.Sine * transform.PixelsPerWorldUnitY;
		callbackTransform.Projection.D = transform.Cosine * transform.PixelsPerWorldUnitY;
		canvas.PushClip(layer, callbackTransform.ClipMinimum, callbackTransform.ClipMaximum, true);
		DrawSpriteCategory(canvas, layer, callbackTransform, snapshot->AtlasTexture,
			snapshot->AtlasSize, snapshot->Sections, false);
		DrawSplineTriangles(canvas, layer, callbackTransform, snapshot->SplineWhiteTexture,
			snapshot->SplineWhiteTextureLease, snapshot->Sections);
		DrawSpriteCategory(canvas, layer, callbackTransform, snapshot->AtlasTexture,
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
		return {static_cast<float>(transform.ScreenOrigin.x + transform.Projection.A * dx +
			transform.Projection.B * dy),
			static_cast<float>(transform.ScreenOrigin.y + transform.Projection.C * dx +
				transform.Projection.D * dy)};
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
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		auto* frame = Performance::ActiveRender;
		auto* pass = frame != nullptr ? &(overlayLast ? frame->Overlay : frame->Ordinary) : nullptr;
		Performance::Timer draw(pass != nullptr ? &pass->Time : nullptr);
#endif
		if (texture.id == 0 || atlasSize.width == 0 || atlasSize.height == 0)
		{
			return;
		}
		float ordinaryAtlasWidth = 0.0f;
		float ordinaryAtlasHeight = 0.0f;
		if (!overlayLast)
		{
			ordinaryAtlasWidth = static_cast<float>(atlasSize.width);
			ordinaryAtlasHeight = static_cast<float>(atlasSize.height);
		}
		/** Owns the vertex and index storage for one sprite pass at a time. */
		struct SpriteBatchScratch
		{
			std::vector<MiniMap::Services::TexturedTriangleVertex> Vertices;
			std::vector<std::uint16_t> Indices;
			bool InUse = false;
		};
		/** Returns empty scratch to its owner even when submission throws. */
		struct SpriteBatchScratchUse
		{
			SpriteBatchScratch& Scratch;

			/** Clears logical batch contents and ends the exclusive use of this scratch. */
			~SpriteBatchScratchUse() noexcept
			{
				Scratch.Vertices.clear();
				Scratch.Indices.clear();
				Scratch.InUse = false;
			}
		};
		static thread_local SpriteBatchScratch reusableScratch;
		SpriteBatchScratch reentrantScratch;
		auto& scratch = reusableScratch.InUse ? reentrantScratch : reusableScratch;
		scratch.InUse = true;
		const SpriteBatchScratchUse scratchUse{scratch};
		auto& vertices = scratch.Vertices;
		auto& indices = scratch.Indices;
		static constexpr std::size_t MaximumVertices = 65535;
		if (!sections.empty() &&
			(vertices.capacity() < MaximumVertices || indices.capacity() < MaximumVertices))
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			const auto previousVertexCapacity = vertices.capacity();
#endif
			vertices.reserve(MaximumVertices);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			RecordVertexBatchInsertion(pass, previousVertexCapacity, vertices);
			const auto previousIndexCapacity = indices.capacity();
#endif
			indices.reserve(MaximumVertices);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			RecordIndexBatchInsertion(pass, previousIndexCapacity, indices);
#endif
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		bool sampleCurrentBatch = false;
		std::vector<Performance::PresentationTiming*> pendingTimingOwners;
		// One owner per represented Section; three vertices per triangle bound the pending set.
		pendingTimingOwners.reserve((std::min)(sections.size(), MaximumVertices / 3));
#endif
		for (const auto& section : sections)
		{
			const auto& geometry = *section->PreparedGeometry;
			const auto& bindings = overlayLast ? section->OverlayBindings : section->OrdinaryBindings;
			const auto& triangles = overlayLast ? geometry.OverlayTriangles : geometry.OrdinaryTriangles;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			auto* timing = geometry.Timing.get();
			bool timingRecorded = timing == nullptr ||
				timing->FirstSubmissionTicks.load(std::memory_order_relaxed) != 0;
			bool timingPending = false;
			if (pass != nullptr)
			{
				++pass->Sections;
			}
#endif
			for (const auto& binding : bindings)
			{
				if (binding.ContentRectangle[2] == 0 || binding.ContentRectangle[3] == 0)
				{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
					if (pass != nullptr)
					{
						++pass->UnboundRanges;
					}
#endif
					continue;
				}
				SpriteUvOperands ordinaryUvOperands{};
				if (!overlayLast)
				{
					ordinaryUvOperands.AtlasWidth = ordinaryAtlasWidth;
					ordinaryUvOperands.AtlasHeight = ordinaryAtlasHeight;
					ordinaryUvOperands.RectangleX = static_cast<float>(binding.ContentRectangle[0]);
					ordinaryUvOperands.RectangleY = static_cast<float>(binding.ContentRectangle[1]);
					ordinaryUvOperands.RectangleWidth = static_cast<float>(binding.ContentRectangle[2]);
					ordinaryUvOperands.RectangleHeight = static_cast<float>(binding.ContentRectangle[3]);
				}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
				const bool rangeSelected = pass != nullptr && Performance::SampleDrawRange();
				auto* sampled = rangeSelected &&
					(overlayLast || MINIMAP_ORDINARY_RANGE_SAMPLING_DIAGNOSTICS) ? pass : nullptr;
#endif
				for (std::size_t triangleIndex = 0; triangleIndex < binding.TriangleCount; ++triangleIndex)
				{
					const auto& triangle = triangles[binding.FirstTriangle + triangleIndex];
					if (vertices.size() + 3 > MaximumVertices)
					{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
						if (pass != nullptr) ++pass->CanvasCalls;
						const auto batchStarted = sampleCurrentBatch
							? Performance::Clock::now() : Performance::Clock::time_point{};
						Performance::Timer canvasSubmission(!overlayLast && pass != nullptr
							? &pass->OrdinaryCanvasSubmission : nullptr);
						const bool submissionAccepted = canvas.TexturedTriangles(layer, texture, vertices, indices);
						canvasSubmission.Finish();
						if (!submissionAccepted)
#else
						if (!canvas.TexturedTriangles(layer, texture, vertices, indices))
#endif
						{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
							if (pass != nullptr) ++pass->FailedSubmissions;
#endif
							throw std::runtime_error("MiniMap: bulk textured-triangle submission rejected");
						}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
						if (pass != nullptr)
						{
							pass->Submitted += indices.size() / 3;
						}
						if (sampleCurrentBatch)
						{
							pass->CanvasSample.Add(Performance::Milliseconds(Performance::Clock::now() - batchStarted));
						}
						for (auto* timingOwner : pendingTimingOwners)
						{
							timingOwner->RecordFirstSubmission();
						}
						pendingTimingOwners.clear();
						if (timingPending)
						{
							timingRecorded = true;
							timingPending = false;
						}
						sampleCurrentBatch = false;
#endif
						vertices.clear();
						indices.clear();
					}
					if (!overlayLast)
					{
						AppendOrdinarySpriteTriangle(transform, triangle, ordinaryUvOperands,
							vertices, indices
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
						, sampled
#if MINIMAP_ORDINARY_BATCH_CAPACITY_DIAGNOSTICS
						, pass
#endif
#endif
						);
					}
					else
					{
						AppendSpriteTriangle(transform, atlasSize, binding, triangle, vertices, indices
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
							, sampled
#if MINIMAP_ORDINARY_BATCH_CAPACITY_DIAGNOSTICS
							, nullptr
#endif
#endif
							);
					}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
					if (sampled != nullptr) sampleCurrentBatch = true;
					if (timing != nullptr && !timingRecorded && !timingPending)
					{
						pendingTimingOwners.push_back(timing);
						timingPending = true;
					}
#endif
				}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
				if (pass != nullptr)
				{
					++pass->BoundRanges;
					pass->Visited += binding.TriangleCount;
				}
#endif
			}
		}
		if (!vertices.empty())
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			if (pass != nullptr) ++pass->CanvasCalls;
			const auto batchStarted = sampleCurrentBatch
				? Performance::Clock::now() : Performance::Clock::time_point{};
			Performance::Timer canvasSubmission(!overlayLast && pass != nullptr
				? &pass->OrdinaryCanvasSubmission : nullptr);
			const bool submissionAccepted = canvas.TexturedTriangles(layer, texture, vertices, indices);
			canvasSubmission.Finish();
			if (!submissionAccepted)
#else
			if (!canvas.TexturedTriangles(layer, texture, vertices, indices))
#endif
			{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
				if (pass != nullptr) ++pass->FailedSubmissions;
#endif
				throw std::runtime_error("MiniMap: bulk textured-triangle submission rejected");
			}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			if (pass != nullptr)
			{
				pass->Submitted += indices.size() / 3;
			}
			if (sampleCurrentBatch)
			{
				pass->CanvasSample.Add(Performance::Milliseconds(Performance::Clock::now() - batchStarted));
			}
			for (auto* timingOwner : pendingTimingOwners)
			{
				timingOwner->RecordFirstSubmission();
			}
			pendingTimingOwners.clear();
#endif
		}
	}


	#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
	/** Records one vertex insertion's capacity change and vector high-water marks. */
	void BuildingMapGpuViewport::RecordVertexBatchInsertion(
		BuildingMapPerformanceDiagnostics::DrawPass* measurements,
		std::size_t previousCapacity,
		const std::vector<MiniMap::Services::TexturedTriangleVertex>& vertices) noexcept
	{
		if (measurements == nullptr)
		{
			return;
		}
		if (vertices.capacity() > previousCapacity)
		{
			++measurements->VertexCapacityIncreases;
			measurements->VertexCapacityGrowthElements += vertices.capacity() - previousCapacity;
		}
		measurements->MaximumVertexCount = (std::max)(measurements->MaximumVertexCount, vertices.size());
		measurements->MaximumVertexCapacity = (std::max)(measurements->MaximumVertexCapacity, vertices.capacity());
	}

	/** Records one index insertion's capacity change and vector high-water marks. */
	void BuildingMapGpuViewport::RecordIndexBatchInsertion(
		BuildingMapPerformanceDiagnostics::DrawPass* measurements,
		std::size_t previousCapacity,
		const std::vector<std::uint16_t>& indices) noexcept
	{
		if (measurements == nullptr)
		{
			return;
		}
		if (indices.capacity() > previousCapacity)
		{
			++measurements->IndexCapacityIncreases;
			measurements->IndexCapacityGrowthElements += indices.capacity() - previousCapacity;
		}
		measurements->MaximumIndexCount = (std::max)(measurements->MaximumIndexCount, indices.size());
		measurements->MaximumIndexCapacity = (std::max)(measurements->MaximumIndexCapacity, indices.capacity());
	}
	#endif

	/** Projects one retained triangle into the portable transient batch arrays. */
	void BuildingMapGpuViewport::AppendSpriteTriangle(
		const CanvasTransform& transform,
		MiniMap::Services::TextureSize atlasSize,
		const BuildingMapSectionPresentation::SpriteBinding& binding,
		const BuildingMapSectionPresentation::SpriteTriangle& triangle,
		std::vector<MiniMap::Services::TexturedTriangleVertex>& vertices,
		std::vector<std::uint16_t>& indices
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		, Performance::DrawPass* sampledPass
#if MINIMAP_ORDINARY_BATCH_CAPACITY_DIAGNOSTICS
		, Performance::DrawPass* batchDiagnostics
#endif
#endif
		) const
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const auto projectionStarted = sampledPass != nullptr ? Performance::Clock::now() : Performance::Clock::time_point{};
#endif
		const auto firstIndex = static_cast<std::uint16_t>(vertices.size());
		for (std::size_t index = 0; index < 3; ++index)
		{
			const auto position = TransformWorldPoint(triangle.World[index], transform);
			const float u = static_cast<float>((binding.ContentRectangle[0] +
				triangle.ProcessedUv[index][0] * binding.ContentRectangle[2]) / atlasSize.width);
			const float v = static_cast<float>((binding.ContentRectangle[1] +
				triangle.ProcessedUv[index][1] * binding.ContentRectangle[3]) / atlasSize.height);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS && MINIMAP_ORDINARY_BATCH_CAPACITY_DIAGNOSTICS
			const auto previousVertexCapacity = vertices.capacity();
#endif
			vertices.push_back({position.x, position.y, u, v, 0xFFFFFFFFu});
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS && MINIMAP_ORDINARY_BATCH_CAPACITY_DIAGNOSTICS
			RecordVertexBatchInsertion(batchDiagnostics, previousVertexCapacity, vertices);
			const auto previousIndexCapacity = indices.capacity();
#endif
			indices.push_back(static_cast<std::uint16_t>(firstIndex + index));
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS && MINIMAP_ORDINARY_BATCH_CAPACITY_DIAGNOSTICS
			RecordIndexBatchInsertion(batchDiagnostics, previousIndexCapacity, indices);
#endif
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		if (sampledPass != nullptr)
		{
			sampledPass->ProjectionSample.Add(Performance::Milliseconds(Performance::Clock::now() - projectionStarted));
		}
#endif
	}

	/** Projects an ordinary triangle into the transient batch using its preconverted UV operands. */
	void BuildingMapGpuViewport::AppendOrdinarySpriteTriangle(
		const CanvasTransform& transform,
		const BuildingMapSectionPresentation::SpriteTriangle& triangle,
		const SpriteUvOperands& uvOperands,
		std::vector<MiniMap::Services::TexturedTriangleVertex>& vertices,
		std::vector<std::uint16_t>& indices
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		, Performance::DrawPass* sampledPass
#if MINIMAP_ORDINARY_BATCH_CAPACITY_DIAGNOSTICS
		, Performance::DrawPass* batchDiagnostics
#endif
#endif
		) const
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const auto projectionStarted = sampledPass != nullptr ? Performance::Clock::now() : Performance::Clock::time_point{};
#endif
		const auto firstIndex = static_cast<std::uint16_t>(vertices.size());
		for (std::size_t index = 0; index < 3; ++index)
		{
			const auto position = TransformWorldPoint(triangle.World[index], transform);
			const float u = static_cast<float>((uvOperands.RectangleX +
				triangle.ProcessedUv[index][0] * uvOperands.RectangleWidth) / uvOperands.AtlasWidth);
			const float v = static_cast<float>((uvOperands.RectangleY +
				triangle.ProcessedUv[index][1] * uvOperands.RectangleHeight) / uvOperands.AtlasHeight);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS && MINIMAP_ORDINARY_BATCH_CAPACITY_DIAGNOSTICS
			const auto previousVertexCapacity = vertices.capacity();
#endif
			vertices.push_back({position.x, position.y, u, v, 0xFFFFFFFFu});
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS && MINIMAP_ORDINARY_BATCH_CAPACITY_DIAGNOSTICS
			RecordVertexBatchInsertion(batchDiagnostics, previousVertexCapacity, vertices);
			const auto previousIndexCapacity = indices.capacity();
#endif
			indices.push_back(static_cast<std::uint16_t>(firstIndex + index));
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS && MINIMAP_ORDINARY_BATCH_CAPACITY_DIAGNOSTICS
			RecordIndexBatchInsertion(batchDiagnostics, previousIndexCapacity, indices);
#endif
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		if (sampledPass != nullptr)
		{
			sampledPass->ProjectionSample.Add(Performance::Milliseconds(Performance::Clock::now() - projectionStarted));
		}
#endif
	}


	/** Rejects nonfinite or empty clip rectangles before any spline submission. */
	bool BuildingMapGpuViewport::IsValidSplineClip(const CanvasTransform& transform) noexcept
	{
		return std::isfinite(transform.ClipMinimum.x) && std::isfinite(transform.ClipMinimum.y) &&
			std::isfinite(transform.ClipMaximum.x) && std::isfinite(transform.ClipMaximum.y) &&
			transform.ClipMinimum.x < transform.ClipMaximum.x &&
			transform.ClipMinimum.y < transform.ClipMaximum.y;
	}

	/** Draws prepared spline ranges in Section and triangle order. */
	void BuildingMapGpuViewport::DrawSplineTriangles(
		MiniMap::Services::ICanvas& canvas,
		MiniMap::Services::DrawLayer layer,
		const CanvasTransform& transform,
		MiniMap::Services::Texture splineWhiteTexture,
		const std::shared_ptr<const void>& splineWhiteTextureLease,
		const std::vector<std::shared_ptr<const BuildingMapSectionPresentation>>& sections) const
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		auto* frame = Performance::ActiveRender;
		auto* pass = frame != nullptr ? &frame->Spline : nullptr;
		Performance::Timer draw(pass != nullptr ? &pass->Time : nullptr);
#endif
		if (!IsValidSplineClip(transform))
		{
			return;
		}
		thread_local std::vector<MiniMap::Services::TexturedTriangleVertex> vertices;
		thread_local std::vector<std::uint16_t> indices;
		PrepareSplineBatchScratch(vertices, indices);
		MiniMap::Services::Texture activeTexture{};
		bool hasActiveTexture = false;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		Performance::Timer traversal(pass != nullptr ? &pass->SplineTraversal : nullptr);
#endif
		for (const auto& section : sections)
		{
			DrawSplineSection(canvas, layer, transform, *section, splineWhiteTexture,
				splineWhiteTextureLease, activeTexture, hasActiveTexture, vertices, indices
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
				, pass
#endif
				);
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		(void)traversal.Finish();
#endif
		if (hasActiveTexture)
		{
			SubmitSplineTriangleBatch(canvas, layer, activeTexture, vertices, indices
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
				, pass
#endif
				);
		}
	}

	/** Draws one Section while sharing spline batching state with adjacent Sections. */
	void BuildingMapGpuViewport::DrawSplineSection(
		MiniMap::Services::ICanvas& canvas,
		MiniMap::Services::DrawLayer layer,
		const CanvasTransform& transform,
		const BuildingMapSectionPresentation& section
		, MiniMap::Services::Texture splineWhiteTexture,
		const std::shared_ptr<const void>& splineWhiteTextureLease,
		MiniMap::Services::Texture& activeTexture,
		bool& hasActiveTexture,
		std::vector<MiniMap::Services::TexturedTriangleVertex>& vertices,
		std::vector<std::uint16_t>& indices
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		, BuildingMapPerformanceDiagnostics::DrawPass* pass
#endif
		) const
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		if (pass != nullptr)
		{
			++pass->Sections;
		}
#endif
		const auto& geometry = *section.PreparedGeometry;
		for (const auto& range : geometry.SplineBindingRanges)
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			if (pass != nullptr)
			{
				++pass->SplineRangesVisited;
			}
#endif
			DrawSplineBindingRange(canvas, layer, transform, geometry, range,
				splineWhiteTexture, splineWhiteTextureLease, activeTexture, hasActiveTexture,
				vertices, indices
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
				, pass
#endif
				);
		}
	}

	/** Clears and retains the existing bounded render-thread spline batch scratch. */
	void BuildingMapGpuViewport::PrepareSplineBatchScratch(
		std::vector<MiniMap::Services::TexturedTriangleVertex>& vertices,
		std::vector<std::uint16_t>& indices)
	{
		static constexpr std::size_t MaximumVertices = 65535;
		vertices.clear();
		indices.clear();
		if (vertices.capacity() < MaximumVertices || indices.capacity() < MaximumVertices)
		{
			vertices.reserve(MaximumVertices);
			indices.reserve(MaximumVertices);
		}
	}

	/** Proves separation on one screen axis using the callback clip and all four world corners. */
	bool BuildingMapGpuViewport::IsSplineBoundsOutsideClip(
		const BuildingMapSectionPresentation::SplineWorldBounds& bounds,
		const CanvasTransform& transform) noexcept
	{
		const auto& minimum = bounds.MinimumWorldXY;
		const auto& maximum = bounds.MaximumWorldXY;
		if (!bounds.HasFiniteWorldXY || !(minimum[0] <= maximum[0] && minimum[1] <= maximum[1]))
		{
			return false;
		}

		// The same projection and float conversion as the triangle path are monotone
		// in each world axis for a fixed coefficient sign, including at clip edges.
		bool left = true;
		bool right = true;
		bool above = true;
		bool below = true;
		for (const double x : {minimum[0], maximum[0]})
		{
			for (const double y : {minimum[1], maximum[1]})
			{
				const auto corner = TransformWorldPoint({x, y, 0.0}, transform);
				if (!std::isfinite(corner.x) || !std::isfinite(corner.y))
				{
					return false;
				}
				left &= corner.x <= transform.ClipMinimum.x;
				right &= corner.x >= transform.ClipMaximum.x;
				above &= corner.y <= transform.ClipMinimum.y;
				below &= corner.y >= transform.ClipMaximum.y;
			}
		}
		return left || right || above || below;
	}

	/** Resolves and culls one binding interval while retaining the active cross-range batch. */
	void BuildingMapGpuViewport::DrawSplineBindingRange(
		MiniMap::Services::ICanvas& canvas,
		MiniMap::Services::DrawLayer layer,
		const CanvasTransform& transform,
		const BuildingMapSectionPresentation::Geometry& geometry,
		const BuildingMapSectionPresentation::SplineBindingRange& range,
		MiniMap::Services::Texture splineWhiteTexture,
		const std::shared_ptr<const void>& splineWhiteTextureLease,
		MiniMap::Services::Texture& activeTexture,
		bool& hasActiveTexture,
		std::vector<MiniMap::Services::TexturedTriangleVertex>& vertices,
		std::vector<std::uint16_t>& indices
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		, BuildingMapPerformanceDiagnostics::DrawPass* pass
#endif
		) const
	{
		if (range.TriangleCount == 0 || range.FirstTriangle > geometry.SplineTriangles.size() ||
			range.TriangleCount > geometry.SplineTriangles.size() - range.FirstTriangle)
		{
			return;
		}
		const auto texture = ResolveSplineTextureBinding(
			range.BindingSlot, splineWhiteTexture, splineWhiteTextureLease);
		if (texture.id == 0)
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			if (pass != nullptr)
			{
				++pass->UnboundRanges;
			}
#endif
			return;
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		if (pass != nullptr)
		{
			++pass->BoundRanges;
		}
#endif
		if (IsSplineBoundsOutsideClip(range.Bounds, transform))
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			if (pass != nullptr)
			{
				++pass->SplineRangesCulled;
				pass->SplineTrianglesInCulledRanges += range.TriangleCount;
			}
#endif
			return;
		}
		DrawSplineTriangleGroups(canvas, layer, transform, geometry, range, texture,
			activeTexture, hasActiveTexture, vertices, indices
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			, pass
#endif
			);
	}

	/** Checks consecutive child intervals within a validated range and appends surviving triangles. */
	void BuildingMapGpuViewport::DrawSplineTriangleGroups(
		MiniMap::Services::ICanvas& canvas,
		MiniMap::Services::DrawLayer layer,
		const CanvasTransform& transform,
		const BuildingMapSectionPresentation::Geometry& geometry,
		const BuildingMapSectionPresentation::SplineBindingRange& range,
		MiniMap::Services::Texture texture,
		MiniMap::Services::Texture& activeTexture,
		bool& hasActiveTexture,
		std::vector<MiniMap::Services::TexturedTriangleVertex>& vertices,
		std::vector<std::uint16_t>& indices
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		, BuildingMapPerformanceDiagnostics::DrawPass* pass
#endif
		) const
	{
		constexpr auto capacity = BuildingMapSectionPresentation::SplineTriangleGroup::Capacity;
		const auto endTriangle = range.FirstTriangle + range.TriangleCount;
		// The parent interval determines coverage. Only an exact child match can cull a chunk.
		const bool hasExpectedGroupCount = range.Groups.size() == 1 + (range.TriangleCount - 1) / capacity;
		std::size_t firstTriangle = range.FirstTriangle;
		for (std::size_t groupIndex = 0; firstTriangle < endTriangle; ++groupIndex)
		{
			const auto count = (std::min)(capacity, endTriangle - firstTriangle);
			const auto groupEnd = firstTriangle + count;
			const auto* group = hasExpectedGroupCount ? &range.Groups[groupIndex] : nullptr;
			if (group != nullptr && group->FirstTriangle == firstTriangle && group->TriangleCount == count)
			{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
				if (pass != nullptr)
				{
					++pass->SplineGroupsVisited;
				}
#endif
				if (IsSplineBoundsOutsideClip(group->Bounds, transform))
				{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
					if (pass != nullptr)
					{
						++pass->SplineGroupsCulled;
						pass->SplineTrianglesInCulledGroups += count;
					}
#endif
					firstTriangle = groupEnd;
					continue;
				}
			}
			for (; firstTriangle < groupEnd; ++firstTriangle)
			{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
				if (pass != nullptr)
				{
					++pass->Visited;
				}
#endif
				TryAppendSplineTriangle(canvas, layer, texture, geometry.SplineTriangles[firstTriangle],
					transform, activeTexture, hasActiveTexture, vertices, indices
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
					, pass
#endif
					);
			}
		}
	}

	/** Projects and validates finite, nondegenerate, visible spline positions. */
	bool BuildingMapGpuViewport::TryProjectSplinePositions(
		const BuildingMapSectionPresentation::SplineTriangle& triangle,
		const CanvasTransform& transform,
		std::array<MiniMap::Services::Vec2, 3>& positions,
		bool& reverseWinding
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		, BuildingMapPerformanceDiagnostics::DrawPass* pass
#endif
		) const
	{
		for (std::size_t index = 0; index < positions.size(); ++index)
		{
			positions[index] = TransformWorldPoint(triangle.World[index], transform);
			if (!std::isfinite(positions[index].x) || !std::isfinite(positions[index].y))
			{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
				if (pass != nullptr) ++pass->NonfiniteProjectedXY;
#endif
				return false;
			}
		}
		const double area =
			(static_cast<double>(positions[1].x) - positions[0].x) *
			(static_cast<double>(positions[2].y) - positions[0].y) -
			(static_cast<double>(positions[1].y) - positions[0].y) *
			(static_cast<double>(positions[2].x) - positions[0].x);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		if (!std::isfinite(area))
		{
			if (pass != nullptr) ++pass->NonfiniteSignedArea;
			return false;
		}
		if (area == 0.0)
		{
			if (pass != nullptr) ++pass->ZeroSignedArea;
			return false;
		}
#else
		if (!std::isfinite(area) || area == 0.0)
		{
			return false;
		}
#endif
		const float minimumX = (std::min)({positions[0].x, positions[1].x, positions[2].x});
		const float maximumX = (std::max)({positions[0].x, positions[1].x, positions[2].x});
		const float minimumY = (std::min)({positions[0].y, positions[1].y, positions[2].y});
		const float maximumY = (std::max)({positions[0].y, positions[1].y, positions[2].y});
		if (maximumX <= transform.ClipMinimum.x || minimumX >= transform.ClipMaximum.x ||
			maximumY <= transform.ClipMinimum.y || minimumY >= transform.ClipMaximum.y)
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			if (pass != nullptr) ++pass->OutsideScreenClip;
#endif
			return false;
		}
		reverseWinding = area < 0.0;
		return true;
	}

	/** Projects and winding-normalizes one textured triangle with paired UVs. */
	bool BuildingMapGpuViewport::TryProjectSplineTriangle(
		const BuildingMapSectionPresentation::SplineTriangle& triangle,
		const CanvasTransform& transform,
		std::array<MiniMap::Services::TexturedTriangleVertex, 3>& vertices
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		, BuildingMapPerformanceDiagnostics::DrawPass* pass
#endif
		) const
	{
		std::array<MiniMap::Services::Vec2, 3> positions{};
		bool reverseWinding = false;
		if (!TryProjectSplinePositions(triangle, transform, positions, reverseWinding
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			, pass
#endif
			))
		{
			return false;
		}
		const std::uint32_t color = ((std::uint32_t(triangle.Opacity) << 24) |
			((triangle.Rgb & 0xFFu) << 16) | (triangle.Rgb & 0x00FF00u) |
			((triangle.Rgb >> 16) & 0xFFu));
		for (std::size_t index = 0; index < vertices.size(); ++index)
		{
			const std::size_t sourceIndex = reverseWinding && index != 0 ? 3 - index : index;
			const float u = triangle.Uv[sourceIndex][0];
			const float v = triangle.Uv[sourceIndex][1];
			if (!std::isfinite(u) || !std::isfinite(v))
			{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
				if (pass != nullptr) ++pass->NonfiniteUv;
#endif
				return false;
			}
			vertices[index] = {positions[sourceIndex].x, positions[sourceIndex].y, u, v, color};
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		if (pass != nullptr) ++pass->Accepted;
#endif
		return true;
	}

	/** Appends one valid triangle, flushing only when three more vertices exceed capacity. */
	bool BuildingMapGpuViewport::TryAppendSplineTriangle(
		MiniMap::Services::ICanvas& canvas,
		MiniMap::Services::DrawLayer layer,
		MiniMap::Services::Texture texture,
		const BuildingMapSectionPresentation::SplineTriangle& triangle,
		const CanvasTransform& transform,
		MiniMap::Services::Texture& activeTexture,
		bool& hasActiveTexture,
		std::vector<MiniMap::Services::TexturedTriangleVertex>& vertices,
		std::vector<std::uint16_t>& indices
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		, BuildingMapPerformanceDiagnostics::DrawPass* pass
#endif
		) const
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const auto projectionStarted = pass != nullptr ? Performance::Clock::now() : Performance::Clock::time_point{};
#endif
		std::array<MiniMap::Services::TexturedTriangleVertex, 3> projected{};
		if (!TryProjectSplineTriangle(triangle, transform, projected
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			, pass
#endif
			))
		{
			return false;
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		if (pass != nullptr)
		{
			pass->ProjectionSample.Add(Performance::Milliseconds(Performance::Clock::now() - projectionStarted));
		}
#endif
		if (hasActiveTexture && activeTexture.id != texture.id)
		{
			SubmitSplineTriangleBatch(canvas, layer, activeTexture, vertices, indices
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
				, pass
#endif
				);
		}
		activeTexture = texture;
		hasActiveTexture = true;
		if (vertices.size() + projected.size() > 65535)
		{
			SubmitSplineTriangleBatch(canvas, layer, texture, vertices, indices
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
				, pass
#endif
				);
		}
		const auto firstVertex = static_cast<std::uint16_t>(vertices.size());
		vertices.insert(vertices.end(), projected.begin(), projected.end());
		for (std::size_t index = 0; index < projected.size(); ++index)
		{
			indices.push_back(static_cast<std::uint16_t>(firstVertex + index));
		}
		return true;
	}

	/** Resolves the reserved white slot only when its immutable snapshot lease is present. */
	MiniMap::Services::Texture BuildingMapGpuViewport::ResolveSplineTextureBinding(
		Visuals::SplineTextureBindingSlot bindingSlot,
		MiniMap::Services::Texture splineWhiteTexture,
		const std::shared_ptr<const void>& splineWhiteTextureLease) noexcept
	{
		if (bindingSlot != Visuals::SplineTextureBindingSlot::SharedOpaqueWhite ||
			splineWhiteTexture.id == 0 || splineWhiteTextureLease == nullptr)
		{
			return {};
		}
		return splineWhiteTexture;
	}

	/** Submits one nonempty spline batch through the existing bulk textured-triangle canvas operation. */
	void BuildingMapGpuViewport::SubmitSplineTriangleBatch(
		MiniMap::Services::ICanvas& canvas,
		MiniMap::Services::DrawLayer layer,
		MiniMap::Services::Texture texture,
		std::vector<MiniMap::Services::TexturedTriangleVertex>& vertices,
		std::vector<std::uint16_t>& indices
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		, BuildingMapPerformanceDiagnostics::DrawPass* pass
#endif
		) const
	{
		if (vertices.empty())
		{
			return;
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		if (pass != nullptr)
		{
			++pass->CanvasCalls;
		}
		const auto submissionStarted = pass != nullptr ? Performance::Clock::now() : Performance::Clock::time_point{};
#endif
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const bool submissionAccepted = canvas.TexturedTriangles(layer, texture, vertices, indices);
		if (pass != nullptr)
		{
			pass->SplineCanvasSubmission.Add(
				Performance::Milliseconds(Performance::Clock::now() - submissionStarted));
		}
		if (!submissionAccepted)
#else
		if (!canvas.TexturedTriangles(layer, texture, vertices, indices))
#endif
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			if (pass != nullptr)
			{
				++pass->FailedSubmissions;
			}
#endif
			vertices.clear();
			indices.clear();
			throw std::runtime_error("MiniMap: bulk spline-triangle submission rejected");
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		if (pass != nullptr)
		{
			pass->Submitted += indices.size() / 3;
		}
#endif
		vertices.clear();
		indices.clear();
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
