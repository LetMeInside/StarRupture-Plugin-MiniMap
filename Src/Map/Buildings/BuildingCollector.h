#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "plugin.h"
#include "BuildingGeometryTypes.h"
#include "BuildingSplineProfiles.h"
#include "Map/MapTransform.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
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
    struct SplineGeometry
    {
        std::vector<CurveSegment> Segments;
        Bounds CenterlineBounds;
        GeometryIssue Issue = GeometryIssue::None;
    };
    struct Record
    {
        uint64_t Generation = 0;
        int32_t Index = 0, Serial = 0;
        uint32_t BuildingId = 0;
        // Copied object index for render tracing, never a native lifetime certificate.
        int32_t DefinitionObjectIndex = 0;
        bool TransformValid = false, DefinitionValid = false, HasSpline = false;
        bool InteriorKnown = false, InteriorOnly = false;
        std::array<double, 3> Position = {}, Scale = {};
        std::array<double, 4> Rotation = {};
        Bounds Extent;
        std::shared_ptr<const std::string> CanonicalBlueprint;
        std::shared_ptr<const DefinitionGeometry> DefinitionShape;
        std::shared_ptr<const SplineGeometry> Curve;
    };
    struct Snapshot
    {
        uint64_t Generation = 0;
        Bounds Region;
        bool Complete = false; // Observation completeness; never an atlas publication barrier.
        bool IncludedDroneRails = true; // Immutable policy provenance for this publication.
        std::vector<Record> Records;
    };

    // Reusable, caller-scheduled collector. Each object owns its collection,
    // world session, region and publication state. MiniMap's current events
    // continue to use the namespace facade below.
    class BuildingCollector
    {
    public:
        BuildingCollector();
        ~BuildingCollector();
        BuildingCollector(const BuildingCollector&) = delete;
        BuildingCollector& operator=(const BuildingCollector&) = delete;
        BuildingCollector(BuildingCollector&&) noexcept;
        BuildingCollector& operator=(BuildingCollector&&) noexcept;

        bool InitializeResources();
        void Reset();
        void OnExperienceLoadComplete();
        void SetViewport(const MiniMapMap::Transform& transform);
        void SetRegion(const Bounds& region);
        void SetRenderDroneRails(bool enabled);
        std::shared_ptr<const Snapshot> GetSnapshot();
        void OnTick(float delta);
        void ShutdownResources();
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
