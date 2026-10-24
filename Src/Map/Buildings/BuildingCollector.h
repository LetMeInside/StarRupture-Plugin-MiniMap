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

namespace SDK { class UObject; }
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
        int32_t DefinitionIndex = 0;
        uint32_t DefinitionNumber = 0;
        // Copied definition names/index are used only by the existing private
        // capture diagnostics, never as native lifetime certificates.
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
        double PublishedAtSeconds = 0;
        std::vector<Record> Records;
    };
    bool Initialize(IPluginSelf* self);
    void Reset();
    void OnExperienceLoadComplete();
    void SetViewport(const MiniMapMap::Transform& transform);
    std::shared_ptr<const Snapshot> GetSnapshot();
#if defined(MINIMAP_STAGE_R2_BUILDING_CAPTURE)
    // Caller must be on the game thread. Borrowing ends when visitor returns.
    // Revalidates the Mass identity; does not resolve the copied UObject index.
    enum class DiagnosticVisitStatus { Visited,VisitorRejected,CollectorUnavailable,WorldGenerationChanged,InvalidCopiedDefinition,StaleEntity,ParametersUnavailable };
    DiagnosticVisitStatus VisitDiagnosticDefinition(const Record&,bool(*visitor)(const SDK::UObject*,void*),void*);
#endif
    void Shutdown();
}
#endif
