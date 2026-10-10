#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "../plugin.h"
#include "MapTransform.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace MiniMapBuildingInventory { struct Record; }
namespace SDK { class UObject; }
namespace MiniMapBuildingCollector
{
    struct Bounds
    {
        double MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
        bool Valid = false;
    };
    enum class Coverage : uint8_t { Unresolved, OriginPadding, SplineHull, OrdinaryProxy };
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
    // Definition-shared copied metadata. A gameplay targeting box is NOT proof
    // of the complete visual footprint. Safety allowances remain explicit.
    struct DefinitionGeometry
    {
        LocalBounds TargetingBox;
        LocalBounds SplineMeshBounds;
        double SplineMeshCrossSection = 0, RailEndpointExtension = 0;
        int32_t VisualClassObjectIndex = 0;
        int32_t SplineMeshObjectIndex = 0;
        int32_t SplineMeshNameIndex = 0;
        uint32_t SplineMeshNameNumber = 0;
        bool HasSplineMesh = false, HasRailExtension = false;
        GeometryIssue Issue = GeometryIssue::MissingSource;
        bool operator==(const DefinitionGeometry&) const = default;
    };
    struct CurveSegment
    {
        std::array<Point, 4> Controls = {}; // World-space cubic Bezier, Unreal units.
        double StartKey = 0, EndKey = 1;
        uint8_t Mode = 0; // Native EInterpCurveMode; constant jumps stay discontinuous.
        bool operator==(const CurveSegment&) const = default;
    };
    struct SplineGeometry
    {
        std::vector<CurveSegment> Segments;
        Bounds CenterlineBounds;
        GeometryIssue Issue = GeometryIssue::None;
        bool Reconstructed = false;
    };
    enum Source : uint8_t { Indexed = 1, OffGrid = 2, IndexedSpline = 4 };
    struct Record
    {
        uint64_t Generation = 0;
        int32_t Index = 0, Serial = 0;
        uint32_t BuildingId = 0;
        int32_t DefinitionIndex = 0;
        uint32_t DefinitionNumber = 0;
        int32_t DefinitionObjectIndex = 0; // World-scoped metadata identity; no UObject pointer.
        uint8_t Category = 0xFF, Sources = 0;
        bool TransformValid = false, DefinitionValid = false, HasSpline = false;
        std::array<double, 3> Position = {}, Scale = {};
        std::array<double, 4> Rotation = {};
        std::array<float, 4> Tint = { 1, 1, 1, 1 };
        Bounds Extent;
        Bounds SourceFootprint; // Transformed targeting proxy, separate from admission fallback.
        LocalBounds LocalFootprint;
        std::shared_ptr<const DefinitionGeometry> DefinitionShape;
        std::shared_ptr<const SplineGeometry> Curve;
        double SplineInflation = 0; // Includes named width/cap fallback; not a measured half-width.
        GeometryIssue GeometryStatus = GeometryIssue::MissingSource;
        bool UsesSafetyAllowance = true;
        bool InvalidFootprintSource = false;
        bool UnboundedGeometry = false; // Unresolved spline: explicitly admit, never silently lose crossings.
        Coverage BoundsKind = Coverage::Unresolved;
        double RefreshedAtSeconds = 0; // steady_clock; dynamic data/bounds sample time.
    };
    struct Snapshot
    {
        uint64_t Generation = 0;
        Bounds Region;
        bool Complete = false; // Identity/cache completeness; NOT proof of exact visual geometry.
        double PublishedAtSeconds = 0;
        std::vector<Record> Records;
    };
    bool Initialize(IPluginSelf* self);
    void Reset();
    void OnExperienceLoadComplete();
    void SetViewport(const MiniMapMap::Transform& transform);
    std::shared_ptr<const Snapshot> GetSnapshot();
#if defined(MINIMAP_STAGE_R2_SMELTER_CAPTURE)
    // Caller must be on the game thread. Borrowing ends when visitor returns.
    // Revalidates the Mass identity; does not resolve the copied UObject index.
    enum class DiagnosticVisitStatus { Visited,VisitorRejected,CollectorUnavailable,WorldGenerationChanged,InvalidCopiedDefinition,StaleEntity,ParametersUnavailable };
    DiagnosticVisitStatus VisitDiagnosticDefinition(const Record&,bool(*visitor)(const SDK::UObject*,void*),void*);
#endif
    void Shutdown();
    // Diagnostic sink only. Never used to populate production records/caches.
    bool BeginComparison(const SDK::UWorld* world, const void* manager);
    void ObserveBaseline(const MiniMapBuildingInventory::Record& record);
    void EndComparison(size_t matching, double queryMs, double referenceMs);
}
#endif
