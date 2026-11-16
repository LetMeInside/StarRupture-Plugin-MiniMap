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
			output.push_back(binding);
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
				const auto bindingSlot = surface.BindingSlot;
				const std::size_t nextTriangleIndex = assembly.Geometry->SplineTriangles.size();
				if (assembly.Geometry->SplineBindingRanges.empty() ||
					assembly.Geometry->SplineBindingRanges.back().BindingSlot != bindingSlot ||
					assembly.Geometry->SplineBindingRanges.back().FirstTriangle +
						assembly.Geometry->SplineBindingRanges.back().TriangleCount != nextTriangleIndex)
				{
					assembly.Geometry->SplineBindingRanges.push_back(
						{nextTriangleIndex, 0, bindingSlot});
				}
			}
			const auto firstCopied = assembly.SurfaceTriangleIndex;
			while (assembly.SurfaceTriangleIndex < triangles.size() &&
				std::chrono::steady_clock::now() < deadline)
			{
				assembly.Geometry->SplineTriangles.push_back(triangles[assembly.SurfaceTriangleIndex]);
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
		canvas.PushClip(layer, transform.ClipMinimum, transform.ClipMaximum, true);
		DrawSpriteCategory(canvas, layer, transform, snapshot->AtlasTexture,
			snapshot->AtlasSize, snapshot->Sections, false);
#if defined(MINIMAP_EXPERIMENTAL)
		DrawSplineTriangles(canvas, layer, transform, snapshot->SplineWhiteTexture,
			snapshot->SplineWhiteTextureLease, snapshot->Sections);
#else
		DrawSplineTriangles(canvas, layer, transform, snapshot->Sections);
#endif
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
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		auto* frame = Performance::ActiveRender;
		auto* pass = frame != nullptr ? &(overlayLast ? frame->Overlay : frame->Ordinary) : nullptr;
		Performance::Timer draw(pass != nullptr ? &pass->Time : nullptr);
#endif
		if (texture.id == 0 || atlasSize.width == 0 || atlasSize.height == 0)
		{
			return;
		}
#if defined(MINIMAP_EXPERIMENTAL)
		std::vector<MiniMap::Services::TexturedTriangleVertex> vertices;
		std::vector<std::uint16_t> indices;
		static constexpr std::size_t MaximumVertices = 65535;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		bool sampleCurrentBatch = false;
		std::vector<Performance::PresentationTiming*> pendingTimingOwners;
		// One owner per represented Section; three vertices per triangle bound the pending set.
		pendingTimingOwners.reserve((std::min)(sections.size(), MaximumVertices / 3));
#endif
#endif
		for (const auto& section : sections)
		{
			const auto& geometry = *section->PreparedGeometry;
			const auto& bindings = overlayLast ? section->OverlayBindings : section->OrdinaryBindings;
			const auto& triangles = overlayLast ? geometry.OverlayTriangles : geometry.OrdinaryTriangles;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			auto* timing = geometry.Timing.get();
#if defined(MINIMAP_EXPERIMENTAL)
			bool timingRecorded = timing == nullptr ||
				timing->FirstSubmissionTicks.load(std::memory_order_relaxed) != 0;
			bool timingPending = false;
#endif
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
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
				auto* sampled = pass != nullptr && Performance::SampleDrawRange() ? pass : nullptr;
#endif
				for (std::size_t triangleIndex = 0; triangleIndex < binding.TriangleCount; ++triangleIndex)
				{
					const auto& triangle = triangles[binding.FirstTriangle + triangleIndex];
#if defined(MINIMAP_EXPERIMENTAL)
					if (vertices.size() + 3 > MaximumVertices)
					{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
						if (pass != nullptr) ++pass->CanvasCalls;
						const auto batchStarted = sampleCurrentBatch
							? Performance::Clock::now() : Performance::Clock::time_point{};
#endif
						if (!canvas.TexturedTriangles(layer, texture, vertices, indices))
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
					AppendSpriteTriangle(transform, atlasSize, binding, triangle, vertices, indices
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
						, sampled
#endif
						);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
					if (sampled != nullptr) sampleCurrentBatch = true;
					if (timing != nullptr && !timingRecorded && !timingPending)
					{
						pendingTimingOwners.push_back(timing);
						timingPending = true;
					}
#endif
#else
					DrawSpriteTriangle(canvas, layer, transform, texture, atlasSize,
						binding, triangle
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
						, sampled
#endif
						);
					#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
					if (pass != nullptr) ++pass->CanvasCalls;
					#endif
#endif
				}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
				if (pass != nullptr)
				{
					++pass->BoundRanges;
					pass->Visited += binding.TriangleCount;
#if !defined(MINIMAP_EXPERIMENTAL)
					pass->Submitted += binding.TriangleCount;
#endif
				}
#if !defined(MINIMAP_EXPERIMENTAL)
				if (timing != nullptr && binding.TriangleCount != 0)
				{
					timing->RecordFirstSubmission();
					timing = nullptr;
				}
#endif
#endif
			}
		}
#if defined(MINIMAP_EXPERIMENTAL)
		if (!vertices.empty())
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			if (pass != nullptr) ++pass->CanvasCalls;
			const auto batchStarted = sampleCurrentBatch
				? Performance::Clock::now() : Performance::Clock::time_point{};
#endif
			if (!canvas.TexturedTriangles(layer, texture, vertices, indices))
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
#endif
	}

	/** Transforms prepared positions and processed UVs into the snapshot's matching atlas. */
	void BuildingMapGpuViewport::DrawSpriteTriangle(
		MiniMap::Services::ICanvas& canvas,
		MiniMap::Services::DrawLayer layer,
		const CanvasTransform& transform,
		MiniMap::Services::Texture texture,
		MiniMap::Services::TextureSize atlasSize,
		const BuildingMapSectionPresentation::SpriteBinding& binding,
		const BuildingMapSectionPresentation::SpriteTriangle& triangle
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		, Performance::DrawPass* sampledPass
#endif
		) const
	{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const auto projectionStarted = sampledPass != nullptr ? Performance::Clock::now() : Performance::Clock::time_point{};
#endif
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
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		const auto canvasStarted = sampledPass != nullptr ? Performance::Clock::now() : Performance::Clock::time_point{};
#endif
		canvas.TexturedTriangle(layer, texture, positions, uvs, 0xFFFFFFFFu);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		if (sampledPass != nullptr)
		{
			const auto canvasFinished = Performance::Clock::now();
			sampledPass->ProjectionSample.Add(Performance::Milliseconds(canvasStarted - projectionStarted));
			sampledPass->CanvasSample.Add(Performance::Milliseconds(canvasFinished - canvasStarted));
		}
#endif
	}

#if defined(MINIMAP_EXPERIMENTAL)
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
			vertices.push_back({position.x, position.y, u, v, 0xFFFFFFFFu});
			indices.push_back(static_cast<std::uint16_t>(firstIndex + index));
		}
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		if (sampledPass != nullptr)
		{
			sampledPass->ProjectionSample.Add(Performance::Milliseconds(Performance::Clock::now() - projectionStarted));
		}
#endif
	}
#endif

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
#if defined(MINIMAP_EXPERIMENTAL)
		MiniMap::Services::Texture splineWhiteTexture,
		const std::shared_ptr<const void>& splineWhiteTextureLease,
#endif
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
#if defined(MINIMAP_EXPERIMENTAL)
		thread_local std::vector<MiniMap::Services::TexturedTriangleVertex> vertices;
		thread_local std::vector<std::uint16_t> indices;
		PrepareSplineBatchScratch(vertices, indices);
		MiniMap::Services::Texture activeTexture{};
		bool hasActiveTexture = false;
#endif
		for (const auto& section : sections)
		{
#if defined(MINIMAP_EXPERIMENTAL)
			DrawSplineSection(canvas, layer, transform, *section, splineWhiteTexture,
				splineWhiteTextureLease, activeTexture, hasActiveTexture, vertices, indices
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
				, pass
#endif
				);
#else
			DrawSplineSection(canvas, layer, transform, *section
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
				, pass
#endif
				);
#endif
		}
#if defined(MINIMAP_EXPERIMENTAL)
		if (hasActiveTexture)
		{
			SubmitSplineTriangleBatch(canvas, layer, activeTexture, vertices, indices
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
				, pass
#endif
				);
		}
#endif
	}

	/** Draws one Section while sharing spline batching state with adjacent Sections. */
	void BuildingMapGpuViewport::DrawSplineSection(
		MiniMap::Services::ICanvas& canvas,
		MiniMap::Services::DrawLayer layer,
		const CanvasTransform& transform,
		const BuildingMapSectionPresentation& section
#if defined(MINIMAP_EXPERIMENTAL)
		, MiniMap::Services::Texture splineWhiteTexture,
		const std::shared_ptr<const void>& splineWhiteTextureLease,
		MiniMap::Services::Texture& activeTexture,
		bool& hasActiveTexture,
		std::vector<MiniMap::Services::TexturedTriangleVertex>& vertices,
		std::vector<std::uint16_t>& indices
#endif
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
#if defined(MINIMAP_EXPERIMENTAL)
		const auto& geometry = *section.PreparedGeometry;
		for (const auto& range : geometry.SplineBindingRanges)
		{
			DrawSplineBindingRange(canvas, layer, transform, geometry, range,
				splineWhiteTexture, splineWhiteTextureLease, activeTexture, hasActiveTexture,
				vertices, indices
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
				, pass
#endif
				);
		}
#else
		for (const auto& triangle : section.PreparedGeometry->SplineTriangles)
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			if (pass != nullptr)
			{
				++pass->Visited;
			}
#endif
			DrawFilledSplineTriangle(canvas, layer, transform, triangle
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
				, pass
#endif
				);
		}
#endif
	}

#if defined(MINIMAP_EXPERIMENTAL)
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

	/** Traverses one binding interval while retaining the active cross-range batch. */
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
		const std::size_t endTriangle = range.FirstTriangle + range.TriangleCount;
		for (std::size_t triangleIndex = range.FirstTriangle; triangleIndex < endTriangle; ++triangleIndex)
		{
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
			if (pass != nullptr)
			{
				++pass->Visited;
			}
#endif
			TryAppendSplineTriangle(canvas, layer, texture, geometry.SplineTriangles[triangleIndex],
				transform, activeTexture, hasActiveTexture, vertices, indices
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
				, pass
#endif
				);
		}
	}
#endif

	/** Projects and validates finite, nondegenerate, visible spline positions. */
	bool BuildingMapGpuViewport::TryProjectSplinePositions(
		const BuildingMapSectionPresentation::SplineTriangle& triangle,
		const CanvasTransform& transform,
		std::array<MiniMap::Services::Vec2, 3>& positions,
		bool& reverseWinding) const
	{
		for (std::size_t index = 0; index < positions.size(); ++index)
		{
			positions[index] = TransformWorldPoint(triangle.World[index], transform);
			if (!std::isfinite(positions[index].x) || !std::isfinite(positions[index].y))
			{
				return false;
			}
		}
		const double area =
			(static_cast<double>(positions[1].x) - positions[0].x) *
			(static_cast<double>(positions[2].y) - positions[0].y) -
			(static_cast<double>(positions[1].y) - positions[0].y) *
			(static_cast<double>(positions[2].x) - positions[0].x);
		if (!std::isfinite(area) || area == 0.0)
		{
			return false;
		}
		const float minimumX = (std::min)({positions[0].x, positions[1].x, positions[2].x});
		const float maximumX = (std::max)({positions[0].x, positions[1].x, positions[2].x});
		const float minimumY = (std::min)({positions[0].y, positions[1].y, positions[2].y});
		const float maximumY = (std::max)({positions[0].y, positions[1].y, positions[2].y});
		if (maximumX <= transform.ClipMinimum.x || minimumX >= transform.ClipMaximum.x ||
			maximumY <= transform.ClipMinimum.y || minimumY >= transform.ClipMaximum.y)
		{
			return false;
		}
		reverseWinding = area < 0.0;
		return true;
	}

#if defined(MINIMAP_EXPERIMENTAL)
	/** Projects and winding-normalizes one textured triangle with paired UVs. */
	bool BuildingMapGpuViewport::TryProjectSplineTriangle(
		const BuildingMapSectionPresentation::SplineTriangle& triangle,
		const CanvasTransform& transform,
		std::array<MiniMap::Services::TexturedTriangleVertex, 3>& vertices) const
	{
		std::array<MiniMap::Services::Vec2, 3> positions{};
		bool reverseWinding = false;
		if (!TryProjectSplinePositions(triangle, transform, positions, reverseWinding))
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
				return false;
			}
			vertices[index] = {positions[sourceIndex].x, positions[sourceIndex].y, u, v, color};
		}
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
		if (!TryProjectSplineTriangle(triangle, transform, projected))
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
#endif
		if (!canvas.TexturedTriangles(layer, texture, vertices, indices))
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
#else
	/** Preserves the non-experimental FilledTriangle submission and its diagnostics. */
	void BuildingMapGpuViewport::DrawFilledSplineTriangle(
		MiniMap::Services::ICanvas& canvas,
		MiniMap::Services::DrawLayer layer,
		const CanvasTransform& transform,
		const BuildingMapSectionPresentation::SplineTriangle& triangle
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		, BuildingMapPerformanceDiagnostics::DrawPass* pass
#endif
		) const
	{
		std::array<MiniMap::Services::Vec2, 3> positions{};
		bool reverseWinding = false;
		if (!TryProjectSplinePositions(triangle, transform, positions, reverseWinding))
		{
			return;
		}
		if (reverseWinding)
		{
			std::swap(positions[1], positions[2]);
		}
		const std::uint32_t color = ((std::uint32_t(triangle.Opacity) << 24) |
			((triangle.Rgb & 0xFFu) << 16) | (triangle.Rgb & 0x00FF00u) |
			((triangle.Rgb >> 16) & 0xFFu));
		canvas.FilledTriangle(layer, positions, color);
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		if (pass != nullptr)
		{
			++pass->Submitted;
			++pass->CanvasCalls;
		}
#endif
	}
#endif

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
