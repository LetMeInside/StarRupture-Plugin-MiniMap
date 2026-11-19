#if defined(MODLOADER_CLIENT_BUILD)

// Implements read-only access to complete owned Building Map geometry.
#include "PreparedBuildingGeometry.h"

#include <limits>

namespace Services::Map::Buildings::Visuals
{
	/** Reports whether preparation completed for every required geometry input. */
	bool PreparedBuildingGeometry::IsComplete() const noexcept
	{
		return complete_;
	}

	/** Initializes a fresh scene's status slots to the exact publication occurrence count. */
	void PreparedBuildingGeometry::InitializeContributionStates(std::size_t contributionCount)
	{
		contributionStates_.assign(contributionCount, ContributionStatus::NotProcessed);
		spriteOccurrenceLocationsByOrdinal_.assign(contributionCount, std::nullopt);
		unfinishedContributionCount_ = contributionCount;
		failedContributionCount_ = 0;
		terminalOmissionCount_ = 0;
		terminalSplineOmissionReason_ = BuildingSplineOmissionReason::NoDrawableAlternative;
		drawableGpuTriangleCount_ = 0;
#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
		drawableSplineTriangleCount_ = 0;
		diagnosticSourceOccurrenceCount_ = 0;
		diagnosticSourceSplineOccurrenceCount_ = 0;
#endif
		complete_ = false;
	}

	/** Converts clipped sprite UVs once and leaves numerical triangles untouched. */
	void PreparedBuildingGeometry::PrepareSpriteDrawLayout(
		SpriteOccurrence& occurrence,
		const std::array<TexturedTriangle, 6>& clippedTriangles,
		std::size_t triangleCount) noexcept
	{
		occurrence.SectionTriangleCount = triangleCount;
		for (std::size_t triangleIndex = 0; triangleIndex < triangleCount; ++triangleIndex)
		{
			const auto& source = clippedTriangles[triangleIndex];
			auto& destination = occurrence.DrawTriangles[triangleIndex];
			for (std::size_t vertexIndex = 0; vertexIndex < 3; ++vertexIndex)
			{
				destination.World[vertexIndex] = source.Vertices[vertexIndex].Position;
				destination.ProcessedUv[vertexIndex] = {
					static_cast<float>(source.Vertices[vertexIndex].TextureCoordinates[0]),
					static_cast<float>(source.Vertices[vertexIndex].TextureCoordinates[1])};
			}
		}
	}

	/** Stores each new solid spline triangle with center UVs and its exact Draw material values. */
	void PreparedBuildingGeometry::PrepareSplineSurfaceDrawLayout(SplineSurface& surface)
	{
		surface.DrawSplineTriangles.clear();
		surface.DrawSplineTriangles.reserve(surface.SolidTriangles.size());
		for (const SolidTriangle& source : surface.SolidTriangles)
		{
			SectionDrawSplineTriangle triangle;
			triangle.World = source.Vertices;
			triangle.Uv.fill({0.5f, 0.5f});
			triangle.Rgb = surface.SolidRgb;
			triangle.Opacity = surface.Opacity;
			surface.DrawSplineTriangles.push_back(triangle);
		}
	}

	/** Rejects out-of-range ordinals and any change after an occurrence reaches a terminal state. */
	bool PreparedBuildingGeometry::TrySetContributionStatus(
		std::uint64_t ordinal,
		ContributionStatus status) noexcept
	{
		if (ordinal >= static_cast<std::uint64_t>(contributionStates_.size()))
		{
			return false;
		}
		ContributionStatus& current = contributionStates_[static_cast<std::size_t>(ordinal)];
		if (current == status)
		{
			return true;
		}
		const bool currentTerminal = current == ContributionStatus::Prepared ||
			current == ContributionStatus::TerminalOmission ||
			current == ContributionStatus::GeometricOmission ||
			current == ContributionStatus::Failed;
		if (currentTerminal || (current == ContributionStatus::NotProcessed && status == ContributionStatus::NotProcessed))
		{
			return false;
		}
		if (status == ContributionStatus::NotProcessed)
		{
			return false;
		}
		if (status == ContributionStatus::Prepared || status == ContributionStatus::TerminalOmission ||
			status == ContributionStatus::GeometricOmission ||
			status == ContributionStatus::Failed)
		{
			if (unfinishedContributionCount_ == 0)
			{
				return false;
			}
			--unfinishedContributionCount_;
			failedContributionCount_ += status == ContributionStatus::Failed ? 1 : 0;
			terminalOmissionCount_ += status == ContributionStatus::TerminalOmission ||
				status == ContributionStatus::GeometricOmission ? 1 : 0;
		}
		current = status;
		return true;
	}

	/** Stores one append-only pass-vector index in the matching publication ordinal slot. */
	bool PreparedBuildingGeometry::TrySetSpriteOccurrenceLocation(
		std::uint64_t ordinal,
		bool isOverlayLast,
		std::size_t vectorIndex) noexcept
	{
		if (ordinal >= static_cast<std::uint64_t>(spriteOccurrenceLocationsByOrdinal_.size()))
		{
			return false;
		}
		auto& location = spriteOccurrenceLocationsByOrdinal_[static_cast<std::size_t>(ordinal)];
		if (location.has_value())
		{
			return false;
		}
		const auto& occurrences = isOverlayLast ? overlayLastSprites_ : ordinarySprites_;
		if (vectorIndex >= occurrences.size())
		{
			return false;
		}
		location = SpriteOccurrenceLocation{vectorIndex, isOverlayLast};
		drawableGpuTriangleCount_ += occurrences[vectorIndex].SectionTriangleCount;
		return true;
	}

	/** Allows completion only after every fixed publication occurrence has a successful terminal outcome. */
	bool PreparedBuildingGeometry::CanComplete() const noexcept
	{
		return unfinishedContributionCount_ == 0 && failedContributionCount_ == 0;
	}

	/** Returns the Section publication content hash associated with these contributions. */
	std::uint64_t PreparedBuildingGeometry::SourceContentHash() const noexcept
	{
		return sourceContentHash_;
	}

	/** Borrows the original sprite-first source ordinal order for this generation. */
	const std::vector<std::size_t>& PreparedBuildingGeometry::SpritePainterOrder() const noexcept
	{
		return spritePainterOrder_;
	}

	/** Returns the number of valid-placement sprites at the start of SpritePainterOrder(). */
	std::size_t PreparedBuildingGeometry::ValidSpritePainterCount() const noexcept
	{
		return validSpritePainterCount_;
	}

	/** Returns the total sprite prefix length before non-sprite ordinals. */
	std::size_t PreparedBuildingGeometry::SpritePainterCount() const noexcept
	{
		return spritePainterCount_;
	}

	/** Returns owned ordinary sprite values without transferring their storage. */
	const std::vector<PreparedBuildingGeometry::SpriteOccurrence>&
		PreparedBuildingGeometry::OrdinarySprites() const noexcept
	{
		return ordinarySprites_;
	}

	/** Resolves a mutable sprite occurrence through its stable ordinal-indexed location. */
	PreparedBuildingGeometry::SpriteOccurrence* PreparedBuildingGeometry::SpriteOccurrenceAtOrdinal(
		std::uint64_t ordinal,
		bool& isOverlayLast) noexcept
	{
		if (ordinal >= static_cast<std::uint64_t>(spriteOccurrenceLocationsByOrdinal_.size()))
		{
			return nullptr;
		}
		const auto& location = spriteOccurrenceLocationsByOrdinal_[static_cast<std::size_t>(ordinal)];
		if (!location.has_value())
		{
			return nullptr;
		}
		isOverlayLast = location->IsOverlayLast;
		auto& occurrences = isOverlayLast ? overlayLastSprites_ : ordinarySprites_;
		if (location->VectorIndex >= occurrences.size())
		{
			return nullptr;
		}
		auto& occurrence = occurrences[location->VectorIndex];
		return occurrence.SectionLocalOrdinal == ordinal ? &occurrence : nullptr;
	}

	/** Resolves a read-only sprite occurrence through its stable ordinal-indexed location. */
	const PreparedBuildingGeometry::SpriteOccurrence* PreparedBuildingGeometry::SpriteOccurrenceAtOrdinal(
		std::uint64_t ordinal,
		bool& isOverlayLast,
		std::size_t* vectorIndex) const noexcept
	{
		if (ordinal >= static_cast<std::uint64_t>(spriteOccurrenceLocationsByOrdinal_.size()))
		{
			return nullptr;
		}
		const auto& location = spriteOccurrenceLocationsByOrdinal_[static_cast<std::size_t>(ordinal)];
		if (!location.has_value())
		{
			return nullptr;
		}
		isOverlayLast = location->IsOverlayLast;
		const auto& occurrences = isOverlayLast ? overlayLastSprites_ : ordinarySprites_;
		if (location->VectorIndex >= occurrences.size())
		{
			return nullptr;
		}
		const auto& occurrence = occurrences[location->VectorIndex];
		if (occurrence.SectionLocalOrdinal != ordinal)
		{
			return nullptr;
		}
		if (vectorIndex != nullptr)
		{
			*vectorIndex = location->VectorIndex;
		}
		return &occurrence;
	}

	/** Returns owned clipped spline values without transferring their storage. */
	const std::vector<PreparedBuildingGeometry::SplineSurface>&
		PreparedBuildingGeometry::SplineSurfaces() const noexcept
	{
		return splineSurfaces_;
	}

#if MINIMAP_DEVELOPMENT_DIAGNOSTICS
	/** Returns the retained spline draw-triangle count without scanning surfaces. */
	std::size_t PreparedBuildingGeometry::DiagnosticSplineTriangleCount() const noexcept
	{
		return drawableSplineTriangleCount_;
	}

	/** Returns source occurrence cardinality captured during scheduler initialization. */
	std::size_t PreparedBuildingGeometry::DiagnosticSourceOccurrenceCount() const noexcept
	{
		return diagnosticSourceOccurrenceCount_;
	}

	/** Returns spline source cardinality captured during scheduler initialization. */
	std::size_t PreparedBuildingGeometry::DiagnosticSourceSplineOccurrenceCount() const noexcept
	{
		return diagnosticSourceSplineOccurrenceCount_;
	}
#endif

	/** Returns source ordinals accepted as terminal no-drawable spline omissions. */
	const std::vector<std::uint64_t>& PreparedBuildingGeometry::TerminalSplineOmissionOrdinals() const noexcept
	{
		return terminalSplineOmissionOrdinals_;
	}

	/** Returns the representative reason retained for terminal spline omissions. */
	BuildingSplineOmissionReason PreparedBuildingGeometry::TerminalSplineOmissionReason() const noexcept
	{
		return terminalSplineOmissionReason_;
	}

	/** Returns owned overlay-last sprite values without transferring their storage. */
	const std::vector<PreparedBuildingGeometry::SpriteOccurrence>&
		PreparedBuildingGeometry::OverlayLastSprites() const noexcept
	{
		return overlayLastSprites_;
	}

	/** Returns source-ordinal outcomes while preserving incomplete-scene visibility semantics. */
	const std::vector<PreparedBuildingGeometry::ContributionStatus>&
		PreparedBuildingGeometry::ContributionStates() const noexcept
	{
		return contributionStates_;
	}

}

#endif // MODLOADER_CLIENT_BUILD
