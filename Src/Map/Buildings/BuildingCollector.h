#pragma once

// Declares the client-side StarRupture collector and immutable Snapshot source data.
#if defined(MODLOADER_CLIENT_BUILD)
#include "plugin.h"
#include "BuildingGeometryTypes.h"
#include "BuildingSplineProfiles.h"
#include "Map/MapTransform.h"
#include "Services/Map/Buildings/IBuildingCollector.h"
#include "Services/Map/Buildings/BuildingSourceView.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace MiniMapBuildingCollector
{
    struct Bounds
    {
        double MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
        bool Valid = false;
    };
    enum class GeometryIssue : uint8_t
    {
        None, MissingSource, Uninitialized, Looped, InvalidArray, InvalidCurve,
        UnsupportedMode, NonFinite, Oversized, InvalidTransform
    };
    using Point = std::array<double, 3>;
    struct LocalBounds
    {
        Point Min = {}, Max = {};
        bool Valid = false;
        bool operator==(const LocalBounds&) const = default;
    };
    struct DefinitionGeometry
    {
        std::shared_ptr<const MiniMapBuildingSplineProfiles::Set> Profiles;
    };
    struct CurveSegment
    {
        std::array<Point, 4> Controls = {}; // Copied world-space XYZ cubic Bezier, Unreal units; no entity transform applied.
        // Initialized and reconstructed controls retain this verified world-space basis.
        double StartKey = 0, EndKey = 1;
        uint8_t Mode = 0; // Native EInterpCurveMode; constant jumps stay discontinuous.
        bool operator==(const CurveSegment&) const = default;
    };
    /** Owns the neutral numerical arrays adapted from one completed Record.
     * BuildingSplineRenderInput spans are formed only after Snapshot construction
     * finishes, and remain backed by these arrays for the Snapshot lifetime.
     */
    struct OwnedSplineAdaptation
    {
        std::vector<Services::Map::Buildings::Visuals::SplineCurveSegment> Segments;
        std::vector<Services::Map::Buildings::Visuals::BuildingSplineSurfaceRenderInput> Surfaces;
        Services::Map::Buildings::Visuals::SplineSurfaceTransform RootTransform{};
        Services::Map::Buildings::Visuals::SplineSurfaceTransform ComponentTransform{};
        Services::Map::Buildings::Visuals::BuildingSplineSurfaceSelectionMode SelectionMode =
            Services::Map::Buildings::Visuals::BuildingSplineSurfaceSelectionMode::DrawAllSurfaces;
    };
    struct SplineGeometry
    {
        std::vector<CurveSegment> Segments;
        Bounds CenterlineBounds;
        GeometryIssue Issue = GeometryIssue::None;
    };
    struct Record
    {
        uint64_t Generation = 0;
        uint64_t SourceFingerprint = 0;
        int32_t Index = 0, Serial = 0;
        uint32_t BuildingId = 0;
        // Copied object index for render tracing, never a native lifetime certificate.
        int32_t DefinitionObjectIndex = 0;
        bool TransformValid = false, DefinitionValid = false, HasSpline = false;
        /** True only after the current Mass spline-fragment lookup established source kind. */
        bool SplineClassificationKnown = false;
        bool InteriorKnown = false, InteriorOnly = false;
        std::array<double, 3> Position = {}, Scale = {};
        std::array<double, 4> Rotation = {};
        Bounds Extent;
        std::shared_ptr<const std::string> CanonicalBlueprint;
        std::shared_ptr<const DefinitionGeometry> DefinitionShape;
        std::shared_ptr<const SplineGeometry> Curve;
    };
    /** Retains one immutable acquisition result, its completed records and frozen source descriptors.
     * Descriptor string_views are backed by shared immutable keys retained here for Snapshot lifetime.
     */
    struct Snapshot
    {
        uint64_t Generation = 0;
        Bounds Region;
        bool Complete = false; // Authoritative membership reconciliation succeeded.
        bool AcquisitionComplete = false; // No eligible pending work; included support extents are valid.
        bool IncludedDroneRails = true; // Immutable policy provenance for this publication.
        /** True only when every included completed spline Record had a faithful neutral adaptation.
         * This does not certify sprite support, final sampling policy or complete source readiness.
         */
        bool SplineAdaptationComplete = false;
        std::vector<Record> Records;
        /** Known ordinary sprite requirements in deterministic enumeration order. */
        std::vector<Services::Map::Buildings::BuildingSpriteSourceRecord> OrdinarySprites;
        /** Known overlay-last sprite requirements in deterministic enumeration order. */
        std::vector<Services::Map::Buildings::BuildingSpriteSourceRecord> OverlayLastSprites;
        /** Shared owners for every key referenced by the borrowed descriptor string_views. */
        std::vector<std::shared_ptr<const std::string>> SpriteRequirementKeyOwners;
        /** Owned nested curve/surface arrays backing Splines; no descriptor points into collector state. */
        std::vector<OwnedSplineAdaptation> OwnedSplineAdaptations;
        /** Borrowed nested spans backed only by the immutable Snapshot-owned adaptations. */
        std::vector<Services::Map::Buildings::Visuals::BuildingSplineRenderInput> Splines;
    };
    struct CollectorDiagnostics
    {
        uint64_t PublicationGeneration = 0;
        uint64_t AuthoritativeBuildingCount = 0;
        uint64_t CompletedBuildingCount = 0;
        uint64_t PublishedInRegionCount = 0;
        uint64_t PendingAcquisitionCount = 0;
        bool Ready = false;
        bool MembershipComplete = false;
    };

    // Reusable, caller-scheduled collector. Each object owns its collection,
    // world session, region and publication state. MiniMap's current events
    // continue to use the namespace facade below.
    class BuildingCollector final : public Services::Map::Buildings::IBuildingCollector
    {
    public:
        BuildingCollector();
        ~BuildingCollector() override;
        BuildingCollector(const BuildingCollector&) = delete;
        BuildingCollector& operator=(const BuildingCollector&) = delete;
        BuildingCollector(BuildingCollector&&) noexcept;
        BuildingCollector& operator=(BuildingCollector&&) noexcept;

        bool InitializeResources();
        void Reset();
        void OnExperienceLoadComplete();
        bool BeginExplicitSession();
        void SetViewport(const MiniMapMap::Transform& transform);
        /** Selects the explicit acquisition rectangle and revokes source eligibility when it changes.
         * @param region Map-space bounds used by the collector's explicit acquisition session.
         */
        void SetRegion(const Bounds& region);
        /** Validates and stores a native-centimeter acquisition rectangle without conversion. */
        bool SetAcquisitionRegion(
            const Services::Map::Buildings::BuildingCollectionRegion& region) noexcept override;
        /** Fills row-major section observations from one immutable published snapshot. */
        void ObserveSections(
            const Services::Map::Buildings::BuildingSectionGrid& grid,
            std::span<Services::Map::Buildings::BuildingSectionObservation> observations) const override;
        /** Retains the currently exposed immutable snapshot without advancing acquisition.
         * A stale but still exposed snapshot is returned with revoked currentness.
         * @return Retained publication, or empty when no Snapshot is currently exposed.
         */
        std::shared_ptr<const Services::Map::Buildings::IBuildingSourcePublication>
            AcquireSourcePublication() const override;
        /** Changes the effective immutable-publication policy and revokes the old source synchronously.
         * @param enabled Whether drone-rail contributors belong in the next Snapshot.
         */
        void SetRenderDroneRails(bool enabled);
        std::shared_ptr<const Snapshot> GetSnapshot();
        CollectorDiagnostics GetDiagnostics() const;
        void OnTick(float delta);
        void ShutdownResources();
        /** Releases native definition metadata and revokes any publication still exposed. */
        void ShutdownMetadataResources();

    private:
        struct State;
        std::unique_ptr<State> state_;
    };

    // Existing MiniMap compatibility facade; it forwards to one persistent
    // MiniMap-owned BuildingCollector instance.
    bool Initialize(IPluginSelf* self);
    void OnTick(float delta);
    void Reset();
    void OnExperienceLoadComplete();
    void SetViewport(const MiniMapMap::Transform& transform);
    std::shared_ptr<const Snapshot> GetSnapshot();
    void Shutdown();
}
#endif
