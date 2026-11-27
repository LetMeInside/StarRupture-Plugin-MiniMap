// Defines provider-owned source descriptions and spatial applicability for generic UE terrain.
// Providers are constructed externally; retrieval capabilities are strongly owned and capture is value-only.
#pragma once

#include "ITerrainSourceRetrieval.h"
#include "TerrainImage.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace Services::Map::Terrain
{
    /** Describes terrain sources without owning service acquisition, cache, conversion or presentation. */
    class ITerrainDataProvider
    {
    public:
        /** Copies a native world position; no particular game axes or unit scale are imposed. */
        struct Position
        {
            double X = 0.0;
            double Y = 0.0;
            double Z = 0.0;

            /** Reports whether all coordinates are finite. */
            bool IsFinite() const noexcept
            {
                return std::isfinite(X) && std::isfinite(Y) && std::isfinite(Z);
            }
        };

        /** Describes a finite positive-volume half-open selection or acquisition box. */
        struct Bounds
        {
            Position Minimum;
            Position Maximum;

            /** Requires finite corners and strictly increasing bounds on every axis. */
            bool IsValid() const noexcept
            {
                return Minimum.IsFinite() && Maximum.IsFinite() &&
                    Minimum.X < Maximum.X && Minimum.Y < Maximum.Y && Minimum.Z < Maximum.Z;
            }

            /** Tests physical position with included minima and excluded maxima; invalid values never match. */
            bool Contains(Position position) const noexcept
            {
                return IsValid() && position.IsFinite() &&
                    Minimum.X <= position.X && position.X < Maximum.X &&
                    Minimum.Y <= position.Y && position.Y < Maximum.Y &&
                    Minimum.Z <= position.Z && position.Z < Maximum.Z;
            }

            /** Reports positive-volume intersection; touching faces/edges do not overlap. */
            bool Overlaps(const Bounds& other) const noexcept
            {
                return IsValid() && other.IsValid() &&
                    Minimum.X < other.Maximum.X && other.Minimum.X < Maximum.X &&
                    Minimum.Y < other.Maximum.Y && other.Minimum.Y < Maximum.Y &&
                    Minimum.Z < other.Maximum.Z && other.Minimum.Z < Maximum.Z;
            }
        };

        /** Associates one stable region/output identifier with physical selection bounds. */
        struct Region
        {
            std::string Id;
            Bounds SelectionBounds;
        };

        /** Distinguishes one world default from explicitly bounded alternate regions. */
        enum class CoverageRole
        {
            Default,
            Alternate
        };

        /** Owns registration metadata copied by the service; providers must re-register coverage changes. */
        struct Coverage
        {
            std::string ProviderId;
            std::string WorldId;
            CoverageRole Role = CoverageRole::Alternate;
            std::vector<Region> Regions;

            /** Validates identity, default/alternate shape and distinct nonoverlapping region definitions. */
            bool IsValid() const noexcept
            {
                if (ProviderId.empty() || WorldId.empty()) return false;
                if (Role == CoverageRole::Default) return Regions.empty();
                if (Role != CoverageRole::Alternate || Regions.empty()) return false;
                for (std::size_t regionIndex = 0; regionIndex < Regions.size(); ++regionIndex)
                {
                    const auto& region = Regions[regionIndex];
                    if (region.Id.empty() || !region.SelectionBounds.IsValid()) return false;
                    for (std::size_t earlierIndex = 0; earlierIndex < regionIndex; ++earlierIndex)
                    {
                        const auto& earlier = Regions[earlierIndex];
                        if (region.Id == earlier.Id || region.SelectionBounds.Overlaps(earlier.SelectionBounds))
                            return false;
                    }
                }
                return true;
            }
        };

        /** Separates physical selection position from source sampling used by view demand. */
        struct SourceQuery
        {
            std::string WorldId;
            std::string RegionId;
            Position PhysicalPosition;
            Position SamplePosition;
        };

        /** Couples an opaque locator with strong ownership of its correctly typed retrieval capability. */
        struct RetrievalSource
        {
            ITerrainSourceRetrieval::SourceLocator Locator;
            std::shared_ptr<ITerrainSourceRetrieval> Capability;
            TerrainImage::Registration ImageRegistration;
        };

        /** Copies desired cooked-content capture inputs; creates/owns no world, job or native resource.
         * Data layer paths describe desired activated inputs. Runtime support and clipping require
         * independent validation before a later capture executor may admit this definition.
         */
        struct CaptureDefinition
        {
            std::string OriginalWorldPackage;
            std::vector<std::string> CookedCellPackages;
            std::vector<std::string> TargetGrids;
            std::vector<std::string> ActivatedDataLayerAssets;
            Bounds StreamingBounds;
            Bounds CaptureBounds;
            TerrainImage::Registration ImageRegistration;
            std::uint32_t WidthPixels = 0;
            std::uint32_t HeightPixels = 0;
            bool SRgb = false;
            std::array<std::uint8_t, 3> OpaqueBackground{0, 0, 0};
        };

        /** Owns copied source metadata and exactly one acquisition kind; monostate means unavailable. */
        struct SourceDescription
        {
            TerrainImage::Identity Source;
            std::variant<std::monostate, RetrievalSource, CaptureDefinition> Acquisition;
            TerrainPageIdentity Page;
        };

        /** Releases provider-local metadata; no service jobs/cache or viewport resources are owned here. */
        virtual ~ITerrainDataProvider() = default;

        /** Returns owned applicability metadata for registration; this call must not reenter the service. */
        virtual Coverage DescribeCoverage() const = 0;

        /** Describes query's source without terrain-cache IO, conversion, capture execution or uploads.
         * Retrieval descriptions retain their capability; capture descriptions require no retrieval interface.
         */
        virtual SourceDescription DescribeSource(const SourceQuery& query) = 0;
    };
}
