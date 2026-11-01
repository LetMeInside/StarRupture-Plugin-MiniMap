#pragma once

#include "BuildingGeometryTypes.h"
#include <array>
#include <cstdint>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

// CPU-only, world-scoped source index for future BuildingComplex consumers.
// It owns copied numerical/source data and has no Mass, UObject, atlas or GPU dependencies.
namespace MiniMapBuildingComplexIndex
{
    constexpr double kCellWorldSizeCm = 12500.0;
    constexpr uint32_t kCellImageSizePixels = 256;

    struct WorldOrigin
    {
        double X = 0, Y = 0; // Copy MapTerrainTopLeftPivotPoint.X/Y.
        bool Valid = false;
    };

    struct CellKey
    {
        int64_t X = 0, Y = 0; // World axes: X right, Y down.
        bool operator==(const CellKey&) const = default;
        bool operator<(const CellKey& rhs) const noexcept
        { return X < rhs.X || (X == rhs.X && Y < rhs.Y); }
    };

    struct MassKey
    {
        int32_t Index = -1, Serial = 0;
        bool operator==(const MassKey&) const = default;
        bool operator<(const MassKey& rhs) const noexcept
        { return Index < rhs.Index || (Index == rhs.Index && Serial < rhs.Serial); }
    };

    struct WorldBounds
    {
        double MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
        bool Valid = false;
        bool operator==(const WorldBounds&) const = default;
    };

    enum class SupportState : uint8_t { Unknown, Supported, ExplicitlyUnsupported };
    enum class UpsertResult : uint8_t { Inserted, Replaced, Unchanged, InvalidInput, AllocationFailure };
    enum class RemoveResult : uint8_t { Removed, NotFound, AllocationFailure };

    // All strings, transforms, quads, profile inputs and spline section data are
    // copied before construction. Shared immutable payloads avoid duplicating
    // long curves/definitions for each contributing cell.
    struct SplineSectionSource
    {
        uint64_t Identity = 0;
        WorldBounds Bounds; // Original complete source-section support, not clipped cell output.
        std::vector<double> NumericalInputs;
    };

    struct SplineCurveSegmentSource
    {
        std::array<std::array<double, 3>, 4> WorldControls{};
        double StartKey = 0, EndKey = 1;
        uint8_t InterpolationMode = 0;
    };

    struct SourcePayload
    {
        uint64_t SourceRevision = 0;
        uint64_t SourceGeneration = 0;
        uint32_t CanonicalBuildingType = 0;
        std::shared_ptr<const std::string> BlueprintIdentity;
        MiniMapBuildingGeometryTypes::Transform Transform;
        std::array<std::array<double, 3>, 4> SpriteQuadWorld{};
        bool SpriteQuadValid = false;
        uint64_t SpriteCaptureSupportMetadata = 0;
        uint64_t SplineSourceIdentity = 0;
        std::vector<SplineCurveSegmentSource> SplineCurve;
        WorldBounds VisualSupport;
        SupportState Support = SupportState::Unknown;
        std::vector<SplineSectionSource> SplineSections;
        std::vector<double> NumericalProfileInputs;
    };

    struct BuildingRecord
    {
        MassKey Key;
        uint64_t WorldGeneration = 0;
        std::shared_ptr<const SourcePayload> Source;
    };

    struct CellSnapshot
    {
        CellKey Key;
        uint64_t WorldGeneration = 0;
        // Zero for an absent cell; not a persistent cache identity. Async
        // consumers must validate world generation and the relevant source /
        // invalidation revision, never CellRevision alone.
        uint64_t CellRevision = 0;
        uint64_t SourceRevision = 0;
        std::vector<std::shared_ptr<const BuildingRecord>> Records;
        // True if an unbounded unknown-support record could affect this cell.
        bool HasUnresolvedContributor = false;
    };

    struct ChangeSet
    {
        uint64_t WorldGeneration = 0;
        uint64_t SourceRevision = 0;
        std::vector<CellKey> InvalidatedCells;
        // An unbounded unknown-support transition may affect any cell. This
        // invalidates completeness without enumerating world cells.
        bool GlobalCompletenessInvalidated = false;
    };

    class Index
    {
    public:
        Index() = default;
        Index(const Index&) = delete;
        Index& operator=(const Index&) = delete;

        // Origin is copied from the live terrain pivot by the future world service.
        bool BeginWorld(uint64_t worldGeneration, WorldOrigin origin) noexcept;
        void ResetWorld(uint64_t nextWorldGeneration) noexcept;

        UpsertResult Upsert(BuildingRecord record, ChangeSet& changes) noexcept;
        RemoveResult Remove(MassKey key, uint64_t worldGeneration, ChangeSet& changes) noexcept;

        bool TrySnapshot(CellKey key, CellSnapshot& output) const noexcept;
        bool TryGetRecord(MassKey key, uint64_t worldGeneration,
            std::shared_ptr<const BuildingRecord>& output) const noexcept;
        std::vector<std::shared_ptr<const BuildingRecord>> SnapshotUnresolved() const;
        uint64_t WorldGeneration() const noexcept;
        uint64_t SourceRevision() const noexcept;

    private:
        struct Cell
        {
            std::set<MassKey> Records;
            uint64_t Revision = 0;
        };
        struct Stored
        {
            std::shared_ptr<const BuildingRecord> Record;
            std::set<CellKey> Cells;
        };

        mutable std::mutex mutex_;
        WorldOrigin origin_;
        uint64_t worldGeneration_ = 0;
        uint64_t sourceRevision_ = 0;
        uint64_t cellRevisionSequence_ = 0;
        uint64_t resetRevision_ = 0;
        size_t unboundedUnresolvedCount_ = 0;
        std::map<MassKey, Stored> records_;
        std::map<CellKey, Cell> cells_;
        std::set<MassKey> unresolved_;
        // Narrow future dependency hooks. Section support stays whole until a
        // separate preparation transaction produces cell-visible output.
        std::map<std::pair<MassKey, uint64_t>, std::set<CellKey>> sectionCells_;
        std::map<MassKey, std::set<uint64_t>> occluderSections_;
    };

    bool TryWorldCell(double worldX, double worldY, WorldOrigin origin, CellKey& output) noexcept;
}
