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
    // Complete means every drawable section in the selected MiniMap
    // representation was retained; it does not certify visual contributors.
    enum class SplinePreparationState : uint8_t { NotPresent, Complete, Partial, Unsupported, Invalid };
    enum class UpsertResult : uint8_t { Inserted, Replaced, Unchanged, InvalidInput, AllocationFailure };
    enum class RemoveResult : uint8_t { Removed, NotFound, AllocationFailure };

    // All strings, transforms, quads, profile inputs and spline section data are
    // copied before construction. Shared immutable payloads avoid duplicating
    // long curves/definitions for each contributing cell.
    struct SplineTriangleSource
    {
        std::array<std::array<double, 3>, 3> Vertices{};
        bool operator==(const SplineTriangleSource&) const = default;
    };

    struct SplineInsetBandSource
    {
        std::array<std::array<double, 2>, 4> Vertices{};
        bool operator==(const SplineInsetBandSource&) const = default;
    };

    struct SplineSectionSource
    {
        uint64_t Identity = 0; // Ordinal in this exact Mass/source-revision geometry.
        uint64_t SourceSegment = 0, SampleOrdinal = 0, ProfileIndex = 0;
        double StartT = 0, EndT = 1;
        bool SyntheticThin = false;
        WorldBounds Bounds; // Bounds of the complete generated MiniMap-equivalent section.
        std::vector<SplineTriangleSource> Triangles; // Original, unoccluded world XYZ geometry.
        std::array<SplineInsetBandSource, 2> WalkwayInsetBands{};
        bool HasWalkwayInsetBands = false;
        std::vector<double> NumericalInputs;
        bool operator==(const SplineSectionSource&) const = default;
    };

    struct SectionKey
    {
        MassKey Building;
        uint64_t SourceRevision = 0, Identity = 0;
        bool operator==(const SectionKey&) const = default;
        bool operator<(const SectionKey& rhs) const noexcept
        {
            if (Building < rhs.Building) return true;
            if (rhs.Building < Building) return false;
            return SourceRevision < rhs.SourceRevision ||
                (SourceRevision == rhs.SourceRevision && Identity < rhs.Identity);
        }
    };

    struct SplineOccluderDependency
    {
        MassKey Key;
        uint64_t SourceRevision = 0;
        std::array<std::array<double, 2>, 4> Corners{};
        double TopZ = 0;
    };

    struct OccludedSplineSection
    {
        uint64_t Identity = 0;
        uint64_t DependencyRevision = 0;
        std::vector<SplineTriangleSource> VisibleTriangles;
        std::vector<SplineTriangleSource> WalkwayInsetTriangles;
        std::vector<SplineOccluderDependency> Dependencies; // Includes all triangle-AABB candidates.
        bool Complete = false;
    };

    enum class SplineOcclusionState : uint8_t { Unevaluated, Complete, Partial, Invalid };

    struct SplineOcclusionPayload
    {
        uint64_t WorldGeneration = 0, SourceRevision = 0, DependencyRevision = 0;
        SplineOcclusionState State = SplineOcclusionState::Unevaluated;
        std::vector<OccludedSplineSection> Sections;
    };

    struct SplineCurveSegmentSource
    {
        std::array<std::array<double, 3>, 4> WorldControls{};
        double StartKey = 0, EndKey = 1;
        uint8_t InterpolationMode = 0;
    };

    // Exact copied inputs consumed by the existing MiniMap spline profile
    // projector. They remain distinct from its prepared CPU output.
    struct SplineProfileSource
    {
        std::array<double, 3> BoundsOrigin{}, BoundsExtent{};
        double BoundsSphereRadius = 0;
        std::array<double, 3> ComponentTranslation{}, ComponentRotationDegrees{}, ComponentScale{1, 1, 1};
        std::array<double, 2> StartScale{1, 1}, EndScale{1, 1}, StartOffset{}, EndOffset{};
        double StartRoll = 0, EndRoll = 0;
        std::array<double, 3> Up{0, 0, 1};
        uint8_t ForwardAxis = 0;
        uint8_t SourceRank = 0; // 0 high-res trait, 1 low-res trait, 2 authored class.
        std::array<int64_t, 12> SelectionOrder{}; // Existing collector's exact deterministic tie-break key.
    };

    struct SourcePayload
    {
        uint64_t SourceRevision = 0;
        uint64_t SourceGeneration = 0;
        uint32_t CanonicalBuildingType = 0;
        std::shared_ptr<const std::string> BlueprintIdentity;
        // Preserves the established Base Core last-overlay classification.
        bool BaseCoreOverlay = false;
        // Copied from the existing CrBuildingData Interior tag policy.
        // False is meaningful only when InteriorKnown is true.
        bool InteriorKnown = false;
        bool InteriorOnly = false;
        MiniMapBuildingGeometryTypes::Transform Transform;
        std::array<std::array<double, 3>, 4> SpriteQuadWorld{};
        bool SpriteQuadValid = false;
        uint64_t SpriteCaptureSupportMetadata = 0;
        uint64_t SplineSourceIdentity = 0;
        bool HasSplineSource = false;
        uint8_t SplineCategory = 3; // MiniMapBuildingSplinePolicy::SplineCategory numeric value.
        bool SplineSortZValid = false;
        double SplineSortZ = 0;
        std::vector<SplineCurveSegmentSource> SplineCurve;
        SplinePreparationState SplinePreparation = SplinePreparationState::NotPresent;
        // Bounds of prepared MiniMap-equivalent ribbon sections. This field
        // targets cells but never certifies complete visual-contributor support.
        WorldBounds PreparedSplineSupport;
        // Bounds proven only for the ordinary sprite. They support targeted
        // cell invalidation but do not certify complete visual support.
        WorldBounds OrdinarySpriteSupport;
        // Full visual-contributor bounds; only this field can bound an
        // Unknown record for global completeness purposes.
        WorldBounds VisualSupport;
        SupportState Support = SupportState::Unknown;
        std::vector<SplineSectionSource> SplineSections;
        std::vector<SplineProfileSource> SplineProfiles;
        std::vector<double> NumericalProfileInputs;
    };

    struct BuildingRecord
    {
        MassKey Key;
        uint64_t WorldGeneration = 0;
        std::shared_ptr<const SourcePayload> Source;
        std::shared_ptr<const SplineOcclusionPayload> SplineOcclusion;
    };

    // Phase-1 foundation only: no current validator accepts this token yet.
    // Compare only within the same world/restore/grid/render-policy scope.
    // Zero means unavailable; it must never certify currentness.
    struct CellValidity
    {
        uint64_t SafetyGeneration = 0;
        uint64_t LocalEpoch = 0;
        bool operator==(const CellValidity&) const = default;
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
        uint64_t DependencyRevision = 0;
        WorldOrigin Origin;
        std::vector<std::shared_ptr<const BuildingRecord>> Records;
        // True if an unbounded unknown-support record could affect this cell.
        bool HasUnresolvedContributor = false;
        CellValidity Validity;
    };

    struct ChangeSet
    {
        uint64_t WorldGeneration = 0;
        uint64_t SourceRevision = 0;
        uint64_t DependencyRevision = 0;
        std::vector<CellKey> InvalidatedCells;
        std::vector<SectionKey> InvalidatedSections;
        // An unbounded unknown-support transition may affect any cell. This
        // invalidates completeness without enumerating world cells.
        bool GlobalCompletenessInvalidated = false;
        bool GlobalSplineOcclusionInvalidated = false;
        // Parallel local-validity coverage; legacy InvalidatedCells semantics
        // and publication invalidation remain unchanged in Phase 1.
        std::vector<CellKey> OldGraphicalCells;
        std::vector<CellKey> NewGraphicalCells;
        std::vector<CellKey> DependencyOutputCells;
        std::vector<CellKey> LocalInvalidatedCells;
        bool LocalSafetyInvalidated = false;
        uint64_t SafetyGeneration = 0;
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
        // Metadata-only safety transitions, never source discovery or GPU work.
        // World owner calls before capture availability mutations, including
        // replacements with unchanged source geometry. Bounds changes still
        // flow through Upsert's old/new support transaction.
        void NotifyCaptureChange(MassKey key, uint64_t worldGeneration, ChangeSet& changes) noexcept;
        void InvalidateSafety() noexcept;
        bool ObserveRenderingPolicy(bool renderDroneRails) noexcept;

        UpsertResult Upsert(BuildingRecord record, ChangeSet& changes) noexcept;
        RemoveResult Remove(MassKey key, uint64_t worldGeneration, ChangeSet& changes) noexcept;

        bool TrySnapshot(CellKey key, CellSnapshot& output) const noexcept;
        bool TryGetRecord(MassKey key, uint64_t worldGeneration,
            std::shared_ptr<const BuildingRecord>& output) const noexcept;
        std::vector<SectionKey> SnapshotSplineSections() const;
        bool IsCurrentSplineSection(SectionKey section, uint64_t worldGeneration) const noexcept;
        bool TryGetOcclusionInputs(SectionKey section,
            std::shared_ptr<const BuildingRecord>& spline,
            std::vector<std::shared_ptr<const BuildingRecord>>& ordinaryCandidates,
            bool& hasUnresolvedOrdinary, uint64_t& dependencyRevision) const noexcept;
        bool PublishSplineOcclusion(SectionKey section, uint64_t worldGeneration,
            OccludedSplineSection output, SplineOcclusionState state,
            ChangeSet& changes) noexcept;
        std::vector<std::shared_ptr<const BuildingRecord>> SnapshotUnresolved() const;
        uint64_t WorldGeneration() const noexcept;
        uint64_t SourceRevision() const noexcept;
        uint64_t DependencyRevision() const noexcept;

    private:
        void AdvanceSafetyLocked() const noexcept;
        CellValidity SnapshotValidityLocked(CellKey key) const noexcept;
        void PrepareLocalChangeLocked(ChangeSet& changes, const std::set<CellKey>& oldCells,
            const std::set<CellKey>& newCells, const std::vector<SectionKey>& affectedSections,
            const std::map<SectionKey, std::set<CellKey>>* newSections = nullptr) const noexcept;
        void ApplyLocalChangeLocked(ChangeSet& changes) noexcept;
        struct Cell
        {
            std::set<MassKey> Records;
            uint64_t Revision = 0;
        };
        struct Stored
        {
            std::shared_ptr<const BuildingRecord> Record;
            std::set<CellKey> Cells;
            std::set<SectionKey> Sections;
        };

        mutable std::mutex mutex_;
        WorldOrigin origin_;
        uint64_t worldGeneration_ = 0;
        uint64_t sourceRevision_ = 0;
        uint64_t dependencyRevision_ = 0;
        uint64_t cellRevisionSequence_ = 0;
        uint64_t resetRevision_ = 0;
        // Tombstones are independent of allocated Cell records, including
        // demanded Empty cells. Retain until safety advances; old certificates
        // then fail by generation even if local epoch values are reused.
        mutable uint64_t safetyGeneration_ = 1;
        mutable uint64_t localEpochSequence_ = 0;
        mutable bool localValidityAvailable_ = true;
        mutable std::map<CellKey, uint64_t> localEpochs_;
        bool renderingPolicyKnown_ = false;
        bool renderDroneRails_ = true;
        size_t unboundedUnresolvedCount_ = 0;
        size_t unresolvedOccluderCount_ = 0;
        std::map<MassKey, Stored> records_;
        std::map<CellKey, Cell> cells_;
        std::set<MassKey> unresolved_;
        std::map<SectionKey, std::set<CellKey>> sectionCells_;
        std::map<MassKey, std::set<SectionKey>> occluderSections_;
        std::map<SectionKey, uint64_t> sectionDependencyRevisions_;
    };

    bool TryWorldCell(double worldX, double worldY, WorldOrigin origin, CellKey& output) noexcept;
}
